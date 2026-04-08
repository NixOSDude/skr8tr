// SSoA LEVEL 0: SOVEREIGN ANCHOR
// FILE: crates/skr8tr-agent/src/events.rs
// MISSION: AgentEvent type — the canonical vocabulary of mesh anomalies that
//          the skr8tr-agent detects, reasons about, and reports on.
//          All other modules derive from this enum. Immutable law.

use std::fmt;

// ---------------------------------------------------------------------------
// Mesh anomaly events
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub enum AgentEvent {
    /// A node that was previously seen in the NODES table has disappeared.
    /// The Conductor will evict its placements and attempt to rebalance.
    NodeLost {
        node_id: String,
        ip:      String,
    },

    /// A new node has joined the mesh (informational — no action required).
    NodeJoined {
        node_id: String,
        ip:      String,
    },

    /// The active replica count for an app has dropped below its desired level.
    ReplicaDrop {
        app:     String,
        was:     usize,
        now:     usize,
        node_id: String,
    },

    /// An app that was running is no longer listed in the placement table.
    AppGone {
        app: String,
    },

    /// Node CPU is above the scale-up threshold but replica count hasn't grown.
    HighCpu {
        node_id: String,
        ip:      String,
        cpu_pct: u32,
    },

    /// A previously placed replica has been pending (pid=0) for too long.
    ReplicaStuck {
        app:     String,
        node_id: String,
    },

    /// Conductor is unreachable on PING.
    ConductorUnreachable,

    /// Tower is unreachable on PING.
    TowerUnreachable,
}

impl AgentEvent {
    /// Short tag used in log output and as the RAG query prefix.
    pub fn tag(&self) -> &'static str {
        match self {
            Self::NodeLost          { .. } => "NODE_LOST",
            Self::NodeJoined        { .. } => "NODE_JOINED",
            Self::ReplicaDrop       { .. } => "REPLICA_DROP",
            Self::AppGone           { .. } => "APP_GONE",
            Self::HighCpu           { .. } => "HIGH_CPU",
            Self::ReplicaStuck      { .. } => "REPLICA_STUCK",
            Self::ConductorUnreachable     => "CONDUCTOR_UNREACHABLE",
            Self::TowerUnreachable         => "TOWER_UNREACHABLE",
        }
    }

    /// Build the natural-language query to send to the RAG pipeline.
    pub fn rag_query(&self) -> String {
        match self {
            Self::NodeLost { node_id, ip } =>
                format!(
                    "A Skr8tr node has gone silent. node_id={node_id} ip={ip}. \
                     What does rebalancer_thread do in skr8tr_sched.c when NODE_EXPIRY_S \
                     has elapsed? What does placement_evict_node do? How does the \
                     Conductor reconcile replica counts after a node expires?"
                ),

            Self::NodeJoined { node_id, ip } =>
                format!(
                    "A new node has joined the Skr8tr mesh: node_id={node_id} ip={ip}. \
                     How does the Conductor learn about it? What triggers workload placement?"
                ),

            Self::ReplicaDrop { app, was, now, node_id } =>
                format!(
                    "Replica count for app '{app}' dropped from {was} to {now}. \
                     Last node: {node_id}. \
                     What does rebalancer_thread do to reconcile replica counts in \
                     skr8tr_sched.c? What does placement_count and placement_alloc \
                     do when a replica is lost? How does launch_replica recover it?"
                ),

            Self::AppGone { app } =>
                format!(
                    "App '{app}' has completely disappeared from the Skr8tr workload table. \
                     What triggers eviction? Could the Conductor have restarted and lost state?"
                ),

            Self::HighCpu { node_id, ip, cpu_pct } =>
                format!(
                    "Node {node_id} ({ip}) CPU is at {cpu_pct}%. \
                     In skr8tr_sched.c rebalancer_thread, what is the SCALE_UP_CYCLES \
                     threshold? What does high_cpu_cycles track? When does launch_replica \
                     fire on a different target node via node_least_loaded?"
                ),

            Self::ReplicaStuck { app, node_id } =>
                format!(
                    "Replica of '{app}' on node {node_id} has pid=0 (pending) for multiple \
                     poll cycles. In skr8tr_node.c launch_proc, what causes a pid to stay 0? \
                     What does execv failure look like in the child process? \
                     Does the Conductor in skr8tr_sched.c track pid updates after LAUNCH?"
                ),

            Self::ConductorUnreachable =>
                "skr8tr_sched is not responding to PING on UDP port 7771. \
                 What does fabric_bind do in skr8tr_sched.c main? What is SCHED_PORT? \
                 How does state_load replay state from the conductor state file on restart? \
                 What command restarts the Conductor without losing workload state?".to_string(),

            Self::TowerUnreachable =>
                "skr8tr_reg is not responding to PING on UDP port 7772. \
                 What does skr8tr_reg.c bind? What is TOWER_PORT in skr8tr_node.c? \
                 Can launch_proc still run without the Tower? \
                 What happens to tower_register calls if the Tower is down?".to_string(),
        }
    }
}

impl fmt::Display for AgentEvent {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NodeLost    { node_id, ip }       => write!(f, "[NODE_LOST]      node={node_id} ip={ip}"),
            Self::NodeJoined  { node_id, ip }       => write!(f, "[NODE_JOINED]    node={node_id} ip={ip}"),
            Self::ReplicaDrop { app, was, now, .. } => write!(f, "[REPLICA_DROP]   app={app} {was}→{now} replicas"),
            Self::AppGone     { app }               => write!(f, "[APP_GONE]       app={app}"),
            Self::HighCpu     { node_id, cpu_pct, ..} => write!(f, "[HIGH_CPU]       node={node_id} cpu={cpu_pct}%"),
            Self::ReplicaStuck{ app, node_id }      => write!(f, "[REPLICA_STUCK]  app={app} node={node_id}"),
            Self::ConductorUnreachable              => write!(f, "[CONDUCTOR_UNREACHABLE]"),
            Self::TowerUnreachable                  => write!(f, "[TOWER_UNREACHABLE]"),
        }
    }
}
