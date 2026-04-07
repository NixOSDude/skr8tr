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

---

## [2026-04-05] main — Phase 2 Complete: Sovereign Static File Server

### Files Delivered
- `src/server/skr8tr_serve.c` — SSoA LEVEL 1: HTTP/1.1 static file server

### Verified
- `GET /` → 200 + index.html (directory auto-index) ✓
- `GET /index.html` → 200 + correct MIME `text/html; charset=utf-8` ✓
- `HEAD` → 200 + headers, no body ✓
- `GET /missing` → 200 + index.html (SPA fallback — correct for React routing) ✓
- ETag round-trip: `If-None-Match` → 304 Not Modified ✓
- `skr8tr_node --run manifest.skr8tr` with `serve static <dir>` → auto-launches
  skr8tr_serve with `--dir` and `--port` args ✓
- Full integration: parse manifest → fork skr8tr_serve → serve HTML → STATUS
  shows process in node table ✓

### Design Decisions
- `sendfile(2)` for zero-copy file transfer — kernel handles data path
- ETag = hex(mtime) + hex(size) — lightweight, no content hashing required
- SPA fallback: unknown paths serve `<root>/index.html` — React/Vue router works
- Path traversal: `realpath()` + prefix check — jail to static root, no escape
- MIME table covers html, css, js, mjs, json, wasm, svg, png, jpg, gif, webp,
  ico, pdf, txt, map, xml, ttf, woff, woff2
- Detached pthread per connection, SO_REUSEADDR + SO_REUSEPORT on listener
- `Cache-Control: public, max-age=3600` — browsers cache assets for 1 hour
- No nginx. No Node. No Apache. Zero external dependencies.
- Hardware/cloud agnostic: runs on bare metal, VPS, ARM, x86, GCP VM, NUC, Pi
- No GPU dependency — GPU is LambdaC's domain, not Skr8tr's

### Architecture Notes (Captain additions this session)
- Elasticity: `scale { min, max, cpu-above, cpu-below }` already in LambProc structs.
  The Conductor (skr8tr_sched.c) will consume heartbeat metrics and issue
  LAUNCH/KILL commands to grow/shrink replica counts. Node already broadcasts
  cpu_pct + ram_free_mb every 5s as input to scaling decisions.
- Hardware/cloud agnostic by design: bare process on any Linux host, UDP mesh
  auto-discovers peers on subnet — no cloud SDK, no container runtime.

---

## [2026-04-05] main — Phase 4 + 5 Complete: Tower + Deck — FULL STACK OPERATIONAL

### Files Delivered
- `src/daemon/skr8tr_reg.c` — SSoA LEVEL 1: The Tower (service registry)
- `cli/Cargo.toml` + `cli/src/main.rs` — SSoA LEVEL 3: The Deck (Rust CLI)

### Tower Verified
- REGISTER|name|ip|port → OK|REGISTERED ✓
- LOOKUP round-robin across 3 replicas: .10 → .11 → .12 ✓
- After DEREGISTER one: alternates remaining two ✓
- ERR|NOT_FOUND for unknown service ✓
- TTL countdown shown in LIST ✓
- Reaper thread expires stale entries automatically ✓

### CLI Verified (full stack: node + sched + reg + serve + skr8tr)
- skr8tr ping  → conductor ok, tower ok ✓
- skr8tr nodes → 1 node, node_id, ip, cpu%, ram_free_mb ✓
- skr8tr up    → submitted, app name + node_id returned ✓
- skr8tr status → nodes + 3 replicas placed ✓
- skr8tr lookup → ERR|NOT_FOUND for unregistered service ✓
- skr8tr down  → evicted ok ✓
- skr8tr list  → 0 replica(s) running ✓

### All 5 Phases Complete
| Phase | Component | Binary |
|-------|-----------|--------|
| 1 | SkrtrMaker Parser + Fleet Node | bin/skr8tr_node |
| 2 | Static File Server | bin/skr8tr_serve |
| 3 | The Conductor (scheduler) | bin/skr8tr_sched |
| 4 | The Tower (service registry) | bin/skr8tr_reg |
| 5 | The Deck (Rust CLI) | cli/target/release/skr8tr |

