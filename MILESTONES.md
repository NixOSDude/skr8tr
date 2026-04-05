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

---

## [2026-04-05] main — Phase 1 Complete: SkrtrMaker Parser + Fleet Node Daemon

### Files Delivered
- `src/parser/skrmaker.h` — SSoA LEVEL 0: `LambProc` canonical workload struct + all sub-structs
- `src/parser/skrmaker.c` — SSoA LEVEL 1: SkrtrMaker file parser (indent + brace syntax)
- `src/parser/skrmaker_test.c` — SSoA LEVEL 3: smoke test binary
- `src/core/fabric.h` — SSoA LEVEL 1: UDP mesh primitive API
- `src/core/fabric.c` — SSoA LEVEL 1: UDP socket layer (bind, send, recv, broadcast)
- `src/daemon/skr8tr_node.c` — SSoA LEVEL 1: Fleet node daemon

### Verified
- `skrmaker_parse()` round-trips both example manifests (react-app + lambdac-job) cleanly
- RAM parsing: `256mb` → `268435456 B`, `4gb` → `4294967296 B`
- All sub-blocks parsed: build, serve, health, scale, env, input, output, on-complete
- `skr8tr_node` boots and generates ML-DSA-65 ephemeral identity from liboqs-0.15.0
- UDP commands verified over live socket:
  - `PING` → `OK|PONG|<node_id>`
  - `STATUS` → `OK|STATUS|0|`
  - `LAUNCH|name=demo|bin=/bin/true|port=8080` → `OK|LAUNCHED|demo|<pid>`
  - `STATUS` (post-launch) → `OK|STATUS|1|demo:<pid>`
- `--run <manifest.skr8tr>` path: parses manifest and fork/execs on startup
- `KILL|<app_name>` → SIGTERM → SIGKILL with 2s grace window

### Design Decisions
- `skr8tr_node.c` linked with `skrmaker.c` — daemon parses manifests natively, no intermediate format
- `gnu23` standard flag used (C23 + POSIX extensions) — required for `nanosleep`, `putenv`, `kill`
- Heartbeat thread broadcasts `HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb>` every 5s
- Node ID = hex of first 16 bytes of ML-DSA-65 public key — ephemeral, not stored
- Zero LambdaC source files modified — Skr8tr is a fully sovereign codebase

### Next Milestone
Phase 2: `skr8tr_serve.c` — static file server (serve any app's dist/, no nginx)
- HTTP/1.1 GET handler for static directories
- Port from CLAUDE.md: 7773 (external ingress)
- Serves whatever `serve static <dir>` points to — React, Vue, raw HTML, WASM, anything

---

## BACKBURNER

- Phase 2: skr8tr_sched.c — capacity-aware masterless scheduler
- Phase 3: skr8tr_reg.c — service registry (Tower)
- Phase 4: Rust CLI (`skr8tr` binary) — up/down/scale/status/logs
- Phase 5: LambdaC workload integration — .lc jobs as native Skr8tr targets
- Phase 6: LambdaStrater — full enterprise orchestration platform on Skr8tr foundation
