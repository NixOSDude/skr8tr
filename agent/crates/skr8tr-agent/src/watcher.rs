// SSoA LEVEL 1: FOUNDATION ANCHOR
// FILE: crates/skr8tr-agent/src/watcher.rs
// MISSION: Poll the Skr8tr Conductor and Tower for mesh state, diff successive
//          snapshots to detect anomalies, and emit AgentEvents.
//          Wire protocol is identical to the CLI — plain UDP, no crypto.

use std::{
    collections::HashMap,
    net::UdpSocket,
    time::Duration,
};

use anyhow::Result;

use crate::events::AgentEvent;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const CONDUCTOR_PORT: u16 = 7771;
const TOWER_PORT:     u16 = 7772;
const STUCK_PENDING_CYCLES: u32 = 3;   /* >N poll cycles pending = stuck */
const HIGH_CPU_THRESHOLD:   u32 = 80;  /* % above which we fire HIGH_CPU */

// ---------------------------------------------------------------------------
// Snapshot types
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct NodeInfo {
    pub node_id: String,
    pub ip:      String,
    pub cpu_pct: u32,
    pub ram_mb:  i64,
}

#[derive(Debug, Clone)]
pub struct PlacementInfo {
    pub app:     String,
    pub node_id: String,
    pub pid:     i32,          /* 0 = pending */
}

/// Complete mesh state at one poll cycle.
#[derive(Debug, Default, Clone)]
pub struct MeshSnapshot {
    pub nodes:      Vec<NodeInfo>,
    pub placements: Vec<PlacementInfo>,
    /// For tracking how many consecutive cycles each replica has been pending.
    pub pending_cycles: HashMap<(String, String), u32>,
}

// ---------------------------------------------------------------------------
// CLI args
// ---------------------------------------------------------------------------

#[derive(Debug, clap::Args)]
pub struct WatchArgs {
    /// Conductor host
    #[arg(long, default_value = "127.0.0.1")]
    pub conductor: String,

    /// Tower host
    #[arg(long, default_value = "127.0.0.1")]
    pub tower: String,

    /// Poll interval in seconds
    #[arg(long, default_value_t = 30)]
    pub interval_s: u64,

    /// Directory of the HNSW index (must be pre-built via `ingest`)
    #[arg(long, default_value = "/home/sbaker/RusticAgentic/vault/skr8tr-index")]
    pub index: std::path::PathBuf,

    /// Automatically apply fixes for gated anomalies (ConductorUnreachable, ReplicaDrop)
    #[arg(long, default_value_t = false)]
    pub auto_fix: bool,

    /// Path to the skr8tr CLI binary (used by auto-fix)
    #[arg(long, default_value = "/home/sbaker/skr8tr/cli/target/release/skr8tr")]
    pub skr8tr_bin: std::path::PathBuf,
}

// ---------------------------------------------------------------------------
// UDP helper — same interface as the Rust CLI
// ---------------------------------------------------------------------------

pub fn udp_send(host: &str, port: u16, msg: &str, timeout_ms: u64)
    -> Result<String, String>
{
    let addr = format!("{host}:{port}");
    let sock = UdpSocket::bind("0.0.0.0:0")
        .map_err(|e| format!("bind: {e}"))?;
    sock.set_read_timeout(Some(Duration::from_millis(timeout_ms)))
        .map_err(|e| format!("timeout: {e}"))?;
    sock.send_to(msg.as_bytes(), &addr)
        .map_err(|e| format!("send to {addr}: {e}"))?;
    let mut buf = [0u8; 8192];
    let (n, _) = sock.recv_from(&mut buf)
        .map_err(|e| format!("no response from {addr}: {e}"))?;
    Ok(String::from_utf8_lossy(&buf[..n]).trim().to_string())
}

// ---------------------------------------------------------------------------
// Parse NODES response: OK|NODES|n|id:ip:cpu:ram,...
// ---------------------------------------------------------------------------

pub fn parse_nodes(resp: &str) -> Vec<NodeInfo> {
    let body = match resp.strip_prefix("OK|NODES|") {
        Some(b) => b,
        None    => return vec![],
    };
    let parts: Vec<&str> = body.splitn(2, '|').collect();
    if parts.len() < 2 || parts[1].is_empty() { return vec![]; }

    parts[1].split(',').filter_map(|entry| {
        let f: Vec<&str> = entry.splitn(4, ':').collect();
        if f.len() < 4 { return None; }
        Some(NodeInfo {
            node_id: f[0].to_string(),
            ip:      f[1].to_string(),
            cpu_pct: f[2].parse().unwrap_or(0),
            ram_mb:  f[3].parse().unwrap_or(0),
        })
    }).collect()
}

// ---------------------------------------------------------------------------
// Parse LIST response: OK|LIST|n|app:node:pid,...
// ---------------------------------------------------------------------------