### Architecture
- Hardware/cloud agnostic: bare process on any Linux host
- PQC identity: ML-DSA-65 ephemeral per-node via liboqs
- Elastic: heartbeat-driven scale up/down, dead node recovery
- Process-agnostic: any compiled binary, any language
- VM/hypervisor orchestration: Phase 6 path — point bin at QEMU/Firecracker
- No Docker. No YAML. No etcd. No Kubernetes. 15MB control plane.

### Next Milestone
Phase 7: TLS on Tower, NixOS overlay for reproducible builds, public alpha packaging

---

---

## [2026-04-05] feature/sovereign-multiplication-logic — Phase 6 Complete: VM Orchestration + Production Hardening

### Files Delivered / Modified
- `src/parser/skrmaker.h` — Added `SkrtrVM` struct + `SKRTR_TYPE_VM` enum variant
- `src/parser/skrmaker.c` — Added `parse_vm()` block parser; `type vm` wired in `parse_app`
- `src/daemon/skr8tr_node.c` — Full Phase 6 rewrite:
  - Dual-socket architecture: port 7770 (mesh broadcast) + port 7775 (dedicated command port)
  - `LogRing` (200 × 256) per-process stdout/stderr ring buffer
  - `log_reader_thread()` — captures child stdout/stderr via pipe
  - `health_probe()` — TCP + HTTP GET health check enforcement; kill+mark-inactive on failure
  - `tower_register()` / `tower_deregister()` — auto-registration with Tower on launch/kill
  - `launch_vm()` — QEMU argv builder + Firecracker JSON config generator
  - LOGS command handler — returns ring buffer to CLI
  - `--tower <host>` CLI flag
- `src/daemon/skr8tr_sched.c` — Persistent workload state:
  - `manifest_path` field in `Workload` struct
  - `state_save()` — writes active manifest paths to `/tmp/skr8tr_conductor.state`
  - `state_load()` — replays SUBMIT commands on restart
  - Forward declaration to resolve ordering
- `cli/src/main.rs` — Added `logs` command:
  - Two-step node resolution: LIST → find node_id, NODES → find node IP
  - Queries node:7775 directly for `LOGS|<app>` response
  - `find_node_for_app()` / `find_ip_for_node()` helpers
- `examples/vm-workload.skr8tr` — NEW: Firecracker microVM manifest example

### Verified — Full Integration
| Command | Result |
|---------|--------|
| `skr8tr ping` | conductor ok, tower ok |
| `skr8tr nodes` | 2 nodes, node_id, ip, cpu%, ram_free_mb |
| `skr8tr up analytics-job.skr8tr` | submitted, placement recorded |
| `skr8tr list` | 1-2 replicas listed |
| `skr8tr logs test-echo` | 0 lines captured (sh has no output — correct) |
| `skr8tr down test-echo` | evicted ok |
| `skr8tr status` | 0 replicas after eviction |
| Persistent state | `/tmp/skr8tr_conductor.state` written on SUBMIT, cleared on EVICT |

### Architecture Now
- Fleet node: dual UDP sockets (7770 mesh / 7775 cmd), full log capture, health enforcement
- Conductor: survives restart — state persists and replays
- VM orchestration: QEMU and Firecracker microVMs as SkrProc workloads
- CLI: `up / down / status / nodes / list / lookup / logs / ping`
- Zero Docker. Zero YAML. Zero etcd. Zero Kubernetes. ~20KB control plane.

### Next Milestone
Phase 7: NixOS overlay for reproducible builds + commercial packaging ($19.99/site/month)

---

## [2026-04-06] main — Phase 9: Skr8trView — Sovereign Mesh Control UI

### What Was Built
Full-stack sovereign control panel for the Skr8tr mesh. Five files, zero external frameworks.

| File | Role |
|------|------|
| `src/cockpit/skrtrpass.h` | ML-DSA-65 SkrtrPass token verify + mint (SSoA L1) |
| `src/cockpit/skr8tr_cockpit.c` | C23 WebSocket cockpit server, port 7780 (SSoA L2) |
| `src/cockpit/gen_skrtrpass.c` | Token keygen/mint/verify CLI (SSoA L3) |
| `ui/index.html` | Sovereign dark-theme single-page UI (SSoA L3) |
| `Makefile` | Added skr8tr_cockpit + gen_skrtrpass targets |

