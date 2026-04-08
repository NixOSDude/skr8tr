// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/pipeline.rs
// MISSION: RAG pipeline orchestrator — single entry point for the gateway.
//          Owns Embedder + Retriever + Generator.
//
//          Two query modes:
//            query()             — General RAG: embed → retrieve → generate (factual)
//            query_lambassist()  — LambdaC mode: embed → retrieve(lambdac docs) → generate_lambdac
//
// PHASE 2 NOTE: In Phase 2 the Embedder's embed() call is transparently
//               replaced by a fabric CMD_EMBED dispatch to lvm_nodes.
//               This file does not change — only embedder.rs changes.

use std::{
    path::{Path, PathBuf},
    time::Instant,
};

use crate::{
    embedder::{Embedder, EMBEDDING_DIM},
    generator::Generator,
    retriever::Retriever,
    types::{Document, Embedding, Query, RagError, RagResponse, RagResult},
};

// ---------------------------------------------------------------------------
// LambAssist response — extends RagResponse with extracted LambdaC code
// ---------------------------------------------------------------------------

/// Response from a LambAssist (code generation) query.
pub struct LambassistResponse {
    pub query_id:   uuid::Uuid,
    /// Full natural-language response including explanation text.
    pub answer:     String,
    /// Extracted LambdaC code block (empty string if none generated).
    pub lambdac:    String,
    pub model:      String,
    pub latency_ms: u64,
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

pub struct PipelineConfig {
    pub onnx_model_path: PathBuf,
    pub tokenizer_path:  PathBuf,
    pub index_dir:       PathBuf,
    pub llm_model:       String,
}

impl PipelineConfig {
    pub fn from_env() -> Self {
        Self {
            onnx_model_path: PathBuf::from(
                std::env::var("RA_ONNX_MODEL_PATH")
                    .unwrap_or_else(|_| "models/gte-large-en-v1.5/model.onnx".to_string()),
            ),
            tokenizer_path: PathBuf::from(
                std::env::var("RA_TOKENIZER_PATH")
                    .unwrap_or_else(|_| "models/gte-large-en-v1.5/tokenizer.json".to_string()),
            ),
            index_dir: PathBuf::from(
                std::env::var("RA_INDEX_DIR").unwrap_or_else(|_| "vault/index".to_string()),
            ),
            llm_model: std::env::var("RA_LLM_MODEL")
                .unwrap_or_else(|_| Generator::DEFAULT_MODEL.to_string()),
        }
    }
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

pub struct Pipeline {
    embedder:  Embedder,
    retriever: Retriever,
    generator: Generator,
}

impl Pipeline {
    pub fn load(cfg: &PipelineConfig) -> RagResult<Self> {
        tracing::info!("Loading RusticAgentic RAG pipeline...");

        let embedder = Embedder::load(&cfg.onnx_model_path, &cfg.tokenizer_path)?;
        tracing::info!("  [1/3] Embedder ready (gte-large-en-v1.5, CUDA EP)");

        let index_path = cfg.index_dir.join("index.usearch");
        let retriever = if index_path.exists() {
            let mut r = Retriever::load_index(&index_path, EMBEDDING_DIM)?;
            let doc_store_path = cfg.index_dir.join("doc_store.json");
            if doc_store_path.exists() {
                let raw = std::fs::read_to_string(&doc_store_path)?;
                let docs: Vec<(u64, crate::types::Document)> =
                    serde_json::from_str(&raw).map_err(|e| {
                        RagError::VaultError(format!("doc_store.json parse failed: {e}"))
                    })?;
                r.restore_doc_store(docs);
            }
            tracing::info!("  [2/3] Retriever ready ({} docs indexed)", r.len());
            r
        } else {
            tracing::warn!(
                "  [2/3] No index found at {} — starting with empty retriever.",
                index_path.display()
            );
            Retriever::new(EMBEDDING_DIM)?
        };

        let generator = Generator::new(&cfg.llm_model);
        tracing::info!("  [3/3] Generator ready ({})", cfg.llm_model);

        tracing::info!("Pipeline loaded.");
        Ok(Self { embedder, retriever, generator })
    }

    // -----------------------------------------------------------------------
    // General RAG Query
    // -----------------------------------------------------------------------