pub fn parse_placements(resp: &str) -> Vec<PlacementInfo> {
    let body = match resp.strip_prefix("OK|LIST|") {
        Some(b) => b,
        None    => return vec![],
    };
    let parts: Vec<&str> = body.splitn(2, '|').collect();
    if parts.len() < 2 || parts[1].is_empty() { return vec![]; }

    parts[1].split(',').filter_map(|entry| {
        let f: Vec<&str> = entry.splitn(3, ':').collect();
        if f.len() < 3 { return None; }
        Some(PlacementInfo {
            app:     f[0].to_string(),
            node_id: f[1].to_string(),
            pid:     f[2].parse().unwrap_or(0),
        })
    }).collect()
}

// ---------------------------------------------------------------------------
// Diff two snapshots → produce events
// ---------------------------------------------------------------------------

pub fn diff(prev: &MeshSnapshot, next: &mut MeshSnapshot) -> Vec<AgentEvent> {
    let mut events = Vec::new();

    // Build lookup maps
    let prev_nodes: HashMap<&str, &NodeInfo> = prev.nodes.iter()
        .map(|n| (n.node_id.as_str(), n))
        .collect();
    let next_nodes: HashMap<&str, &NodeInfo> = next.nodes.iter()
        .map(|n| (n.node_id.as_str(), n))
        .collect();

    // --- Node lost ---
    for (id, info) in &prev_nodes {
        if !next_nodes.contains_key(*id) {
            events.push(AgentEvent::NodeLost {
                node_id: id.to_string(),
                ip:      info.ip.clone(),
            });
        }
    }

    // --- Node joined ---
    for (id, info) in &next_nodes {
        if !prev_nodes.contains_key(*id) {
            events.push(AgentEvent::NodeJoined {
                node_id: id.to_string(),
                ip:      info.ip.clone(),
            });
        }
    }

    // --- High CPU ---
    for node in &next.nodes {
        if node.cpu_pct >= HIGH_CPU_THRESHOLD {
            events.push(AgentEvent::HighCpu {
                node_id: node.node_id.clone(),
                ip:      node.ip.clone(),
                cpu_pct: node.cpu_pct,
            });
        }
    }

    // --- Replica drop ---
    // Count active replicas per app in prev and next
    let count = |placements: &[PlacementInfo]| {
        let mut m: HashMap<String, usize> = HashMap::new();
        for p in placements { *m.entry(p.app.clone()).or_default() += 1; }
        m
    };
    let prev_counts = count(&prev.placements);
    let next_counts = count(&next.placements);

    for (app, &prev_n) in &prev_counts {
        let next_n = next_counts.get(app).copied().unwrap_or(0);
        if next_n < prev_n {
            // Find a node_id from the previous placements for context
            let node_id = prev.placements.iter()
                .find(|p| &p.app == app)
                .map(|p| p.node_id.clone())
                .unwrap_or_default();
            events.push(AgentEvent::ReplicaDrop {
                app:     app.clone(),
                was:     prev_n,
                now:     next_n,
                node_id,
            });
        }
        if next_n == 0 {
            events.push(AgentEvent::AppGone { app: app.clone() });
        }
    }

    // --- Replica stuck in pending ---
    for pl in &next.placements {
        let key = (pl.app.clone(), pl.node_id.clone());
        if pl.pid == 0 {
            let cycles = next.pending_cycles.entry(key).or_default();
            *cycles += 1;
            if *cycles >= STUCK_PENDING_CYCLES {
                events.push(AgentEvent::ReplicaStuck {
                    app:     pl.app.clone(),
                    node_id: pl.node_id.clone(),
                });
            }
        } else {
            next.pending_cycles.remove(&key);
        }
    }

    events
}

// ---------------------------------------------------------------------------
// Single poll cycle — returns new snapshot and any events vs previous
// ---------------------------------------------------------------------------

pub fn poll(
    conductor_host: &str,
    tower_host:     &str,
    prev:           &MeshSnapshot,
) -> (MeshSnapshot, Vec<AgentEvent>) {
    let mut events = Vec::new();
    const TIMEOUT: u64 = 3000;

    // Ping conductor
    let cond_ok = udp_send(conductor_host, CONDUCTOR_PORT, "PING", TIMEOUT).is_ok();
    if !cond_ok {
        events.push(AgentEvent::ConductorUnreachable);
    }

    // Ping tower
    let tower_ok = udp_send(tower_host, TOWER_PORT, "PING", TIMEOUT).is_ok();
    if !tower_ok {
        events.push(AgentEvent::TowerUnreachable);
    }

    // Query nodes
    let nodes = if cond_ok {
        udp_send(conductor_host, CONDUCTOR_PORT, "NODES", TIMEOUT)
            .ok()
            .as_deref()
            .map(parse_nodes)
            .unwrap_or_default()
    } else {
        vec![]
    };

    // Query placements
    let placements = if cond_ok {
        udp_send(conductor_host, CONDUCTOR_PORT, "LIST", TIMEOUT)
            .ok()
            .as_deref()
            .map(parse_placements)
            .unwrap_or_default()
    } else {
        vec![]
    };

    let mut next = MeshSnapshot {
        nodes,
        placements,
        pending_cycles: prev.pending_cycles.clone(),
    };

    let mut diff_events = diff(prev, &mut next);
    events.append(&mut diff_events);

    (next, events)
}
