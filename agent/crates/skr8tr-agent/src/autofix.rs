// SSoA LEVEL 3: MANIFEST SHARD
// REPO: oss
// FILE: agent/crates/skr8tr-agent/src/autofix.rs
// MISSION: Deterministic auto-fix actions mapped directly to AgentEvent types.
//          The LLM recommendation is advisory; this layer is the executor.
//          Only gated event types trigger action — all others are skipped.
//
// Gated events:
//   ConductorUnreachable → restart skr8tr_sched
//   ReplicaDrop          → resubmit app manifest via `skr8tr up`
//   NodeLost             → no-op (conductor rebalancer handles recovery)

use std::path::PathBuf;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use crate::events::AgentEvent;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const CONDUCTOR_BIN:   &str = "/home/sbaker/skr8tr/bin/skr8tr_sched";
const CONDUCTOR_STATE: &str = "/tmp/skr8tr_conductor.state";

// ---------------------------------------------------------------------------
// Result type
// ---------------------------------------------------------------------------

pub enum AutoFixResult {
    /// Action was taken — description of what ran
    Applied(String),
    /// No action taken — reason
    Skipped(String),
}

impl AutoFixResult {
    pub fn log(&self, event_tag: &str) {
        let ts = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);

        match self {
            Self::Applied(msg) => {
                tracing::warn!(
                    "[AUTO-FIX] [{ts}] {event_tag} → APPLIED: {msg}"
                );
            }
            Self::Skipped(msg) => {
                tracing::info!(
                    "[AUTO-FIX] [{ts}] {event_tag} → skipped: {msg}"
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/// Apply a deterministic fix for the given event.
/// `skr8tr_bin` is the path to the skr8tr CLI binary.
pub fn apply(event: &AgentEvent, skr8tr_bin: &PathBuf) -> AutoFixResult {
    match event {
        AgentEvent::ConductorUnreachable => {
            fix_restart_conductor()
        }

        AgentEvent::ReplicaDrop { app, .. } => {
            fix_resubmit_app(app, skr8tr_bin)
        }

        AgentEvent::NodeLost { node_id, .. } => {
            // Conductor rebalancer_thread handles dead-node recovery automatically.
            // No intervention needed — log and let it run.
            AutoFixResult::Skipped(format!(
                "NodeLost {node_id}: conductor rebalancer will evict placements and relaunch"
            ))
        }

        other => AutoFixResult::Skipped(format!(
            "{}: no auto-fix defined for this event type",
            other.tag()
        )),
    }
}

// ---------------------------------------------------------------------------
// Fix: restart the Conductor daemon
// ---------------------------------------------------------------------------

fn fix_restart_conductor() -> AutoFixResult {
    match Command::new(CONDUCTOR_BIN).spawn() {
        Ok(child) => AutoFixResult::Applied(format!(
            "Spawned skr8tr_sched (pid={})", child.id()
        )),
        Err(e) => AutoFixResult::Skipped(format!(
            "Cannot spawn {CONDUCTOR_BIN}: {e}"
        )),
    }
}

// ---------------------------------------------------------------------------
// Fix: resubmit a dropped app via `skr8tr up <manifest>`
//
// Reads /tmp/skr8tr_conductor.state which the Conductor writes on every
// SUBMIT — one manifest path per line, format: "<app_name>|<manifest_path>"
// ---------------------------------------------------------------------------

fn fix_resubmit_app(app: &str, skr8tr_bin: &PathBuf) -> AutoFixResult {
    let state = match std::fs::read_to_string(CONDUCTOR_STATE) {
        Ok(s)  => s,
        Err(e) => return AutoFixResult::Skipped(format!(
            "Cannot read {CONDUCTOR_STATE}: {e}"
        )),
    };

    // Find line matching this app
    let manifest_path = state
        .lines()
        .find_map(|line| {
            // Format: "app_name|/path/to/manifest.skr8tr"
            let mut parts = line.splitn(2, '|');
            let name = parts.next()?.trim();
            let path = parts.next()?.trim();
            if name == app { Some(path.to_string()) } else { None }
        });

    let path = match manifest_path {
        Some(p) => p,
        None    => return AutoFixResult::Skipped(format!(
            "No manifest path found for '{app}' in {CONDUCTOR_STATE}"
        )),
    };

    match Command::new(skr8tr_bin).args(["up", &path]).output() {
        Ok(out) => {
            let reply = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if out.status.success() {
                AutoFixResult::Applied(format!(
                    "Resubmitted '{app}' via {path} → {reply}"
                ))
            } else {
                let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
                AutoFixResult::Skipped(format!(
                    "skr8tr up {path} failed: {err}"
                ))
            }
        }
        Err(e) => AutoFixResult::Skipped(format!(
            "Failed to exec skr8tr up {path}: {e}"
        )),
    }
}
