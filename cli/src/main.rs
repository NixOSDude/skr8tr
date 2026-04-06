// skr8tr CLI — The Deck
// Skr8tr Sovereign Workload Orchestrator
//
// SSoA LEVEL 3 — Manifest Shard (operator-facing CLI)
//
// Commands:
//   skr8tr up   <manifest.skr8tr>   — submit workload to Conductor
//   skr8tr down <app_name>          — evict all replicas of a workload
//   skr8tr status                   — show live nodes and placed workloads
//   skr8tr nodes                    — show live node metrics
//   skr8tr list                     — show all running workloads
//   skr8tr lookup <service>         — resolve a service name via the Tower
//   skr8tr ping                     — ping Conductor and Tower
//
// All commands speak UDP to the Conductor (7771), Tower (7772), or Node (7775).
// No persistent state. No config file required for local use.

use clap::{Parser, Subcommand};
use std::net::UdpSocket;
use std::time::Duration;

// -------------------------------------------------------------------------
// CLI definition
// -------------------------------------------------------------------------

#[derive(Parser)]
#[command(
    name    = "skr8tr",
    version = "0.1.0",
    about   = "Skr8tr — Sovereign Workload Orchestrator",
    long_about = "Deploy, scale, and manage workloads across a bare-metal mesh.\n\
                  No Docker. No YAML. No Kubernetes. Just binaries on machines."
)]
struct Cli {
    /// Conductor host (default: 127.0.0.1)
    #[arg(long, default_value = "127.0.0.1", global = true)]
    conductor: String,

    /// Tower host (default: 127.0.0.1)
    #[arg(long, default_value = "127.0.0.1", global = true)]
    tower: String,

    /// UDP timeout in milliseconds
    #[arg(long, default_value_t = 3000, global = true)]
    timeout_ms: u64,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Submit a .skr8tr manifest to the Conductor for scheduling
    Up {
        /// Path to the .skr8tr manifest file
        manifest: String,
    },
    /// Evict all replicas of a workload from the mesh
    Down {
        /// App name as declared in the manifest
        app: String,
    },
    /// Show live nodes and all running workload placements
    Status,
    /// Show live node metrics (cpu%, ram_free_mb)
    Nodes,
    /// List all running workload placements
    List,
    /// Resolve a service name to ip:port via the Tower
    Lookup {
        /// Service name to resolve
        service: String,
    },
    /// Ping the Conductor and Tower to verify they are reachable
    Ping,
    /// Tail the stdout/stderr ring buffer of a running app
    Logs {
        /// App name to fetch logs from
        app: String,
        /// Node host to query directly (auto-resolved from Conductor if omitted)
        #[arg(long)]
        node: Option<String>,
    },
}

// -------------------------------------------------------------------------
// UDP messenger — send a command, wait for one response
// -------------------------------------------------------------------------

fn udp_send(host: &str, port: u16, msg: &str, timeout_ms: u64)
    -> Result<String, String>
{
    let addr = format!("{}:{}", host, port);
    let sock = UdpSocket::bind("0.0.0.0:0")
        .map_err(|e| format!("bind error: {e}"))?;

    sock.set_read_timeout(Some(Duration::from_millis(timeout_ms)))
        .map_err(|e| format!("timeout error: {e}"))?;

    sock.send_to(msg.as_bytes(), &addr)
        .map_err(|e| format!("send to {addr} failed: {e}"))?;

    let mut buf = [0u8; 8192];
    let (n, _) = sock.recv_from(&mut buf)
        .map_err(|e| format!("no response from {addr}: {e}"))?;

    Ok(String::from_utf8_lossy(&buf[..n]).trim().to_string())
}

// -------------------------------------------------------------------------
// Response formatters
// -------------------------------------------------------------------------

/// Pretty-print OK|NODES|n|id:ip:cpu:ram,...
fn print_nodes(resp: &str) {
    if let Some(body) = resp.strip_prefix("OK|NODES|") {
        let parts: Vec<&str> = body.splitn(2, '|').collect();
        let count = parts[0];
        println!("  {count} live node(s)\n");
        if parts.len() > 1 && !parts[1].is_empty() {
            println!("  {:<34}  {:<16}  {:>5}  {:>10}",
                     "NODE ID", "IP", "CPU%", "RAM FREE");
            println!("  {}", "-".repeat(72));
            for entry in parts[1].split(',') {
                let f: Vec<&str> = entry.splitn(4, ':').collect();
                if f.len() == 4 {
                    println!("  {:<34}  {:<16}  {:>4}%  {:>7} MB",
                             f[0], f[1], f[2], f[3]);
                }
            }
        }
    } else {
        println!("  {resp}");
    }
}

