// SSoA LEVEL 3: MANIFEST SHARD
// FILE: crates/skr8tr-agent/src/main.rs
// MISSION: CLI entry point for the Skr8tr autonomous healing agent.
//
// Usage:
//   skr8tr-agent ingest [--src /home/sbaker/skr8tr] [--index ./vault/skr8tr-index]
//       Walk Skr8tr source tree, chunk at function boundaries, embed via
//       gte-large-en-v1.5, store in usearch HNSW. Run once before watch.
//
//   skr8tr-agent watch [--conductor 127.0.0.1] [--interval-s 30] [--index ...]
//       Poll the Conductor and Tower for mesh state every N seconds.
//       On any anomaly (node lost, replica drop, high CPU, stuck replica):
//         1. Build a RAG query from the event context
//         2. Retrieve relevant Skr8tr source code chunks from HNSW
//         3. Send retrieved context + event description to Mistral-Nemo
//         4. Print the recommendation to stdout
//
//   skr8tr-agent ask "<question>" [--index ...]
//       Ad-hoc RAG query against the Skr8tr codebase.
//       Example: skr8tr-agent ask "how does the Conductor handle dead nodes?"

mod autofix;
mod events;
mod ingest;
mod nlcmd;
mod reasoner;
mod watcher;

use clap::{Parser, Subcommand};
use std::path::PathBuf;
use tracing_subscriber::EnvFilter;

// ---------------------------------------------------------------------------
// CLI definition
// ---------------------------------------------------------------------------

#[derive(Parser)]
#[command(
    name    = "skr8tr-agent",
    version = "0.1.0",
    about   = "Skr8tr Autonomous Healing Agent — RAG-powered mesh intelligence",
    long_about = "Watches the Skr8tr mesh for anomalies and generates recommendations\n\
                  grounded in the actual Skr8tr source code via gte-large-en-v1.5 + Mistral-Nemo."
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Ingest the Skr8tr source tree into the vector index
    Ingest(ingest::IngestArgs),

    /// Watch the Skr8tr mesh and generate recommendations on anomalies
    Watch(watcher::WatchArgs),

    /// Ad-hoc RAG query against the Skr8tr codebase
    Ask {
        /// Your question about Skr8tr internals
        question: String,

        /// HNSW index directory (must be pre-built via `ingest`)
        #[arg(long, default_value = "/home/sbaker/RusticAgentic/vault/skr8tr-index")]
        index: PathBuf,

        /// Number of source chunks to retrieve
        #[arg(long, default_value_t = 5)]
        top_k: usize,
    },

    /// Translate a natural-language instruction into a skr8tr command and run it
    Cmd(nlcmd::CmdArgs),
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Init tracing — INFO by default, override with RUST_LOG env var
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("info"))
        )
        .with_target(false)
        .compact()
        .init();

    let cli = Cli::parse();

    match cli.command {
        Commands::Ingest(args) => {
            ingest::run(args).await?;
        }

        Commands::Watch(args) => {
            cmd_watch(args).await?;
        }

        Commands::Ask { question, index, top_k } => {
            cmd_ask(&question, &index, top_k).await?;
        }

        Commands::Cmd(args) => {
            nlcmd::run(args).await?;
        }
    }

    Ok(())
}

// ---------------------------------------------------------------------------
// watch — poll loop
// ---------------------------------------------------------------------------

async fn cmd_watch(args: watcher::WatchArgs) -> anyhow::Result<()> {
    use tokio::time::{sleep, Duration};

    tracing::info!("=== Skr8tr Agent: Watch Mode ===");
    tracing::info!("Conductor: {}:{}", args.conductor, 7771);
    tracing::info!("Tower:     {}:{}", args.tower,     7772);
    tracing::info!("Interval:  {}s", args.interval_s);
    tracing::info!("Index:     {}", args.index.display());

    let mut reasoner = reasoner::Reasoner::load(&args.index)?;

    let mut state = watcher::MeshSnapshot::default();
    let interval  = Duration::from_secs(args.interval_s);

    // First poll — establish baseline (events from first diff are noise-filtered)
    let (baseline, _) = watcher::poll(&args.conductor, &args.tower, &state);
    state = baseline;
    log_snapshot(&state);
    tracing::info!("Baseline established — watching for anomalies...\n");

    loop {
        sleep(interval).await;

        let (next_state, events) = watcher::poll(&args.conductor, &args.tower, &state);

        if events.is_empty() {
            tracing::debug!("Poll: no anomalies ({} nodes, {} replicas)",
                next_state.nodes.len(), next_state.placements.len());
        } else {
            tracing::info!("Poll: {} anomal{} detected",
                events.len(),
                if events.len() == 1 { "y" } else { "ies" });

            for event in &events {
                tracing::warn!("{event}");

                match reasoner.analyze(event).await {
                    Ok(rec) => rec.display(),
                    Err(e)  => tracing::error!("Reasoner failed for {}: {e}", event.tag()),
                }

                if args.auto_fix {
                    let result = autofix::apply(event, &args.skr8tr_bin);
                    result.log(event.tag());
                }
            }
        }

        state = next_state;
    }
}

// ---------------------------------------------------------------------------
// ask — ad-hoc query
// ---------------------------------------------------------------------------

async fn cmd_ask(question: &str, index: &PathBuf, top_k: usize) -> anyhow::Result<()> {
    use ra_rag::{pipeline::{Pipeline, PipelineConfig}, types::Query};

    tracing::info!("=== Skr8tr Agent: Ask Mode ===");
    tracing::info!("Question: {question}");

    let cfg = PipelineConfig {
        onnx_model_path: PathBuf::from(
            "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/model.onnx"
        ),
        tokenizer_path:  PathBuf::from(
            "/home/sbaker/RusticAgentic/models/gte-large-en-v1.5/tokenizer.json"
        ),
        index_dir:       index.clone(),
        llm_model:       "mistral-nemo".to_string(),
    };

    let mut pipeline = Pipeline::load(&cfg)?;

    let q = Query::new(question).with_top_k(top_k);
    let resp = pipeline.query(&q).await?;

    let sep = "─".repeat(72);
    println!("\n{sep}");
    println!("Q: {question}");
    println!("{sep}");
    println!("{}", resp.answer.trim());

    if !resp.sources.is_empty() {
        println!("\nSource chunks:");
        for s in &resp.sources {
            println!("  [{:.2}] {}:{} — {}",
                s.score,
                s.document.metadata.source,
                s.document.metadata.chunk_index,
                s.document.content.lines().next().unwrap_or("").trim(),
            );
        }
    }
    println!("({} ms | {} chunks searched)", resp.latency_ms, pipeline.index_size());
    println!("{sep}\n");

    Ok(())
}

// ---------------------------------------------------------------------------
// Pretty-print mesh state snapshot
// ---------------------------------------------------------------------------

fn log_snapshot(s: &watcher::MeshSnapshot) {
    tracing::info!("Mesh state: {} node(s), {} replica(s)",
        s.nodes.len(), s.placements.len());

    for n in &s.nodes {
        tracing::info!("  node {} ({}) cpu={}% ram={}MB",
            &n.node_id[..8], n.ip, n.cpu_pct, n.ram_mb);
    }
    for p in &s.placements {
        let pid_str = if p.pid == 0 { "pending".to_string() } else { p.pid.to_string() };
        tracing::info!("  app={} node={} pid={}", p.app, &p.node_id[..8], pid_str);
    }
}
