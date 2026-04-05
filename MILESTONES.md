# MILESTONES.md — Skr8tr Sovereign Trajectory
# Skr8tr — The k8s Killer
#
# PROTOCOL: Read this file first when Captain says "I am back".
# Append an entry after every major change or milestone commit.

---

## [2026-04-05] main — Sovereign Scaffold Initialized

- Gitea repo created: http://192.168.68.50:3000/gitea/skr8tr
- CLAUDE.md: full federation laws, Skr8tr identity, language boundary law
- Directory structure: src/core, src/daemon, src/parser, src/server, cli/, examples/
- Companion project: LambdaC (gitea:LambdaC) — lvm_nodes are native Skr8tr workers
- SkrtrMaker format defined — sovereign alternative to YAML/TOML/k8s manifests
- Four substrates named: Conductor (sched), Fleet (node), Tower (reg), Deck (CLI)

### Design Decisions Locked
- No Docker, no OCI, no containerd — bare processes and WASM modules only
- C23 for all daemons, Rust for CLI — hard boundary, no mixing
- Masterless scheduler — no SPOF, no etcd, no Raft leader election
- ML-DSA-65 PQC identity on every node from boot
- SkrtrMaker is the ONLY deployment manifest format

### Next Milestone
Build Phase 1: SkrtrMaker parser + skr8tr_node.c worker daemon
  - skrmaker.c: parse .skr8tr files into workload descriptor structs
  - skr8tr_node.c: UDP mesh listener, process launch/kill, health heartbeat
  - Verify: `skr8tr up examples/react-app.skr8tr` deploys nginx-free static app

---

## BACKBURNER

- Phase 2: skr8tr_sched.c — capacity-aware masterless scheduler
- Phase 3: skr8tr_reg.c — service registry (Tower)
- Phase 4: Rust CLI (`skr8tr` binary) — up/down/scale/status/logs
- Phase 5: LambdaC workload integration — .lc jobs as native Skr8tr targets
- Phase 6: LambdaStrater — full enterprise orchestration platform on Skr8tr foundation
