<div align="center">
  <img src="docs/skr8tr.svg" alt="skr8tr" width="480" />

  <br />
  <br />

  **The sovereign workload orchestrator.**<br />
  No Docker. No YAML. No Kubernetes. 5 MB control plane. Post-quantum auth.

  <br />

  [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
  [![Language](https://img.shields.io/badge/language-C23%20%2B%20Rust-orange.svg)]()
  [![Auth](https://img.shields.io/badge/auth-ML--DSA--65%20PQC-cyan.svg)]()
  [![Website](https://img.shields.io/badge/website-skr8tr.online-green.svg)](https://skr8tr.online)

</div>

---

## What is Skr8tr?

Skr8tr is a production-grade workload orchestrator built from the ground up in C23 and Rust. It runs microservices, APIs, workers, static sites, and WASM modules on bare metal or cloud clusters — without Docker, without Kubernetes, and without the 600 MB of control plane overhead they bring.

**Three binaries. Any number of nodes. Zero config files.**

```bash
# Deploy an app
skr8tr --key ~/.skr8tr/signing.sec up api-server.skr8tr

# Roll out a new version with zero downtime
skr8tr --key ~/.skr8tr/signing.sec rollout api-server-v2.skr8tr

# Check your cluster
skr8tr nodes
skr8tr list
skr8tr logs api-server
```

---

## Why Not Kubernetes?

| | Kubernetes | Skr8tr |
|---|---|---|
| Control plane size | ~620 MB | ~5 MB |
| Binaries to run a cluster | 7+ (etcd, apiserver, scheduler, ...) | 3 |
| Auth model | base64 bearer tokens (plaintext equivalent) | ML-DSA-65 post-quantum signatures |
| Deploy a service | 6 resource types, 200+ lines of YAML | 1 manifest file, 10 lines |
| Rolling update | ReadinessProbes + PodDisruptionBudgets + strategy YAML | `skr8tr rollout app.skr8tr` |
| HTTP ingress | nginx-ingress Helm chart (90 MB container) | built-in, 320 lines of C23 |
| New node joins | ~3 minutes (kubelet cert approval) | < 6 seconds (first heartbeat) |
| Time-to-running after deploy | ~45s (image pull + pod scheduling) | ~1.2s (fork + exec) |

Read the full technical breakdown: [Why We Killed Our Kubernetes Cluster](https://skr8tr.online/blog/why-we-killed-kubernetes.html)

---

## Architecture

```
                          Internet / Cloud LB
                                 │ HTTPS
                      ┌──────────▼──────────┐
                      │   skr8tr_ingress     │  HTTP reverse proxy
                      │   (TCP :80)          │  longest-prefix routing
                      └──────────┬──────────┘
                                 │ Tower LOOKUP (UDP)
┌─────────────┐  UDP :7771  ┌───▼─────────────┐
│  skr8tr CLI │ ──────────▶ │  skr8tr_sched   │  The Conductor
│  (Rust)     │ ◀────────── │  ML-DSA-65 auth │  Masterless scheduler
└─────────────┘             └───┬─────────────┘
                                │ LAUNCH via UDP :7775
                         ┌──────▼──────┐
                         │ skr8tr_node │  Fleet node (×N)
                         │ fork + exec │  Runs your workloads
                         └──────┬──────┘
                                │ REGISTER via UDP :7772
                         ┌──────▼──────┐
                         │  skr8tr_reg │  The Tower
                         │  (UDP :7772)│  Service discovery
                         └─────────────┘
```

**No etcd. No leader election. No SPOF. Every node is a sovereign peer.**

---

## Features

- **Post-Quantum Auth** — Every mutating command is ML-DSA-65 signed (NIST FIPS 204). The signing key never touches the wire. Replay protection built in.
- **Rolling Updates** — `skr8tr rollout app.skr8tr` — launch new replicas, health-probe gated, drain old ones. One command, zero downtime.
- **HTTP/HTTPS Ingress** — Built-in TLS-terminating reverse proxy. Longest-prefix route matching. No nginx config. OpenSSL opt-in.
- **Masterless Mesh** — UDP heartbeat discovery. Add a node by running the binary. No certificate approval, no kubeconfig rotation.
- **Service Discovery** — Built-in Tower registry. Auto-register on launch, auto-deregister on kill. Round-robin across replicas.
- **Health Checks** — HTTP GET probes, configurable interval/timeout/retries. Dead replicas killed and relaunched automatically.
- **Log Streaming** — Per-process ring buffer. `skr8tr logs app` auto-resolves to the right node. No SSH required.
- **Prometheus Metrics** — Every node exposes `/metrics` on port 9100. Scrape with any standard Prometheus stack.
- **Remote Exec** — `skr8tr exec app <cmd>` — run a shell command inside any running workload across the mesh. No SSH.
- **Restart Policy** — `restart always | on-failure | never` per manifest. Conductor rebalances on `never`.
- **Persistent Volumes** — `volume {}` block in manifest — host paths injected as env vars post-fork.
- **Graceful Drain** — Configurable `drain Ns` SIGTERM→SIGKILL window per workload.
- **Secret Injection** — `secret {}` block — injected post-fork, never logged, never in UDP wire commands.

> **Enterprise features** (RBAC, SSO, audit ledger, HTTP/2, multi-tenant, autoscale) →
> [skr8tr.online/#enterprise](https://skr8tr.online/#enterprise)

---

## Manifest Format

Skr8tr's sovereign alternative to Kubernetes YAML. No anchors. No implicit types. No CRDs.

```
app api-server
  exec     /usr/local/bin/myapi
  args     --port 8080 --db postgres.internal:5432
  port     8080
  replicas 3

  health {
    check    GET /healthz 200
    interval 10s
    retries  3
  }
```

---

## Quick Start

### Dependencies

- `gcc` with C23 support (`-std=gnu23`)
- `liboqs` ≥ 0.15.0 (post-quantum crypto — [open-quantum-safe.org](https://openquantumsafe.org))
- `openssl` ≥ 3.0 (TLS ingress)
- `rustup` + `cargo`
- `pthread` (system)

### Build

```bash
git clone https://github.com/NixOSDude/skr8tr
cd skr8tr

# Build all C23 daemons
make

# Build the CLI
cd cli && cargo build --release
cp target/release/skr8tr ~/.local/bin/skr8tr
```

### Generate your keypair (once per operator machine)

```bash
bin/skrtrkey keygen
# → skrtrview.pub    (1952 bytes) — copy to conductor host
# → ~/.skr8tr/signing.sec  (4032 bytes, chmod 600) — stays on your machine
```

### Start the cluster

```bash
# On the conductor host:
nohup bin/skr8tr_reg   > /tmp/tower.log   2>&1 &
nohup bin/skr8tr_sched --pubkey skrtrview.pub > /tmp/sched.log 2>&1 &

# On every worker node:
nohup bin/skr8tr_node > /tmp/node.log 2>&1 &

# Optional: HTTP ingress on port 80
nohup bin/skr8tr_ingress \
  --listen 80 --tower <conductor-host> \
  --route /api:api-service \
  --route /:frontend \
  > /tmp/ingress.log 2>&1 &
```

### Deploy your first workload

```bash
skr8tr ping                                                   # verify cluster up
skr8tr --key ~/.skr8tr/signing.sec up examples/my-server.skr8tr
skr8tr list
skr8tr logs my-server
```

Full documentation: [OPERATIONS.md](OPERATIONS.md)

---

## Port Map

| Port | Protocol | Component | Purpose |
|------|----------|-----------|---------|
| 80 | TCP | `skr8tr_ingress` | HTTP ingress reverse proxy |
| 7770 | UDP | All nodes | Heartbeat mesh |
| 7771 | UDP | `skr8tr_sched` | Conductor — SUBMIT, EVICT, ROLLOUT, LIST |
| 7772 | UDP | `skr8tr_reg` | Tower — REGISTER, LOOKUP |
| 7775 | UDP | `skr8tr_node` | Node commands — LAUNCH, KILL, LOGS |

---

## Professional Services

Skr8tr is open source and free to use. Need help deploying it or integrating it into your infrastructure?

| Service | Description |
|---------|-------------|
| **Cluster Setup** | We design and deploy your Skr8tr cluster — bare metal, cloud, or hybrid. Conductor HA, node fleet, ingress, PQC key provisioning. |
| **Migration from k8s** | Full migration from Kubernetes: manifest conversion, ingress routing, CI/CD pipeline rework. |
| **Architecture Review** | 2-hour deep dive into your current orchestration stack. Written recommendations delivered within 48 hours. |
| **Training & Workshops** | Half-day or full-day team workshops. C23 systems programming, Skr8tr operations, post-quantum security fundamentals. |
| **Ongoing Support** | Monthly retainer — SLA-backed response times, priority bug fixes, direct access. |

**Email:** [scott.bakerphx@gmail.com](mailto:scott.bakerphx@gmail.com) · **Website:** [skr8tr.online](https://skr8tr.online)

We respond to all serious inquiries within 24 hours.

---

## License

```
Copyright 2026 Scott Baker

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

See [LICENSE](LICENSE) for the full license text.

---

## Contributing

Issues and pull requests are welcome. Please open an issue before starting significant work so we can discuss the approach.

- Bug reports: [GitHub Issues](https://github.com/NixOSDude/skr8tr/issues)
- Security issues: email directly — do not open a public issue for vulnerabilities
- Feature requests: open an issue with the `enhancement` label

---

<div align="center">
  <strong>No Docker. No YAML. No k8s.</strong><br />
  <em>Sovereign infrastructure for teams who know what they are running.</em>
</div>
