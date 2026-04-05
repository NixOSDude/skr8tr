# SESSION_STATE.md — Skr8tr Session Handoff
## Branch: main
## Date: 2026-04-05

---

### Last Completed Task

Sovereign scaffold initialized. Repo live at gitea/skr8tr.
CLAUDE.md, MILESTONES.md, SESSION_STATE.md, shell.nix, Makefile,
examples, and .gitignore written. First commit pushed.

### Next Task (resume here)

**Phase 1 — SkrtrMaker parser + skr8tr_node.c**

STEP 1: Write `src/parser/skrmaker.c`
  - Parse .skr8tr manifests into LambProc descriptor structs
  - Support: app, port, replicas, ram, build, bin, serve/static,
    health (GET url code), scale (min/max/cpu-above)
  - Both indent and brace syntax valid
  - SSoA LEVEL 1

STEP 2: Write `src/daemon/skr8tr_node.c`
  - UDP listener on port 7770
  - Receive LAUNCH|<manifest_json> → fork/exec the process
  - Receive KILL|<app_name> → SIGTERM the process
  - Broadcast HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb> every 5s
  - ML-DSA-65 ephemeral identity (port liboqs from LambdaC)
  - SSoA LEVEL 1

STEP 3: Wire skrmaker.c → skr8tr_node.c
  - `skr8tr_node --run react-app.skr8tr` parses and launches immediately
  - Verify react-app.skr8tr example deploys a static file server

### Open Blockers
- None at scaffold stage

### Files In This Repo
- CLAUDE.md
- MILESTONES.md
- SESSION_STATE.md
- shell.nix
- Makefile
- .gitignore
- examples/react-app.skr8tr
- examples/lambdac-job.skr8tr
- src/ (directories created, no C23 files yet)
- cli/ (directory created, no Rust files yet)
