// skr8tr CLI — The Deck
// Skr8tr Sovereign Workload Orchestrator
//
// SSoA LEVEL 3 — Manifest Shard (operator-facing CLI)
//
// Commands:
//   skr8tr up      <manifest.skr8tr>  — submit workload to Conductor
//   skr8tr down    <app_name>         — evict all replicas of a workload
//   skr8tr rollout <manifest.skr8tr>  — rolling zero-downtime update
//   skr8tr status                     — show live nodes and placed workloads
//   skr8tr nodes                      — show live node metrics
//   skr8tr list                       — show all running workloads
//   skr8tr lookup  <service>          — resolve a service name via the Tower
//   skr8tr logs    <app>              — fetch stdout/stderr ring buffer
//   skr8tr ping                       — ping Conductor and Tower
//
// Auth:
//   Mutating commands (up, down) are ML-DSA-65 signed when --key is provided.
//   The Conductor rejects unsigned mutations when skrtrview.pub is present.
//   Generate a keypair once with:  skrtrkey keygen
//   Then use:  skr8tr --key ~/.skr8tr/signing.sec up app.skr8tr

use clap::{Parser, Subcommand};
use std::net::UdpSocket;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

// -------------------------------------------------------------------------
// ML-DSA-65 signing via liboqs FFI
// -------------------------------------------------------------------------

/// ML-DSA-65 secret key length in bytes.
const ML_DSA_65_SK_LEN: usize = 4032;
/// ML-DSA-65 signature length in bytes.
const ML_DSA_65_SIG_LEN: usize = 3309;

#[allow(non_snake_case)]
extern "C" {
    /// Direct ML-DSA-65 sign function from liboqs.
    /// Returns 0 (OQS_SUCCESS) on success, non-zero on failure.
    fn OQS_SIG_ml_dsa_65_sign(
        sig:     *mut u8,
        siglen:  *mut usize,
        message: *const u8,
        mlen:    usize,
        sk:      *const u8,
    ) -> i32;
}

/// Sign `cmd` with the ML-DSA-65 secret key at `key_path`.
///
/// Returns the signed string: `<cmd>|<unix_ts>|<hex_sig>`
/// The Conductor verifies this before executing SUBMIT or EVICT.
fn sign_command(cmd: &str, key_path: &str) -> Result<String, String> {
    let sk_bytes = std::fs::read(key_path)
        .map_err(|e| format!("cannot read key '{}': {}", key_path, e))?;

    if sk_bytes.len() != ML_DSA_65_SK_LEN {
        return Err(format!(
            "key '{}' wrong size: {} bytes (expected {})",
            key_path,
            sk_bytes.len(),
            ML_DSA_65_SK_LEN
        ));
    }

    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();

    // payload = "<cmd>|<ts>" — this is what the signature covers
    let payload = format!("{}|{}", cmd, ts);

    let mut sig = vec![0u8; ML_DSA_65_SIG_LEN];
    let mut sig_len = ML_DSA_65_SIG_LEN;

    let rc = unsafe {
        OQS_SIG_ml_dsa_65_sign(
            sig.as_mut_ptr(),
            &mut sig_len,
            payload.as_ptr(),
            payload.len(),
            sk_bytes.as_ptr(),
        )
    };

    if rc != 0 {
        return Err(format!("liboqs signing failed (OQS_STATUS={})", rc));
    }

    let hex_sig: String = sig[..sig_len]
        .iter()
        .map(|b| format!("{:02x}", b))
        .collect();

    // signed command = "<cmd>|<ts>|<hex_sig>"
    Ok(format!("{}|{}", payload, hex_sig))
}

/// Sign `cmd` if `key` is Some, otherwise return `cmd` unchanged.
/// Exits with an error message if signing fails — no silent unsigned fallback.
fn maybe_sign(cmd: &str, key: &Option<String>) -> Option<String> {
    match key {
        None => Some(cmd.to_string()),
        Some(path) => match sign_command(cmd, path) {
            Ok(signed) => Some(signed),
            Err(e) => {
                eprintln!("  signing error: {}", e);
                None
            }
        },
    }
}

