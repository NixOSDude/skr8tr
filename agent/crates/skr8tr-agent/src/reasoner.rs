// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/skr8tr-agent/src/reasoner.rs
// MISSION: Map AgentEvents → RAG queries → Mistral-Nemo recommendations.
//          Uses the pre-built Skr8tr codebase HNSW index to ground answers
//          in actual source code, not hallucination.

use std::io::Write;
use std::path::PathBuf;

use anyhow::{Context, Result};
use ra_rag::{
    pipeline::{Pipeline, PipelineConfig},
    types::Query,
};

use crate::events::AgentEvent;

// ---------------------------------------------------------------------------
// Reasoner — owns the RAG pipeline for the lifetime of `watch` mode
// ---------------------------------------------------------------------------

pub struct Reasoner {
    pipeline: Pipeline,
}

impl Reasoner {
    /// Load the RAG pipeline from the pre-built Skr8tr index.
    pub fn load(index_dir: &PathBuf) -> Result<Self> {
        let cfg = PipelineConfig {
            onnx_model_path: PathBuf::from(
                "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/model.onnx"
            ),
            tokenizer_path:  PathBuf::from(
                "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/tokenizer.json"
            ),
            index_dir:       index_dir.clone(),
            llm_model:       "mistral-nemo".to_string(),
        };

        let pipeline = Pipeline::load(&cfg)
            .context("Failed to load RAG pipeline — run `skr8tr-agent ingest` first")?;

        tracing::info!(
            "Reasoner ready — {} chunks indexed",
            pipeline.index_size()
        );

        Ok(Self { pipeline })
    }

    /// Analyze an event: embed the event query, retrieve relevant Skr8tr code,
    /// generate a recommendation via Mistral-Nemo.
    pub async fn analyze(&mut self, event: &AgentEvent) -> Result<Recommendation> {
        let query_text = event.rag_query();

        tracing::debug!("RAG query: {}", query_text);

        let query = Query::new(&query_text).with_top_k(5);
        let response = self.pipeline.query(&query).await
            .context("Pipeline query failed")?;

        Ok(Recommendation {
            event_tag:   event.tag().to_string(),
            event_str:   event.to_string(),
            answer:      response.answer.clone(),
            source_refs: response.sources.iter().map(|s| {
                format!(
                    "{}:{} (score={:.2})",
                    s.document.metadata.source,
                    s.document.metadata.chunk_index,
                    s.score,
                )
            }).collect(),
            latency_ms:  response.latency_ms,
        })
    }
}

// ---------------------------------------------------------------------------
// Recommendation — the final output of the reasoner
// ---------------------------------------------------------------------------

#[derive(Debug)]
#[allow(dead_code)]   /* event_tag + log_line used by future file-logging path */
pub struct Recommendation {
    pub event_tag:   String,
    pub event_str:   String,
    pub answer:      String,
    pub source_refs: Vec<String>,
    pub latency_ms:  u64,
}

impl Recommendation {
    /// Format for human-readable terminal output.
    /// If the env var `SKRTRVIEW_PIPE` is set, also writes one line to the
    /// named pipe so Skr8trView Agent Feed receives it live.
    pub fn display(&self) {
        let sep = "─".repeat(72);
        println!("\n{sep}");
        println!("⚡ SKRTR-AGENT  {}", self.event_str);
        println!("{sep}");
        println!("{}", self.answer.trim());
        if !self.source_refs.is_empty() {
            println!("\nSource chunks used:");
            for r in &self.source_refs {
                println!("  · {r}");
            }
        }
        println!("({} ms)", self.latency_ms);
        println!("{sep}\n");

        self.pipe_emit();
    }

    /// Write a single newline-terminated frame to the Skr8trView named pipe.
    ///
    /// Format:  AGENT|<tag>|<event_str>|<condensed_answer>\n
    /// Condensed answer: first 300 chars of answer, newlines → " · "
    fn pipe_emit(&self) {
        let pipe_path = match std::env::var("SKRTRVIEW_PIPE") {
            Ok(p) if !p.is_empty() => p,
            _ => return,
        };

        let condensed: String = self.answer
            .trim()
            .lines()
            .map(|l| l.trim())
            .filter(|l| !l.is_empty())
            .collect::<Vec<_>>()
            .join(" · ");

        let truncated = if condensed.len() > 400 {
            format!("{}…", &condensed[..400])
        } else {
            condensed
        };

        let line = format!(
            "AGENT|{}|{}|{}\n",
            self.event_tag,
            self.event_str,
            truncated
        );

        /* Open non-blocking so we don't block the agent if no reader */
        match std::fs::OpenOptions::new()
            .write(true)
            .open(&pipe_path)
        {
            Ok(mut f) => {
                let _ = f.write_all(line.as_bytes());
            }
            Err(e) => {
                tracing::debug!("pipe_emit: cannot open {pipe_path}: {e}");
            }
        }
    }

    /// Format as a compact one-liner for log files.
    #[allow(dead_code)]
    pub fn log_line(&self) -> String {
        let first_line = self.answer.lines().next().unwrap_or("").trim();
        format!("[{}] {} — {}", self.event_tag, self.event_str, first_line)
    }
}
