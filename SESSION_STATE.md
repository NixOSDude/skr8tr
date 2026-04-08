# SESSION_STATE.md — Skr8tr

## Date: 2026-04-07

## Branch: main

## Status: CLEAN — Git system fully documented. All laws updated. Blog corrected.

---

## Git System — Exact State (do not summarize, read this verbatim)

### Remotes

```
enterprise  →  gitea@192.168.68.50:gitea/skr8tr-enterprise.git
               Auth: ed25519 SSH key in Gitea settings — always use gitea@ user
               Receives: ALL files (OSS + enterprise + agent/ + CLAUDE.md + SESSION_STATE.md)

github      →  git@github.com:NixOSDude/skr8tr.git
               Auth: SSH key (NixOSDude)
               Receives: OSS files ONLY
               NEVER receives: src/enterprise/, agent/, shell.nix, CLAUDE.md, SESSION_STATE.md
```

No `origin` remote exists. Removed to prevent accidents.

### Current HEAD State

- local/main = enterprise/main — has enterprise files committed (src/enterprise/, agent/)
- github/main — OSS only, behind enterprise/main on enterprise commits — CORRECT

### Exact Commands for Every Push Scenario

**OSS-only changes (src/core, src/daemon, src/parser, docs/, cli/, flake.nix, etc.):**
```bash
# 1. Commit to local/enterprise as normal:
git add <specific files>         # NEVER git add -A or git add .
git commit -m "OSS: ..."
git push enterprise main

# 2. Push to GitHub via tmp/clone protocol (local/main has enterprise commits — never push directly):
git clone git@github.com:NixOSDude/skr8tr.git /tmp/skr8tr-oss
cp ~/skr8tr/<changed-file> /tmp/skr8tr-oss/<path>/
cd /tmp/skr8tr-oss
git add <file>
git commit -m "OSS: ..."
git push origin main
rm -rf /tmp/skr8tr-oss
```

**Enterprise changes (src/enterprise/, agent/):**
```bash
git add -f src/enterprise/       # force past gitignore
git add -f agent/
git add -f shell.nix             # if modified
git commit -m "Enterprise: ..."
git push enterprise main         # ONLY — NEVER push to github
```

**Session handoff (MILESTONES.md + SESSION_STATE.md):**
```bash
git add MILESTONES.md
git add -f SESSION_STATE.md      # SESSION_STATE.md is gitignored — must force-add
git commit -m "Session handoff — [date]"
git push enterprise main         # ONLY
```

**Website/blog (docs/ only):**
```bash
# Enterprise:
git add docs/ && git commit -m "docs: ..." && git push enterprise main
# GitHub (tmp/clone):
git clone git@github.com:NixOSDude/skr8tr.git /tmp/skr8tr-oss
cp -r ~/skr8tr/docs/ /tmp/skr8tr-oss/
cd /tmp/skr8tr-oss && git add docs/ && git commit -m "docs: ..." && git push origin main
rm -rf /tmp/skr8tr-oss
```

### What NEVER To Do

- `git add -A` or `git add .`
- `git push github main` directly from local — local/main has enterprise commits in history
- Copy enterprise files into the /tmp clone
- `git filter-branch`, `git filter-repo`, `git rebase` without Captain's explicit authorization
- Change remote URLs without Captain's authorization

### Verify Remotes
```bash
git remote -v
# enterprise  gitea@192.168.68.50:gitea/skr8tr-enterprise.git (fetch)
# enterprise  gitea@192.168.68.50:gitea/skr8tr-enterprise.git (push)
# github      git@github.com:NixOSDude/skr8tr.git (fetch)
# github      git@github.com:NixOSDude/skr8tr.git (push)
```

---

## Last Completed Tasks

### Git System Documentation (this session)
- Diagnosed and documented the exact two-remote workflow
- enterprise = full codebase; github = OSS only; gitignore is the boundary
- All exact commands written into CLAUDE.md Git Protocol section, MILESTONES.md, SESSION_STATE.md

### Blog Fix — why-we-killed-kubernetes.html
- Removed "Kubernetes is the right tool for that specific problem" concession
- Rewrote "No container runtime" bullet: no-OCI is a design choice, not a limitation
- Rewrote Design Decisions closing: enterprise RBAC handles multi-tenant
- Rewrote "The Source" closing: Skr8tr IS the right tool, full stop
- All other blog posts audited — no other k8s concessions found

### Federation Laws Updated (CLAUDE.md)
- Law 13: No VPS Law
- Law 15: gitignore protects enterprise — NEVER filter-branch without authorization
- Law 17: GitHub push = `git push github main` directly — no tmp clone, no filter tools
- Law 18: Website Deployment Law — GitHub Pages, NO VPS
- Law 19: Website & Blog Content Law — show what it does, never how it's built
- Law 20: Enterprise Repo Law — enterprise gets everything; github gets OSS only
- Law 21: File Ownership Tagging (corrected ownership table)
- Git Protocol section: rewritten with exact step-by-step for every scenario

### enterprise-flake.nix → src/enterprise/flake.nix
- Moved to src/enterprise/ — covered by gitignore automatically
- .gitignore updated (removed stale enterprise-flake.nix entry)

### LambdaC — lambbook-serve
- Python HTTP server replaced with sovereign Rust binary (lambbook-serve/src/main.rs)
- Federation Law #5 compliance restored

---

## OSS State
- All 12 OSS components complete and building clean
- GitHub: NixOSDude/skr8tr — clean, no enterprise code (gitignored)
- GitHub Pages: skr8tr.online live and correct
- Blog: all posts free of k8s concessions, no enterprise source code exposed

## Enterprise State
- All 8 enterprise modules complete and building clean (make ENTERPRISE=1)
- Enterprise remote: gitea@192.168.68.50:gitea/skr8tr-enterprise.git — full codebase
- Enterprise source in src/enterprise/ — gitignored in normal flow, protected from GitHub

---

## Next Priority
- Resume any pending work Captain directs