// -------------------------------------------------------------------------
// CLI definition
// -------------------------------------------------------------------------

#[derive(Parser)]
#[command(
    name    = "skr8tr",
    version = "0.1.0",
    about   = "Skr8tr — Sovereign Workload Orchestrator",
    long_about = "Deploy, scale, and manage workloads across a bare-metal mesh.\n\
                  No Docker. No YAML. No Kubernetes. Just binaries on machines.\n\
                  \n\
                  Auth: generate a keypair with 'skrtrkey keygen', then pass\n\
                  --key ~/.skr8tr/signing.sec to sign mutating commands."
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

    /// Path to ML-DSA-65 signing key (~/.skr8tr/signing.sec).
    /// Required when the Conductor has PQC auth enabled (skrtrview.pub present).
    #[arg(long, global = true)]
    key: Option<String>,

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
    /// Rolling zero-downtime update — launch new replicas, drain old ones
    Rollout {
        /// Path to the updated .skr8tr manifest file
        manifest: String,
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
    /// Tail the last N entries from the cryptographic audit ledger
    Audit {
        /// Number of entries to display (default: 20)
        #[arg(default_value_t = 20)]
        n: u32,
    },
    /// Walk the full audit chain and verify every SHA-256 hash link
    AuditVerify,
}

// -------------------------------------------------------------------------
// UDP messenger — send a command, wait for one response
// -------------------------------------------------------------------------

