# Skr8tr Enterprise Module Laws

This directory contains **proprietary enterprise-only source code**.

## What lives here

| File | Purpose |
|------|---------|
| `skr8tr_rbac.c/h` | RBAC gateway — ML-DSA-65 signed role enforcement |
| `skr8tr_sso.c/h` | SSO/OIDC bridge — JWT verification, session mapping |
| `skr8tr_conductor_mt.c/h` | Multi-tenant Conductor — tenant isolation + quota enforcement |
| `skr8tr_autoscale.c/h` | Autoscale engine — replica scaling from CPU/RAM telemetry |
| `skr8tr_audit.c/h` | Cryptographic audit ledger — SHA-256 chained entries |
| `skr8tr_syslog.c/h` | Syslog forwarder — RFC 5424 structured event emission |

## Build

These modules are compiled only when `ENTERPRISE=1` is passed to make:

```
make ENTERPRISE=1
```

The Makefile adds `-DENTERPRISE -I./src/enterprise` and links the enterprise
objects into `skr8tr_sched` (conductor-mt, autoscale, audit, syslog) and
builds `skr8tr_rbac` and `skr8tr_sso` as standalone binaries.

## Repo ownership — Federation Law 16

Every file in this directory carries `// REPO: enterprise` in its header.
This tag means the file goes **exclusively** to `gitea/skr8tr-enterprise`.
It must **never** appear in `github.com/NixOSDude/skr8tr`.

## Isolation rules — Federation Laws 15, 16, 17

**Law 15 — Enterprise Code Isolation**
`src/enterprise/` is tracked ONLY on the `enterprise` remote
(`gitea@192.168.68.50:gitea/skr8tr-enterprise.git`).
The `origin` and `github` remotes have been removed from the local repo
to make accidental direct pushes impossible.

**Law 16 — File Ownership Tagging**
Line 2 of every file header must declare `// REPO: enterprise`.
Before any commit, verify no enterprise-tagged file is staged for a
non-enterprise push.

**Law 17 — GitHub Push Protocol**
GitHub NEVER receives a direct push from the local `main` branch.
GitHub pushes use a temp clone + filter-repo pipeline:

```bash
git clone /home/sbaker/skr8tr /tmp/skr8tr-gh-clean
cd /tmp/skr8tr-gh-clean
git-filter-repo \
  --path src/enterprise --invert-paths \
  --path enterprise-flake.nix --invert-paths \
  --path agent --invert-paths \
  --path CLAUDE.md --invert-paths \
  --path shell.nix --invert-paths \
  --path .gitignore --invert-paths \
  --force
git remote add github git@github.com:NixOSDude/skr8tr.git
git push github main --force
rm -rf /tmp/skr8tr-gh-clean
```

## Remotes

| Remote | URL | What it receives |
|--------|-----|-----------------|
| `enterprise` | `gitea@192.168.68.50:gitea/skr8tr-enterprise.git` | Everything — OSS + enterprise |
| ~~`github`~~ | removed from local repo | OSS only, via temp clone |
| ~~`origin`~~ | removed from local repo | — |
