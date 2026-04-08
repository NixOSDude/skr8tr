// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/skr8tr-agent/src/ingest.rs
// MISSION: Walk the Skr8tr source tree, chunk each file at function boundaries,
//          embed via gte-large-en-v1.5, and store in the usearch HNSW index.
//          Run once before `watch` mode — or re-run after code changes.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use ra_rag::{
    chunker::chunk_file,
    pipeline::{Pipeline, PipelineConfig},
    types::{Document, DocumentMetadata, ShardId},
};
use walkdir::WalkDir;

// ---------------------------------------------------------------------------
// File extensions we ingest from the Skr8tr source tree
// ---------------------------------------------------------------------------

const INGEST_EXTENSIONS: &[&str] = &["c", "h", "rs", "md", "toml", "skr8tr"];

// ---------------------------------------------------------------------------
// CLI args for this sub-command
// ---------------------------------------------------------------------------

#[derive(Debug, clap::Args)]
pub struct IngestArgs {
    /// Root of the Skr8tr source tree to ingest (e.g. /home/sbaker/skr8tr)
    #[arg(long, default_value = "/home/sbaker/skr8tr")]
    pub src: PathBuf,

    /// Directory to write the HNSW index and doc_store into
    #[arg(long, default_value = "/home/sbaker/RusticAgentic/vault/skr8tr-index")]
    pub index: PathBuf,

    /// Maximum characters per chunk (default: 1800 — fits in gte-large token limit)
    #[arg(long, default_value_t = 1800)]
    pub max_chars: usize,
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

pub async fn run(args: IngestArgs) -> Result<()> {
    tracing::info!("=== Skr8tr Agent: Ingest Mode ===");
    tracing::info!("Source:  {}", args.src.display());
    tracing::info!("Index:   {}", args.index.display());

    // Discover all eligible source files
    let files = collect_files(&args.src);
    tracing::info!("Found {} eligible source files", files.len());

    // Load RAG pipeline (embedder + fresh retriever)
    let cfg = PipelineConfig {
        onnx_model_path: PathBuf::from("/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/model.onnx"),
        tokenizer_path:  PathBuf::from("/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/tokenizer.json"),
        index_dir:       args.index.clone(),
        llm_model:       "mistral-nemo".to_string(),
    };

    let mut pipeline = Pipeline::load(&cfg)
        .context("Failed to load RAG pipeline — check ONNX model path")?;

    // Chunk and ingest each file
    let mut total_chunks  = 0usize;
    let mut total_files   = 0usize;
    let shard_id = ShardId::new("skr8tr-codebase");

    for file_path in &files {
        let rel_path = file_path
            .strip_prefix(&args.src)
            .unwrap_or(file_path)
            .to_string_lossy()
            .to_string();

        let source = match std::fs::read_to_string(file_path) {
            Ok(s)  => s,
            Err(e) => {
                tracing::warn!("  skip {} — read error: {e}", rel_path);
                continue;
            }
        };

        let chunks = chunk_file(&source, &rel_path, args.max_chars);
        let n_chunks = chunks.len();
        total_files += 1;

        for (i, chunk) in chunks.into_iter().enumerate() {
            let meta = DocumentMetadata {
                source:       rel_path.clone(),
                shard_id:     shard_id.clone(),
                chunk_index:  i as u32,
                total_chunks: n_chunks as u32,
            };
            let doc = Document::new(chunk.content, meta);

            if let Err(e) = pipeline.ingest(doc) {
                tracing::warn!("  chunk {i} of {rel_path} failed: {e}");
            } else {
                total_chunks += 1;
            }
        }

        tracing::info!("  ingested {rel_path} ({n_chunks} chunks)");
    }

    // Persist index to disk
    std::fs::create_dir_all(&args.index)
        .with_context(|| format!("Cannot create index dir: {}", args.index.display()))?;

    pipeline.save(&args.index)
        .context("Failed to save HNSW index")?;

    tracing::info!("=== Ingest complete ===");
    tracing::info!("  files:  {}", total_files);
    tracing::info!("  chunks: {}", total_chunks);
    tracing::info!("  index:  {}", args.index.display());
    tracing::info!("Run `skr8tr-agent watch` to start monitoring the mesh.");

    Ok(())
}

// ---------------------------------------------------------------------------
// Collect eligible files — skip build artifacts, target/, dist-newstyle/
// ---------------------------------------------------------------------------

fn collect_files(root: &Path) -> Vec<PathBuf> {
    let skip_dirs = ["target", "dist-newstyle", "build", ".git", "node_modules", ".cache"];

    WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_entry(|e| {
            // Prune entire directories we don't want
            if e.file_type().is_dir() {
                let name = e.file_name().to_string_lossy();
                return !skip_dirs.iter().any(|s| *s == name.as_ref());
            }
            true
        })
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .filter(|e| {
            let ext = e.path()
                .extension()
                .and_then(|s| s.to_str())
                .unwrap_or("")
                .to_ascii_lowercase();
            INGEST_EXTENSIONS.contains(&ext.as_str())
        })
        .map(|e| e.into_path())
        .collect()
}