/// Pretty-print OK|LIST|n|app:node:pid,...
fn print_list(resp: &str) {
    if let Some(body) = resp.strip_prefix("OK|LIST|") {
        let parts: Vec<&str> = body.splitn(2, '|').collect();
        let count = parts[0];
        println!("  {count} replica(s) running\n");
        if parts.len() > 1 && !parts[1].is_empty() {
            println!("  {:<32}  {:<34}  {:>8}", "APP", "NODE ID", "PID");
            println!("  {}", "-".repeat(78));
            for entry in parts[1].split(',') {
                let f: Vec<&str> = entry.splitn(3, ':').collect();
                if f.len() == 3 {
                    let pid = if f[2] == "0" { "pending".to_string() }
                              else { f[2].to_string() };
                    println!("  {:<32}  {:<34}  {:>8}", f[0], f[1], pid);
                }
            }
        }
    } else {
        println!("  {resp}");
    }
}

// -------------------------------------------------------------------------
// Log helpers — two-step node resolution
// -------------------------------------------------------------------------

/// Parse OK|LIST|n|app:node:pid,... → find node_id for app
fn find_node_for_app(resp: &str, app: &str) -> Option<String> {
    let body = resp.strip_prefix("OK|LIST|")?;
    let parts: Vec<&str> = body.splitn(2, '|').collect();
    if parts.len() < 2 { return None; }
    for entry in parts[1].split(',') {
        let f: Vec<&str> = entry.splitn(3, ':').collect();
        if f.len() == 3 && f[0] == app {
            return Some(f[1].to_string());
        }
    }
    None
}

/// Parse OK|NODES|n|id:ip:cpu:ram,... → find IP for node_id
fn find_ip_for_node(resp: &str, node_id: &str) -> Option<String> {
    let body = resp.strip_prefix("OK|NODES|")?;
    let parts: Vec<&str> = body.splitn(2, '|').collect();
    if parts.len() < 2 { return None; }
    for entry in parts[1].split(',') {
        let f: Vec<&str> = entry.splitn(4, ':').collect();
        if f.len() == 4 && f[0] == node_id {
            return Some(f[1].to_string());
        }
    }
    None
}

// -------------------------------------------------------------------------
// Command handlers
// -------------------------------------------------------------------------

fn cmd_up(cli: &Cli, manifest: &str) {
    let path = std::fs::canonicalize(manifest)
        .unwrap_or_else(|_| std::path::PathBuf::from(manifest));
    let path_str = path.to_string_lossy();

    print!("  submitting {}... ", path_str);
    match udp_send(&cli.conductor, 7771,
                   &format!("SUBMIT|{path_str}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|SUBMITTED|") => {
            let parts: Vec<&str> = r.splitn(4, '|').collect();
            let app  = parts.get(2).unwrap_or(&"?");
            let node = parts.get(3).unwrap_or(&"?");
            println!("ok");
            println!("  app    {app}");
            println!("  node   {node}");
        }
        Ok(r)  => println!("error\n  {r}"),
        Err(e) => println!("error\n  {e}"),
    }
}

fn cmd_down(cli: &Cli, app: &str) {
    print!("  evicting {}... ", app);
    match udp_send(&cli.conductor, 7771,
                   &format!("EVICT|{app}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|EVICTED|") => println!("ok"),
        Ok(r)  => println!("error\n  {r}"),
        Err(e) => println!("error\n  {e}"),
    }
}

fn cmd_status(cli: &Cli) {
    println!("nodes:");
    match udp_send(&cli.conductor, 7771, "NODES", cli.timeout_ms) {
        Ok(r)  => print_nodes(&r),
        Err(e) => println!("  error: {e}"),
    }
    println!("\nworkloads:");
    match udp_send(&cli.conductor, 7771, "LIST", cli.timeout_ms) {
        Ok(r)  => print_list(&r),
        Err(e) => println!("  error: {e}"),
    }
}

fn cmd_nodes(cli: &Cli) {
    match udp_send(&cli.conductor, 7771, "NODES", cli.timeout_ms) {
        Ok(r)  => print_nodes(&r),
        Err(e) => println!("  error: {e}"),
    }
}

