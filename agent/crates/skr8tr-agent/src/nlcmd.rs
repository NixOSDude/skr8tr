// SSoA LEVEL 3: MANIFEST SHARD
// FILE: crates/skr8tr-agent/src/nlcmd.rs
// MISSION: Natural language → skr8tr CLI command translation + execution.
//          Uses the RAG pipeline to map a plain-English instruction to the
//          correct skr8tr command, executes it, and prints the full output.

use std::path::PathBuf;
use std::process::Command;

use anyhow::{bail, Result};
use ra_rag::{
    pipeline::{Pipeline, PipelineConfig},
    types::Query,
};

// ---------------------------------------------------------------------------
// CLI args
// ---------------------------------------------------------------------------

#[derive(Debug, clap::Args)]
pub struct CmdArgs {
    /// Natural language instruction — e.g. "show me all running apps"
    pub instruction: String,

    /// HNSW index directory (must be pre-built via `ingest`)
    #[arg(long, default_value = "/home/sbaker/RusticAgentic/vault/skr8tr-index")]
    pub index: PathBuf,

    /// Path to the skr8tr CLI binary
    #[arg(long, default_value = "/home/sbaker/skr8tr/cli/target/release/skr8tr")]
    pub skr8tr_bin: PathBuf,
}

// ---------------------------------------------------------------------------
// Command vocabulary — embedded so the LLM has an exact reference.
// The retrieval step will add grounding from Skr8tr source chunks on top.
// ---------------------------------------------------------------------------

const COMMAND_REFERENCE: &str = "\
Available skr8tr CLI commands (respond with ONLY the exact command, nothing else):

  skr8tr nodes                     — List all live mesh nodes (id, ip, cpu%, ram)
  skr8tr list                      — List all running workload replicas and their PIDs
  skr8tr status                    — Full cluster status: nodes + replica placements
  skr8tr ping                      — Ping the Conductor and Tower; check connectivity
  skr8tr up <manifest.skr8tr>      — Deploy a workload from a .skr8tr manifest file
  skr8tr down <app-name>           — Evict and stop a running workload by name
  skr8tr logs <app-name>           — Fetch captured stdout/stderr from a running workload
  skr8tr lookup <service-name>     — Look up a service address in the Tower registry
  skr8tr rollout <manifest.skr8tr> — Rolling-update a deployed workload with zero downtime

Rules:
- Output ONLY the exact command to run — no explanation, no markdown, no punctuation.
- Use the shortest correct command for the instruction.
- If the instruction cannot be mapped, output exactly: UNKNOWN";

// ---------------------------------------------------------------------------
// run — translate, execute, display
// ---------------------------------------------------------------------------

pub async fn run(args: CmdArgs) -> Result<()> {
    let sep = "─".repeat(72);

    // Load RAG pipeline
    let cfg = PipelineConfig {
        onnx_model_path: PathBuf::from(
            "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/model.onnx"
        ),
        tokenizer_path: PathBuf::from(
            "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/tokenizer.json"
        ),
        index_dir:  args.index.clone(),
        llm_model:  "mistral-nemo".to_string(),
    };

    let mut pipeline = Pipeline::load(&cfg)?;

    // Build the translation prompt.
    // Top-k=3 pulls relevant source chunks about command handling for grounding.
    let query_text = format!(
        "{}\n\nInstruction: \"{}\"\n\nCommand:",
        COMMAND_REFERENCE,
        args.instruction.trim()
    );

    println!("\n{sep}");
    println!("  NL → CMD  \"{}\"", args.instruction.trim());
    println!("{sep}");

    let q     = Query::new(&query_text).with_top_k(3);
    let resp  = pipeline.query(&q).await?;

    // Extract the first non-empty line — that is the command
    let command_line = resp
        .answer
        .trim()
        .lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .unwrap_or("")
        .to_string();

    if command_line.is_empty() || command_line == "UNKNOWN" {
        bail!(
            "Could not map \"{instruction}\" to a skr8tr command — try rephrasing.",
            instruction = args.instruction.trim()
        );
    }

    if !command_line.starts_with("skr8tr") {
        bail!(
            "Unexpected translation output: {command_line:?}\n\
             Expected output starting with 'skr8tr'."
        );
    }

    // Display the translated command before running it
    println!("  $ {command_line}");
    println!("{sep}");

    // Split into argv — skip the leading "skr8tr" token (we call the binary directly)
    let argv: Vec<&str> = command_line
        .split_whitespace()
        .skip(1)
        .collect();

    let output = Command::new(&args.skr8tr_bin)
        .args(&argv)
        .output()?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    if !stdout.is_empty() {
        print!("{stdout}");
    }
    if !stderr.is_empty() {
        eprint!("{stderr}");
    }

    println!("{sep}");
    println!(
        "  exit {}  ({} ms)",
        output.status.code().unwrap_or(-1),
        resp.latency_ms
    );
    println!("{sep}\n");

    Ok(())
}
