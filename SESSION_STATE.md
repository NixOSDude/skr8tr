# SESSION_STATE.md — Skr8tr Sovereign Session Handoff
## Branch: main
## Date: 2026-04-06
## Session end: ~19:30

---

### Last Completed Task

**Phase 12 COMPLETE — ML-DSA-65 Sovereign Auth Layer**

PQC command signing fully operational. Verified end-to-end:
- Unsigned SUBMIT → `ERR|UNAUTHORIZED`
- Signed SUBMIT (CLI `--key`) → `OK|SUBMITTED|...` accepted and processed
- `skrtrkey verify` round-trip → VALID

Key files:
- `skrtrview.pub` (1952 bytes) — in skr8tr project root, gitignored (site-specific)
- `~/.skr8tr/signing.sec` (4032 bytes, chmod 600) — on operator machine

### Next Task (resume here)

**Phase 13 — NixOS Flake Overlay for Reproducible Commercial Builds**

Skr8tr commercial model: $19.99/site/month, closed-source.
Hermetic builds via NixOS flake — pinned gcc, liboqs, rustup, all deps.

1. Create `flake.nix` wrapping `shell.nix` with flake lock
2. Create `overlay/skr8tr.nix` derivation for all C23 daemons + Rust CLI
3. Test: `nix build .#skr8tr_sched` → hermetic binary
4. Document install story in OPERATIONS.md

### Open Blockers

- nix-shell segfaults — build with env vars instead:
  ```bash
  OQS_INCDIR=$(find /nix/store -name "oqs.h" | head -1 | xargs dirname | xargs dirname)
  OQS_LIBDIR=$(find /nix/store -name "liboqs.so" | head -1 | xargs dirname)
  C_INCLUDE_PATH="$OQS_INCDIR" LIBRARY_PATH="$OQS_LIBDIR" make
  OQS_LIBDIR="$OQS_LIBDIR" /home/sbaker/.cargo/bin/cargo build --release
  ```
- CLI must be run with LD_LIBRARY_PATH set (same OQS_LIBDIR) until flake lands

### Run Commands

```bash
cd /home/sbaker/skr8tr
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" 2>/dev/null | head -1 | xargs dirname)

# Start daemons:
nohup bin/skr8tr_reg   > /tmp/sk_reg.log   2>&1 &
nohup bin/skr8tr_node  > /tmp/sk_node.log  2>&1 &
LD_LIBRARY_PATH="$OQS_LIBDIR" nohup bin/skr8tr_sched > /tmp/sk_sched.log 2>&1 &

# Keygen (one-time per installation):
LD_LIBRARY_PATH="$OQS_LIBDIR" bin/skrtrkey keygen

# CLI usage (signed):
LD_LIBRARY_PATH="$OQS_LIBDIR" \
  cli/target/release/skr8tr --key ~/.skr8tr/signing.sec up examples/my-server.skr8tr

# CLI usage (read-only — no key needed):
cli/target/release/skr8tr nodes
cli/target/release/skr8tr list
cli/target/release/skr8tr ping
```

### Files Modified This Session

- `src/core/fabric.h` — FABRIC_MTU 8192 → 16384
- `src/core/skrauth.h` — NEW: auth API header
- `src/core/skrauth.c` — NEW: ML-DSA-65 sign/verify implementation
- `src/tools/skrtrkey.c` — NEW: keygen/sign/verify tool
- `src/daemon/skr8tr_sched.c` — auth gate + --pubkey flag
- `cli/Cargo.toml` — build = "build.rs"
- `cli/build.rs` — NEW: liboqs link resolution
- `cli/src/main.rs` — --key flag + OQS FFI signing
- `Makefile` — skrtrkey target + skrauth.c in sched build
- `.gitignore` — skrtrview.pub excluded (site-specific)
- `MILESTONES.md` — Phase 12 entry
- `SESSION_STATE.md` — this file