fn cmd_list(cli: &Cli) {
    match udp_send(&cli.conductor, 7771, "LIST", cli.timeout_ms) {
        Ok(r)  => print_list(&r),
        Err(e) => println!("  error: {e}"),
    }
}

fn cmd_lookup(cli: &Cli, service: &str) {
    match udp_send(&cli.tower, 7772,
                   &format!("LOOKUP|{service}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|LOOKUP|") => {
            let parts: Vec<&str> = r.splitn(5, '|').collect();
            let ip   = parts.get(3).unwrap_or(&"?");
            let port = parts.get(4).unwrap_or(&"?");
            println!("  {service}  →  {ip}:{port}");
        }
        Ok(r) if r.starts_with("ERR|NOT_FOUND") => {
            println!("  service '{service}' not found in registry");
            std::process::exit(1);
        }
        Ok(r)  => println!("  {r}"),
        Err(e) => {
            println!("  error: {e}");
            std::process::exit(1);
        }
    }
}

fn cmd_logs(cli: &Cli, app: &str, node_override: Option<&str>) {
    let node_host = if let Some(h) = node_override {
        h.to_string()
    } else {
        /* Step 1: ask Conductor for placement list */
        let list_resp = match udp_send(&cli.conductor, 7771, "LIST", cli.timeout_ms) {
            Ok(r)  => r,
            Err(e) => { println!("  error querying conductor: {e}"); return; }
        };
        let node_id = match find_node_for_app(&list_resp, app) {
            Some(id) => id,
            None => {
                println!("  app '{app}' has no active placements");
                return;
            }
        };
        /* Step 2: resolve node_id → IP */
        let nodes_resp = match udp_send(&cli.conductor, 7771, "NODES", cli.timeout_ms) {
            Ok(r)  => r,
            Err(e) => { println!("  error querying conductor: {e}"); return; }
        };
        match find_ip_for_node(&nodes_resp, &node_id) {
            Some(ip) => ip,
            None => {
                println!("  node {node_id} not found in live node table");
                return;
            }
        }
    };

    /* Step 3: fetch log ring from node (node command port 7775) */
    match udp_send(&node_host, 7775, &format!("LOGS|{app}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|LOGS|") => {
            /* Format: OK|LOGS|<name>|<count>|<lines...> */
            let parts: Vec<&str> = r.splitn(5, '|').collect();
            let count = parts.get(3).copied().unwrap_or("0");
            let body  = parts.get(4).copied().unwrap_or("");
            println!("  logs for '{app}' on {node_host} ({count} lines captured):");
            println!("  {}", "-".repeat(64));
            for line in body.split('\n') {
                if !line.is_empty() {
                    println!("  {line}");
                }
            }
        }
        Ok(r) if r.starts_with("ERR|") => {
            println!("  {r}");
            std::process::exit(1);
        }
        Ok(r)  => println!("  {r}"),
        Err(e) => {
            println!("  error querying node {node_host}: {e}");
            std::process::exit(1);
        }
    }
}

fn cmd_ping(cli: &Cli) {
    print!("  conductor ({}:7771)... ", cli.conductor);
    match udp_send(&cli.conductor, 7771, "PING", cli.timeout_ms) {
        Ok(r) if r.contains("PONG") => println!("ok"),
        Ok(r)  => println!("unexpected: {r}"),
        Err(e) => println!("unreachable: {e}"),
    }

    print!("  tower     ({}:7772)... ", cli.tower);
    match udp_send(&cli.tower, 7772, "PING", cli.timeout_ms) {
        Ok(r) if r.contains("PONG") => println!("ok"),
        Ok(r)  => println!("unexpected: {r}"),
        Err(e) => println!("unreachable: {e}"),
    }
}

// -------------------------------------------------------------------------
// Entry point
// -------------------------------------------------------------------------

fn main() {
    let cli = Cli::parse();

    match &cli.command {
        Commands::Up     { manifest } => cmd_up(&cli, manifest),
        Commands::Down   { app }      => cmd_down(&cli, app),
        Commands::Status              => cmd_status(&cli),
        Commands::Nodes               => cmd_nodes(&cli),
        Commands::List                => cmd_list(&cli),
        Commands::Lookup { service }  => cmd_lookup(&cli, service),
        Commands::Ping                => cmd_ping(&cli),
        Commands::Logs { app, node }  => cmd_logs(&cli, app, node.as_deref()),
    }
}