    pub async fn query(&mut self, query: &Query) -> RagResult<RagResponse> {
        let t_start = Instant::now();

        let query_embedding = self.embedder.embed(&query.text)?;
        let sources = self.retriever.search(&query_embedding, query.top_k)?;

        if sources.is_empty() {
            return Ok(RagResponse {
                query_id:   query.id,
                answer:     "No relevant documents found in the vault.".to_string(),
                sources:    vec![],
                model:      self.generator.model().to_string(),
                latency_ms: t_start.elapsed().as_millis() as u64,
            });
        }

        let answer = self.generator.generate(&query.text, &sources).await?;

        Ok(RagResponse {
            query_id:   query.id,
            answer,
            sources,
            model:      self.generator.model().to_string(),
            latency_ms: t_start.elapsed().as_millis() as u64,
        })
    }

    // -----------------------------------------------------------------------
    // LambAssist Query — natural language → LambdaC code generation
    // -----------------------------------------------------------------------

    /// Execute a LambAssist query: embed the prompt, retrieve LambdaC docs,
    /// generate a code response, extract any ```lambdac blocks.
    pub async fn query_lambassist(
        &mut self,
        prompt: &str,
        schema: &str,
        top_k: usize,
    ) -> RagResult<LambassistResponse> {
        let t_start = Instant::now();
        let id = uuid::Uuid::new_v4();

        // Embed prompt to find relevant LambdaC documentation
        let query_embedding = self.embedder.embed(prompt)?;
        let sources = self.retriever.search(&query_embedding, top_k.clamp(3, 10))?;

        // Generate LambdaC-specialized response
        let answer = if sources.is_empty() {
            // No docs indexed yet — still attempt generation with schema alone
            self.generator
                .generate_lambdac(prompt, schema, &[])
                .await?
        } else {
            self.generator
                .generate_lambdac(prompt, schema, &sources)
                .await?
        };

        // Extract ```lambdac code block from the answer
        let lambdac = extract_lambdac_block(&answer);

        Ok(LambassistResponse {
            query_id:   id,
            answer,
            lambdac,
            model:      self.generator.model().to_string(),
            latency_ms: t_start.elapsed().as_millis() as u64,
        })
    }

    // -----------------------------------------------------------------------
    // Ingest
    // -----------------------------------------------------------------------

    pub fn add_document(&mut self, doc: Document, embedding: &Embedding) -> RagResult<()> {
        self.retriever.add(doc, embedding)
    }

    pub fn ingest(&mut self, doc: Document) -> RagResult<()> {
        let embedding = self.embedder.embed(&doc.content)?;
        self.retriever.add(doc, &embedding)
    }

    pub fn save(&self, index_dir: &Path) -> RagResult<()> {
        std::fs::create_dir_all(index_dir)?;

        // Save HNSW graph
        self.retriever.save_index(&index_dir.join("index.usearch"))?;

        // Save doc_store — required for metadata retrieval after restart
        let snapshot = self.retriever.snapshot_doc_store();
        let json = serde_json::to_string(&snapshot)
            .map_err(|e| RagError::VaultError(format!("doc_store serialize failed: {e}")))?;
        std::fs::write(index_dir.join("doc_store.json"), json)?;

        tracing::info!(
            path = %index_dir.display(),
            docs = self.retriever.len(),
            "Pipeline saved (index + doc_store)"
        );
        Ok(())
    }

    pub fn embedder(&self) -> &Embedder   { &self.embedder }
    pub fn retriever(&self) -> &Retriever { &self.retriever }
    pub fn index_size(&self) -> usize     { self.retriever.len() }
}

// ---------------------------------------------------------------------------
// LambdaC code block extractor
// Finds the first ```lambdac ... ``` block in an LLM response.
// ---------------------------------------------------------------------------

fn extract_lambdac_block(text: &str) -> String {
    // Try ```lambdac first (explicit language marker)
    if let Some(start) = text.find("```lambdac") {
        let after_marker = start + "```lambdac".len();
        // Skip a leading newline if present
        let content_start = if text.as_bytes().get(after_marker) == Some(&b'\n') {
            after_marker + 1
        } else {
            after_marker
        };
        if let Some(end) = text[content_start..].find("```") {
            return text[content_start..content_start + end].trim().to_string();
        }
    }

    // Fall back to plain ``` block (less specific)
    if let Some(start) = text.find("```") {
        let after_marker = start + 3;
        // Skip optional language hint line
        let content_start = if let Some(nl) = text[after_marker..].find('\n') {
            after_marker + nl + 1
        } else {
            return String::new();
        };
        if let Some(end) = text[content_start..].find("```") {
            return text[content_start..content_start + end].trim().to_string();
        }
    }

    String::new()
}