fn udp_send(host: &str, port: u16, msg: &str, timeout_ms: u64) -> Result<String, String> {
    let addr = format!("{}:{}", host, port);
    let sock = UdpSocket::bind("0.0.0.0:0")
        .map_err(|e| format!("bind error: {e}"))?;

    sock.set_read_timeout(Some(Duration::from_millis(timeout_ms)))
        .map_err(|e| format!("timeout error: {e}"))?;

    sock.send_to(msg.as_bytes(), &addr)
        .map_err(|e| format!("send to {addr} failed: {e}"))?;

    let mut buf = vec![0u8; 16384];
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
            println!("  {:<34}  {:<16}  {:>5}  {:>10}", "NODE ID", "IP", "CPU%", "RAM FREE");
            println!("  {}", "-".repeat(72));
            for entry in parts[1].split(',') {
                let f: Vec<&str> = entry.splitn(4, ':').collect();
                if f.len() == 4 {
                    println!("  {:<34}  {:<16}  {:>4}%  {:>7} MB", f[0], f[1], f[2], f[3]);
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
                    let pid = if f[2] == "0" { "pending".to_string() } else { f[2].to_string() };
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
    let raw = format!("SUBMIT|{path_str}");

    let cmd = match maybe_sign(&raw, &cli.key) {
        Some(c) => c,
        None => return,
    };

    print!("  submitting {}... ", path_str);
    match udp_send(&cli.conductor, 7771, &cmd, cli.timeout_ms) {
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
    let raw = format!("EVICT|{app}");

    let cmd = match maybe_sign(&raw, &cli.key) {
        Some(c) => c,
        None => return,
    };

    print!("  evicting {}... ", app);
    match udp_send(&cli.conductor, 7771, &cmd, cli.timeout_ms) {
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
    match udp_send(&cli.tower, 7772, &format!("LOOKUP|{service}"), cli.timeout_ms) {
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
        let list_resp = match udp_send(&cli.conductor, 7771, "LIST", cli.timeout_ms) {
            Ok(r)  => r,
            Err(e) => { println!("  error querying conductor: {e}"); return; }
        };
        let node_id = match find_node_for_app(&list_resp, app) {
            Some(id) => id,
            None => { println!("  app '{app}' has no active placements"); return; }
        };
        let nodes_resp = match udp_send(&cli.conductor, 7771, "NODES", cli.timeout_ms) {
            Ok(r)  => r,
            Err(e) => { println!("  error querying conductor: {e}"); return; }
        };
        match find_ip_for_node(&nodes_resp, &node_id) {
            Some(ip) => ip,
            None => { println!("  node {node_id} not found in live node table"); return; }
        }
    };

    match udp_send(&node_host, 7775, &format!("LOGS|{app}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|LOGS|") => {
            let parts: Vec<&str> = r.splitn(5, '|').collect();
            let count = parts.get(3).copied().unwrap_or("0");
            let body  = parts.get(4).copied().unwrap_or("");
            println!("  logs for '{app}' on {node_host} ({count} lines captured):");
            println!("  {}", "-".repeat(64));
            for line in body.split('\n') {
                if !line.is_empty() { println!("  {line}"); }
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

fn cmd_audit(cli: &Cli, n: u32) {
    match udp_send(&cli.conductor, 7771, &format!("AUDIT|{n}"), cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|AUDIT|") => {
            let body = &r["OK|AUDIT|".len()..];
            println!("  audit log — last {n} entries:");
            println!("  {}", "-".repeat(72));
            for line in body.split('\n') {
                if !line.is_empty() { println!("  {line}"); }
            }
        }
        Ok(r)  => println!("  {r}"),
        Err(e) => println!("  error: {e}"),
    }
}

fn cmd_audit_verify(cli: &Cli) {
    print!("  verifying audit chain... ");
    match udp_send(&cli.conductor, 7771, "AUDIT_VERIFY", cli.timeout_ms * 3) {
        Ok(r) if r.starts_with("OK|AUDIT_VERIFY|OK") => {
            let parts: Vec<&str> = r.splitn(4, '|').collect();
            let detail = parts.get(3).unwrap_or(&"");
            println!("ok");
            println!("  {detail}");
        }
        Ok(r) if r.starts_with("OK|AUDIT_VERIFY|FAIL") => {
            let parts: Vec<&str> = r.splitn(4, '|').collect();
            let detail = parts.get(3).unwrap_or(&"chain broken");
            println!("CHAIN BROKEN");
            println!("  {detail}");
            std::process::exit(1);
        }
        Ok(r)  => println!("\n  {r}"),
        Err(e) => println!("\n  error: {e}"),
    }
}

fn cmd_rollout(cli: &Cli, manifest: &str) {
    let path = std::fs::canonicalize(manifest)
        .unwrap_or_else(|_| std::path::PathBuf::from(manifest));
    let path_str = path.to_string_lossy();
    let raw = format!("ROLLOUT|{path_str}");

    let cmd = match maybe_sign(&raw, &cli.key) {
        Some(c) => c,
        None => return,
    };

    print!("  rolling out {}... ", path_str);
    match udp_send(&cli.conductor, 7771, &cmd, cli.timeout_ms) {
        Ok(r) if r.starts_with("OK|ROLLOUT|") => {
            let parts: Vec<&str> = r.splitn(3, '|').collect();
            let app = parts.get(2).unwrap_or(&"?");
            println!("ok");
            println!("  app     {app}");
            println!("  status  new replicas launching, old replicas draining (8s settle)");
        }
        Ok(r)  => println!("error\n  {r}"),
        Err(e) => println!("error\n  {e}"),
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
        Commands::Up      { manifest } => cmd_up(&cli, manifest),
        Commands::Down    { app }      => cmd_down(&cli, app),
        Commands::Rollout { manifest } => cmd_rollout(&cli, manifest),
        Commands::Status               => cmd_status(&cli),
        Commands::Nodes                => cmd_nodes(&cli),
        Commands::List                 => cmd_list(&cli),
        Commands::Lookup  { service }  => cmd_lookup(&cli, service),
        Commands::Ping                 => cmd_ping(&cli),
        Commands::Logs { app, node }   => cmd_logs(&cli, app, node.as_deref()),
        Commands::Audit { n }          => cmd_audit(&cli, *n),
        Commands::AuditVerify          => cmd_audit_verify(&cli),
    }
}