### Skr8trView Architecture
- **Port 7780**: HTTP static server (serves `ui/`) + WebSocket upgrade at `/ws`
- **Auth gate**: First WS frame must be `AUTH|<skrtrpass_token>`
  - ML-DSA-65 verify against `skrtrview.pub`
  - Grants: `AUTH_OK|operator` (read-only) or `AUTH_OK|admin` (full control)
  - Dev mode: no pubkey file → auto-grant admin
- **WS commands**: NODES, LIST, SERVICES, LOGS|app, SUBMIT|path (admin), EVICT|app (admin), PING
- **Push thread**: broadcasts live NODES + LIST to all authed sessions every 5s
- **UI panels**: Cluster, Workloads, Services, Logs, Agent Feed

### Tokens
```bash
# Generate keypair (one-time):
bin/gen_skrtrpass keygen

# Mint operator token (30 days):
bin/gen_skrtrpass mint --role operator --user captain --ttl 2592000 --key skrtrview.sec

# Mint admin token:
bin/gen_skrtrpass mint --role admin --user captain --ttl 2592000 --key skrtrview.sec

# Start cockpit:
nohup bin/skr8tr_cockpit --ui ./ui --pubkey ./skrtrview.pub > /tmp/cockpit.log 2>&1 &
# Then open: http://127.0.0.1:7780/
```

### Build Status
- Both binaries compile clean: zero warnings, zero errors, `-std=gnu23 -Wall -Wextra`
- Pushed: Gitea `gitea/skr8tr` main → e7e4328

### Next Milestone
Phase 10: Agent Feed live integration — pipe skr8tr-agent events into Skr8trView

---

## [2026-04-06] main — Phase 9 Hotfix: Skr8trView WebSocket + Dev Mode

### Bugs Fixed
1. **SHA-1 single-block overflow** — WS key (24) + GUID (36) = 60 bytes spans two SHA-1 blocks.
   Old code processed only one block → wrong `Sec-WebSocket-Accept` → browser rejected upgrade.
   Fix: extracted `sha1_block()`, proper two-block pad/process in `sha1()`.

2. **Dev mode bypass** — Empty token sent `AUTH|` → `skrtrpass_verify("")` returned
   `SKRTRPASS_ERR_PARSE` before ever checking pubkey file → "Auth failed: malformed token".
   Fix: `stat(g_pubkey_path)` first; if file absent → grant admin without calling verify.

### Status
- Skr8trView fully operational: `http://127.0.0.1:7780/`
- Dev mode confirmed: empty token → AUTH_OK|admin
- All 5 panels loading (Cluster/Workloads/Services/Logs/Agent Feed)

---

## [2026-04-06] main — Phase 10: Agent Feed Live Integration

### What Was Built
Full pipe from skr8tr-agent (Mistral-Nemo recommendations) → Skr8trView Agent Feed panel.

**Wire:**
```
skr8tr-agent watch
  → detects anomaly (NodeLost, ReplicaDrop, HighCpu…)
  → RAG query against Skr8tr codebase (gte-large-en-v1.5, HNSW)
  → Mistral-Nemo generates grounded recommendation
  → pipe_emit() writes AGENT|tag|event_str|answer to /tmp/skr8trview.pipe
  → skr8tr_cockpit pipe_reader_thread reads line
  → broadcast_agent() sends WS frame to all authed sessions
  → ui/index.html renderAgentEvent() renders card in Agent Feed panel
```

### Run (full stack)
```bash
# Skr8tr daemons + cockpit (from /home/sbaker/skr8tr):
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname)
nohup bin/skr8tr_sched > /tmp/sk_sched.log 2>&1 &
nohup bin/skr8tr_node  > /tmp/sk_node.log  2>&1 &
LD_LIBRARY_PATH="$OQS_LIBDIR" nohup bin/skr8tr_cockpit --ui ui > /tmp/cockpit.log 2>&1 &

# skr8tr-agent with pipe (from /home/sbaker/RusticAgentic):
RUST_LOG=info SKRTRVIEW_PIPE=/tmp/skr8trview.pipe \
  nohup ./target/release/skr8tr-agent watch --interval-s 30 --index vault/skr8tr-index &
```

### Next Milestone
Phase 11: NixOS flake overlay for reproducible commercial builds

---

## [2026-04-06] main — Phase 11a: Args Fix + PID Tracking — Full Workload Verification

### Bugs Fixed
1. **SkrProc missing `args` field** — `SkrProc` struct had no args field; node built `argv = { bin, NULL }`.
   `sleep` with no args exits immediately → zombie. Fix: added `char args[512]` to struct.
