# SESSION_STATE.md — Skr8tr Sovereign Session Handoff
## Branch: feature/sovereign-multiplication-logic
## Date: 2026-04-06
## Session end: ~21:00

---

### Last Completed Task

**Phase 13/14/15 COMPLETE — Production Parity with k8s**

All three production features built, compiled clean, documented:

1. **Port Collision Tracking** — `used_ports[64]` per NodeEntry; `node_least_loaded_for_port(port)` skips nodes with port already bound; port claimed on LAUNCHED, released on KILL/EVICT.

2. **Rolling Update (ROLLOUT)** — `ROLLOUT|<manifest>` wire command (ML-DSA-65 signed); rollout_thread: launch new-gen → 8s settle → kill old-gen, one replica at a time; `skr8tr rollout <manifest>` CLI subcommand.

3. **HTTP Ingress (`skr8tr_ingress`)** — ~320-line C23 reverse proxy; longest-prefix route matching; dynamic Tower LOOKUP per request; MAX_RETRY=3 failover; X-Forwarded-For; bidirectional select() proxy; 503/404 error responses.

**Build status:** `make` clean, `cargo build --release` clean.

### Next Task (resume here)

**Phase 16 — End-to-End Integration Test**

Run a full cluster locally and exercise every feature:

1. Start the stack:
   ```bash
   OQS_LIBDIR=$(find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname)
   export LD_LIBRARY_PATH="$OQS_LIBDIR"
   nohup bin/skr8tr_reg   > /tmp/sk_reg.log   2>&1 &
   nohup bin/skr8tr_node  > /tmp/sk_node.log  2>&1 &
   LD_LIBRARY_PATH="$OQS_LIBDIR" nohup bin/skr8tr_sched --pubkey ./skrtrview.pub > /tmp/sk_sched.log 2>&1 &
   nohup bin/skr8tr_ingress --listen 8080 --tower 127.0.0.1 --route /:my-server > /tmp/ingress.log 2>&1 &
   ```
2. Submit a workload and exercise rollout:
   ```bash
   LD_LIBRARY_PATH="$OQS_LIBDIR" cli/target/release/skr8tr --key ~/.skr8tr/signing.sec up examples/my-server.skr8tr
   LD_LIBRARY_PATH="$OQS_LIBDIR" cli/target/release/skr8tr --key ~/.skr8tr/signing.sec rollout examples/my-server.skr8tr
   curl http://127.0.0.1:8080/    # should proxy to my-server
   ```
3. Record any bugs or missing features for Phase 17.

### Open Blockers

- nix-shell segfaults — build with env vars instead (see run commands below)
- CLI must be run with `LD_LIBRARY_PATH` set until flake lands
- `skrtrview.pub` is gitignored (site-specific) — must regenerate per installation with `skrtrkey keygen`

### Run Commands

```bash
cd /home/sbaker/skr8tr
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname)

# Build daemons:
C_INCLUDE_PATH="$(find /nix/store -name "oqs.h" 2>/dev/null | head -1 | xargs dirname | xargs dirname)/include" \
  LIBRARY_PATH="$OQS_LIBDIR" make

# Build CLI:
OQS_LIBDIR="$OQS_LIBDIR" /home/sbaker/.cargo/bin/cargo build --release --manifest-path cli/Cargo.toml

# Start stack:
export LD_LIBRARY_PATH="$OQS_LIBDIR"
nohup bin/skr8tr_reg   > /tmp/sk_reg.log   2>&1 &
nohup bin/skr8tr_node  > /tmp/sk_node.log  2>&1 &
LD_LIBRARY_PATH="$OQS_LIBDIR" nohup bin/skr8tr_sched --pubkey ./skrtrview.pub > /tmp/sk_sched.log 2>&1 &
nohup bin/skr8tr_ingress --listen 8080 --tower 127.0.0.1 --route /:my-server > /tmp/ingress.log 2>&1 &

# Keygen (one-time per installation):
LD_LIBRARY_PATH="$OQS_LIBDIR" bin/skrtrkey keygen

# CLI usage (signed):
LD_LIBRARY_PATH="$OQS_LIBDIR" cli/target/release/skr8tr --key ~/.skr8tr/signing.sec up examples/my-server.skr8tr
LD_LIBRARY_PATH="$OQS_LIBDIR" cli/target/release/skr8tr --key ~/.skr8tr/signing.sec rollout examples/my-server.skr8tr
LD_LIBRARY_PATH="$OQS_LIBDIR" cli/target/release/skr8tr --key ~/.skr8tr/signing.sec down my-server

# Read-only (no key needed):
cli/target/release/skr8tr nodes
cli/target/release/skr8tr list
cli/target/release/skr8tr ping
```

### Files Modified This Session

- `src/daemon/skr8tr_sched.c` — port collision tracking, ROLLOUT command, rollout_thread, forward declarations, use-after-free fix
- `src/daemon/skr8tr_ingress.c` — NEW: HTTP ingress reverse proxy
- `cli/src/main.rs` — `rollout` subcommand, `cmd_rollout()` handler
- `Makefile` — `skr8tr_ingress` build target
- `OPERATIONS.md` — sections 11 (ingress), 12 (rolling updates), port map, CLI reference
- `MILESTONES.md` — Phase 13/14/15 entry
- `SESSION_STATE.md` — this file