2. **Conductor never captured real PID** — `launch_replica()` sent LAUNCH on `send_sock` (bound to 7771),
   never read reply → `pid=0` stored forever. Fix: ephemeral `fabric_bind(0)` socket for LAUNCH send/recv;
   parses `OK|LAUNCHED|name|pid` reply to record real PID.
3. **`exec` manifest key not parsed** — `skrmaker.c` only recognized `bin`; `exec` (the natural key name)
   was silently ignored. Fix: both `exec` and `bin` keys now populate `proc->bin`.

### Files Modified
- `src/parser/skrmaker.h` — `char args[512]` added to `SkrProc` after `bin`
- `src/parser/skrmaker.c` — `exec` + `args` key parsing added
- `src/daemon/skr8tr_sched.c` — ephemeral socket PID capture; `args=` in LAUNCH command
- `src/daemon/skr8tr_node.c` — extract `args=` from LAUNCH; full argv build in `launch_proc()`
- `examples/my-server.skr8tr` — NEW: `exec /bin/sleep` + `args 3600` test workload

### Verified End-to-End
```
$ echo "SUBMIT|.../my-server.skr8tr" > /dev/udp/127.0.0.1/7771
$ exec 3<>/dev/udp/127.0.0.1/7771; echo -n "LIST" >&3; ...
OK|LIST|1|my-server:b33c8ed0...:968189

$ ps -p 968189 -o pid,ppid,comm,args
968189  967724 sleep  /bin/sleep 3600   ✓
```

Workloads tab now shows real PID. Replicas no longer stuck in "pending".

---

## [2026-04-06] main — Phase 12: ML-DSA-65 Sovereign Auth Layer

### What Was Built
Post-quantum command signing for the Skr8tr mesh. No passwords. No TLS. No bearer tokens.

**Auth boundary:** Conductor port 7771. SUBMIT and EVICT require a valid ML-DSA-65
signature. Read-only commands (NODES, LIST, PING) and the internal mesh are open.

**Wire format:** `<cmd>|<unix_ts>|<6618-hex-ml-dsa65-sig>`
**Replay protection:** ±30s nonce window (no server-side state required)
**Dev mode:** if `skrtrview.pub` absent → unauthenticated with warning

### Files Delivered
| File | Role |
|------|------|
| `src/core/skrauth.h` | Auth API — sign/verify contract (SSoA L1) |
| `src/core/skrauth.c` | ML-DSA-65 sign + verify implementation (SSoA L1) |
| `src/tools/skrtrkey.c` | Operator tool: keygen / sign / verify (SSoA L3) |
| `cli/build.rs` | liboqs dynamic link resolution for Rust CLI |
| `src/core/fabric.h` | FABRIC_MTU 8192 → 16384 (fits signed commands) |
| `src/daemon/skr8tr_sched.c` | Auth gate in handle_command + --pubkey flag |
| `cli/src/main.rs` | --key flag; OQS FFI signing; sign_command() |

### Operator Workflow
```bash
# One-time key generation:
bin/skrtrkey keygen
# → skrtrview.pub (1952 bytes, distribute to conductors)
# → ~/.skr8tr/signing.sec (4032 bytes, chmod 600, stays on operator machine)

# All future cluster operations:
skr8tr --key ~/.skr8tr/signing.sec up app.skr8tr
skr8tr --key ~/.skr8tr/signing.sec down my-app
skr8tr nodes    # unsigned — always open
skr8tr list     # unsigned — always open
```

### Verified
- `skrtrkey keygen` → correct key sizes, chmod 600 on secret key
- Unsigned SUBMIT → `ERR|UNAUTHORIZED — sign commands with: skr8tr --key ...`
- Signed SUBMIT via CLI → `OK|SUBMITTED|my-server|...` (accepted and processed)
- `skrtrkey verify skrtrview.pub <signed>` → VALID, bare command extracted

### Why Better Than k8s
k8s: kubeconfig bearer tokens (base64 of a secret string, effectively plaintext)
Skr8tr: ML-DSA-65 post-quantum signatures — key never leaves operator machine,
        no password on the wire, no TLS cert rotation, no YAML RBAC.

### Next Milestone
Phase 13: NixOS flake overlay for reproducible commercial builds
