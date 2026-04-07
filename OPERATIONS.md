# Skr8tr — Operations & Administration Guide

**Sovereign Workload Orchestrator**
No Docker. No YAML. No Kubernetes. Bare processes on bare machines.

---

## Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Building](#2-building)
3. [Starting the Stack](#3-starting-the-stack)
4. [Writing Manifests](#4-writing-manifests)
5. [CLI Reference](#5-cli-reference)
6. [Wire Protocol Reference](#6-wire-protocol-reference)
7. [Multi-Node Clusters](#7-multi-node-clusters)
8. [Auto-Scaling](#8-auto-scaling)
9. [Health Checks](#9-health-checks)
10. [Service Registry](#10-service-registry)
11. [HTTP Ingress](#11-http-ingress)
12. [Rolling Updates](#12-rolling-updates)
13. [Logs](#13-logs)
14. [Troubleshooting](#14-troubleshooting)
15. [Port Map](#15-port-map)

---

## 1. Architecture Overview

```
                                    ┌─────────────────────┐
  Internet / LB ──────────────────▶│  skr8tr_ingress      │  HTTP reverse proxy
                                    │  (TCP 80)            │  longest-prefix routing
                                    └──────────┬──────────┘
                                               │ Tower LOOKUP (UDP 7772)
┌─────────────┐       UDP 7771        ┌────────▼────────┐
│  skr8tr CLI │ ──────────────────── ▶│  skr8tr_sched   │  The Conductor
│  (Rust)     │◀──────────────────── │  (UDP 7771)     │  Masterless scheduler
└─────────────┘                       └────────┬────────┘
                                               │ LAUNCH via UDP 7775
                                    ┌──────────▼──────────┐
                                    │   skr8tr_node        │  Fleet node
                                    │   (UDP 7770 + 7775)  │  Runs workloads
                                    └──────────┬──────────┘
                                               │ REGISTER via UDP 7772
                                    ┌──────────▼──────────┐
                                    │   skr8tr_reg         │  The Tower
                                    │   (UDP 7772)         │  Service discovery
                                    └─────────────────────┘
```

**Components:**

| Binary             | Role                                        | Port(s)              |
|--------------------|---------------------------------------------|----------------------|
| `skr8tr_node`      | Fleet node — launches and monitors workloads | 7770 (mesh), 7775 (cmd) |
| `skr8tr_sched`     | Conductor — schedules, places, scales replicas | 7771              |
| `skr8tr_reg`       | Tower — service discovery registry          | 7772                 |
| `skr8tr_ingress`   | HTTP reverse proxy — routes to backend services | 80 (configurable) |
| `skr8tr_serve`     | Static file server (optional per-workload)  | configurable         |
| `skr8tr` (CLI)     | Operator interface                          | —                    |

**Design laws:**
- No master nodes. No SPOF. No leader election. Every node is a sovereign peer.
- PQC identity: each node generates an ephemeral ML-DSA-65 keypair at boot.
- Process-agnostic: any compiled binary, any language. Skr8tr launches it — that's all.
- No Docker. No OCI. No container runtime. Bare `fork()+exec()`.

---

## 2. Building

### Dependencies

- `gcc` with C23 support (`-std=gnu23`)
- `liboqs` ≥ 0.15.0 (post-quantum crypto)
- `rustup` + `cargo` (for CLI)
- `pthread` (system)

### Build daemons (C23)

```bash
# Standard (if liboqs is in pkg-config path):
make

# NixOS / nix-shell — set env vars manually (nix-shell --run segfaults):
OQS_INCDIR=$(find /nix/store -name "oqs.h" | head -1 | xargs dirname | xargs dirname)
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" | head -1 | xargs dirname)
C_INCLUDE_PATH="$OQS_INCDIR" LIBRARY_PATH="$OQS_LIBDIR" make
```

Produces in `bin/`:
```
bin/skr8tr_node
bin/skr8tr_sched
bin/skr8tr_reg
bin/skr8tr_serve
bin/skr8tr_ingress
bin/skrtrkey
```

### Build CLI (Rust)

```bash
cd cli
cargo build --release
# Binary: cli/target/release/skr8tr
```

Install to PATH:
```bash
cp cli/target/release/skr8tr ~/.local/bin/skr8tr
```

### Clean

```bash
make clean   # removes bin/
```

---

## 3. Starting the Stack

### Minimum stack (single node)

Start each daemon in order. Each runs in the foreground or background.

```bash
# 1. Start the Tower (service registry)
nohup bin/skr8tr_reg > /tmp/skr8tr_reg.log 2>&1 &

# 2. Start the Conductor (scheduler)
nohup bin/skr8tr_sched > /tmp/skr8tr_sched.log 2>&1 &

# 3. Start the Fleet Node (workload runner)
nohup bin/skr8tr_node > /tmp/skr8tr_node.log 2>&1 &

# 4. Verify everything is up
skr8tr ping
```

Expected output:
```
  conductor (127.0.0.1:7771)... ok
  tower     (127.0.0.1:7772)... ok
```

### NixOS / liboqs runtime

If liboqs is in the Nix store, set `LD_LIBRARY_PATH` before starting daemons:

```bash
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" | head -1 | xargs dirname)
export LD_LIBRARY_PATH="$OQS_LIBDIR"
```

### Stopping daemons

```bash
pkill skr8tr_node
pkill skr8tr_sched
pkill skr8tr_reg
```

Conductor state (active workload manifest paths) is persisted to
`/tmp/skr8tr_conductor.state` and replayed automatically on restart.

---

## 4. Writing Manifests

Manifests use the `.skr8tr` format — Skr8tr's sovereign alternative to YAML/k8s.
One `app` block per file. Multiple blocks are parsed as a linked list (for future
batch-submit support).

### Syntax

```
app <name>
  <key> <value>
  block {
    <key> <value>
  }
```

Indentation is two spaces. Braces delimit sub-blocks.

### Full field reference

```
app <name>                        # Required. Workload name (must be unique per mesh).

  # Workload type
  type  process                   # Default: long-running process (service)
  type  job                       # Run-to-completion binary
  type  wasm                      # WASM module via wasmtime
  type  vm                        # Full VM via QEMU or Firecracker

  # Process identity
  exec  /path/to/binary           # Absolute path to the executable (also: bin)
  args  --flag value              # Space-separated arguments (passed as argv[1..])

  # Resources
  port      8080                  # Port this workload listens on (for health + registry)
  replicas  2                     # Number of replicas to maintain
  ram       256mb                 # RAM limit hint (mb/gb accepted; informational for now)
  cpus      2                     # CPU core hint

  # Build (optional — run before exec)
  build {
    run  make                     # Build command(s); multiple run lines allowed
    out  bin/myapp                # Output binary path (exec is set to this if omitted)
  }

  # Static file serving (wraps skr8tr_serve)
  serve {
    static  ./dist                # Serve static files from this directory
    port    3000                  # Port for skr8tr_serve
  }

  # Health check
  health {
    check     GET /health 200     # Protocol: GET <path> <expected_status>
    interval  10s                 # How often to probe (default: 10s)
    timeout   2s                  # Probe timeout (default: 2s)
    retries   3                   # Consecutive failures before kill+relaunch
  }

  # Auto-scaling
  scale {
    min        1                  # Minimum replicas always running
    max        8                  # Maximum replicas (scale ceiling)
    cpu-above  80                 # Scale up when cpu% > this for 2+ heartbeats
    cpu-below  20                 # Scale down when cpu% < this for 4+ heartbeats
  }

  # Environment variables
  env {
    ENV_KEY  value
    DB_HOST  postgres.internal
  }

  # VM config (type vm only)
  vm {
    hypervisor  /usr/bin/qemu-system-x86_64   # or: firecracker
    kernel      /var/images/vmlinux
    rootfs      /var/images/rootfs.ext4
    vcpus       2
    memory      1024                          # MB
    net         user                          # or: tap:<iface>
    extra-args  -nographic -serial mon:stdio
  }
```

### Minimal examples

**Long-running service:**
```
app api-server
  exec /usr/local/bin/myapi
  args --port 8080 --db postgres.internal:5432
  port 8080
  replicas 2
  health {
    check  GET /healthz 200
  }
```

**One-shot job:**
```
app nightly-etl
  type job
  exec /opt/etl/run.sh
  args --date today
```

**Static site:**
```
app docs-site
  serve {
    static  /opt/docs/dist
    port    3000
  }
  replicas 1
```

**WASM module:**
```
app wasm-worker
  type wasm
  exec /opt/modules/worker.wasm
  replicas 4
```

**Firecracker microVM:**
```
app secure-sandbox
  type vm
  vm {
    hypervisor  /usr/bin/firecracker
    kernel      /var/fc/vmlinux
    rootfs      /var/fc/sandbox.ext4
    vcpus       1
    memory      512
    net         tap:fc-tap0
  }
```

---

## 5. CLI Reference

The `skr8tr` CLI communicates with the Conductor (port 7771), Tower (7772), and
directly with nodes (port 7775) via UDP. No config file needed for local use.

### Global flags

```
--conductor <host>   Conductor IP or hostname  (default: 127.0.0.1)
--tower <host>       Tower IP or hostname       (default: 127.0.0.1)
--timeout-ms <ms>    UDP response timeout       (default: 3000)
```

---

### `skr8tr ping`

Verify that Conductor and Tower are reachable.

```
skr8tr ping
```

```
  conductor (127.0.0.1:7771)... ok
  tower     (127.0.0.1:7772)... ok
```

Exit code `0` if both respond. Use for health checks and startup scripts.

---

### `skr8tr up <manifest>`

Submit a `.skr8tr` manifest to the Conductor for scheduling.

```
skr8tr up examples/my-server.skr8tr
```

```
  submitting /home/captain/skr8tr/examples/my-server.skr8tr... ok
  app    my-server
  node   b33c8ed0ea05839d78934cb8dbb1c5f4
```

The Conductor:
1. Parses the manifest
2. Selects the least-loaded eligible node
3. Sends `LAUNCH` to that node
4. Records placement + real PID
5. Enforces replica count going forward (dead replicas are relaunched)

Manifest path is stored by the Conductor. If the Conductor restarts, it replays
all active manifest paths from `/tmp/skr8tr_conductor.state`.

---

### `skr8tr down <app>`

Evict all replicas of a workload from the mesh.

```
skr8tr down my-server
```

```
  evicting my-server... ok
```

Sends `SIGTERM` to the process, waits 2 seconds, then `SIGKILL` if still running.
Auto-deregisters from the Tower. Removes from Conductor placement table.

---

### `skr8tr rollout <manifest>`

Perform a zero-downtime rolling update of a running workload.

```
skr8tr rollout examples/api-server.skr8tr
```

```
  rolling out /home/captain/skr8tr/examples/api-server.skr8tr... ok
  app     api-server
  status  new replicas launching, old replicas draining (8s settle)
```

The Conductor performs the rollout one replica at a time:
1. Parse the updated manifest — determines new binary path, args, env
2. For each existing replica:
   - Launch a **new-generation** replica on the same or best node
   - Wait 8 seconds (settle window — workload starts, health passes)
   - Send `SIGTERM` to the old-generation replica, wait 2s, then `SIGKILL`
3. After all replicas are replaced, increment the generation counter

**Net effect:** At least N−1 replicas are always live during rollout (N = replica count).
For single-replica apps there is a brief overlap, not a gap.

Requires `--key` when the Conductor has PQC auth enabled:
```bash
skr8tr --key ~/.skr8tr/signing.sec rollout api-server.skr8tr
```

---

### `skr8tr status`

Show live node metrics and all running workloads in one view.

```
skr8tr status
```

```
nodes:
  1 live node(s)

  NODE ID                             IP                CPU%   RAM FREE
  ------------------------------------------------------------------------
  b33c8ed0ea05839d78934cb8dbb1c5f4  192.168.68.51        0%   45683 MB

workloads:
  2 replica(s) running

  APP                               NODE ID                              PID
  ------------------------------------------------------------------------------
  api-server                        b33c8ed0ea05839d78934cb8dbb1c5f4      12481
  my-server                         b33c8ed0ea05839d78934cb8dbb1c5f4      12506
```

---

### `skr8tr nodes`

Show live node table only (metrics, no workloads).

```
skr8tr nodes
```

```
  2 live node(s)

  NODE ID                             IP                CPU%   RAM FREE
  ------------------------------------------------------------------------
  b33c8ed0ea05839d78934cb8dbb1c5f4  192.168.68.51        3%   45683 MB
  a1f2c3d4e5f6789012345678abcdef01  192.168.68.52       17%   31204 MB
```

Nodes that have not sent a heartbeat in 15 seconds are expired from the table.

---

### `skr8tr list`

Show all active workload placements (app + node + PID).

```
skr8tr list
```

```
  3 replica(s) running

  APP                               NODE ID                              PID
  ------------------------------------------------------------------------------
  api-server                        b33c8ed0...                          12481
  api-server                        a1f2c3d4...                          18294
  my-server                         b33c8ed0...                          12506
```

PID shows `pending` if the node has not yet confirmed launch.

---

### `skr8tr logs <app>`

Fetch the stdout/stderr ring buffer of a running app (last 200 lines).

```
skr8tr logs api-server
```

```
  logs for 'api-server' on 192.168.68.51 (12 lines captured):
  ----------------------------------------------------------------
  2026-04-06 05:30:12 [INFO] listening on :8080
  2026-04-06 05:30:12 [INFO] connected to postgres
  ...
```

The CLI auto-resolves which node the app is running on by querying the Conductor.
To query a node directly (bypassing Conductor lookup):

```
skr8tr logs api-server --node 192.168.68.51
```

---

### `skr8tr lookup <service>`

Resolve a service name to `ip:port` via the Tower (round-robin across replicas).

```
skr8tr lookup api-server
```

```
  api-server  →  192.168.68.51:8080
```

Workloads that set `port` in their manifest are auto-registered with the Tower on
launch and auto-deregistered on kill.

---

## 6. Wire Protocol Reference

All daemons speak simple pipe-delimited (`|`) UDP datagrams. Max datagram: 8192 bytes.
Useful for scripting, debugging, and direct integration.

### Conductor — port 7771

| Command | Response |
|---------|----------|
| `SUBMIT\|<absolute_manifest_path>` | `OK\|SUBMITTED\|<app>\|<node_id>` or `ERR\|<reason>` |
| `EVICT\|<app_name>` | `OK\|EVICTED\|<app_name>` or `ERR\|<reason>` |
| `ROLLOUT\|<absolute_manifest_path>` | `OK\|ROLLOUT\|<app>` or `ERR\|<reason>` |
| `LIST` | `OK\|LIST\|<n>\|<app:node_id:pid,...>` |
| `NODES` | `OK\|NODES\|<n>\|<node_id:ip:cpu%:ram_mb,...>` |
| `PING` | `OK\|PONG\|conductor` |

SUBMIT, EVICT, and ROLLOUT require a valid ML-DSA-65 signature when the Conductor
has `--pubkey` set. See [PQC Auth](#pqc-auth) in the starting stack section.

Signed wire format: `<cmd>|<unix_ts>|<6618-hex-char-ml-dsa-65-sig>`

**Example — send raw UDP (bash /dev/udp):**
```bash
# Query nodes
exec 3<>/dev/udp/127.0.0.1/7771
echo -n "NODES" >&3
sleep 0.5
cat <&3
exec 3>&-

# Submit a manifest
echo -n "SUBMIT|/absolute/path/to/app.skr8tr" > /dev/udp/127.0.0.1/7771

# Evict a workload
echo -n "EVICT|api-server" > /dev/udp/127.0.0.1/7771
```

### Fleet Node — port 7775 (command port)

| Command | Response |
|---------|----------|
| `LAUNCH\|name=<n>\|bin=<b>\|port=<p>` | `OK\|LAUNCHED\|<name>\|<pid>` or `ERR\|<reason>` |
| `LAUNCH\|name=<n>\|bin=<b>\|port=<p>\|args=<args>` | `OK\|LAUNCHED\|<name>\|<pid>` |
| `LAUNCH\|...\|env=<K=V,...>` | `OK\|LAUNCHED\|<name>\|<pid>` |
| `KILL\|<app_name>` | `OK\|KILLED\|<app_name>` or `ERR\|<reason>` |
| `STATUS` | `OK\|STATUS\|<n>\|<name:pid,...>` |
| `LOGS\|<app_name>` | `OK\|LOGS\|<name>\|<line_count>\|<lines...>` |
| `PING` | `OK\|PONG\|<node_id>` |

### Fleet Node — port 7770 (mesh / heartbeat)

Nodes broadcast heartbeats every 5 seconds. The Conductor listens on this port.

```
HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb>
```

### Tower — port 7772

| Command | Response |
|---------|----------|
| `REGISTER\|<name>\|<ip>\|<port>` | `OK\|REGISTERED\|<name>` |
| `DEREGISTER\|<name>\|<ip>\|<port>` | `OK\|DEREGISTERED\|<name>` |
| `LOOKUP\|<name>` | `OK\|LOOKUP\|<name>\|<ip>\|<port>` or `ERR\|NOT_FOUND` |
| `LIST` | `OK\|LIST\|<n>\|<name:ip:port:ttl_s,...>` |
| `PING` | `OK\|PONG\|tower` |

---

## 7. Multi-Node Clusters

Any machine running `skr8tr_node` joins the mesh automatically. The Conductor
discovers nodes through their heartbeat broadcasts on UDP port 7770.

### Adding a node

On the new machine:
```bash
# Copy the binary (or build from source)
scp bin/skr8tr_node captain@192.168.68.52:/usr/local/bin/skr8tr_node

# Start it — it will immediately begin heartbeating
ssh captain@192.168.68.52 "nohup skr8tr_node --tower 192.168.68.51 > /tmp/node.log 2>&1 &"
```

The Conductor (running on `192.168.68.51`) will detect the new node within one
heartbeat cycle (≤5 seconds). It becomes eligible for new workload placement
immediately.

### Conductor placement

The Conductor places workloads on the **least-loaded** eligible node by comparing
`ram_free_mb` at submission time. Future: CPU-weighted scoring.

### Remote conductor / tower

When the Conductor and Tower run on a different host from your workstation:

```bash
skr8tr --conductor 192.168.68.51 --tower 192.168.68.51 status
skr8tr --conductor 192.168.68.51 --tower 192.168.68.51 up app.skr8tr
```

Or export for the session:
```bash
alias skr8tr='skr8tr --conductor 192.168.68.51 --tower 192.168.68.51'
```

### Node expiry

A node that fails to heartbeat for 15 seconds is marked dead. The Conductor:
1. Removes it from the placement table
2. Relaunches its replicas on healthy nodes (if healthy nodes exist)

---

## 8. Auto-Scaling

The Conductor runs a rebalance loop every 5 seconds, reading node heartbeat metrics.

Configure scaling in the manifest `scale` block:

```
app api-server
  exec /usr/local/bin/myapi
  port 8080
  replicas 2        # starting count
  scale {
    min        1    # never go below 1 replica
    max        8    # never exceed 8 replicas
    cpu-above  80   # scale UP when any node CPU% > 80 for 2 consecutive heartbeats
    cpu-below  20   # scale DOWN when all node CPU% < 20 for 4 consecutive heartbeats
  }
```

**Scale-up trigger:** Any node hosting a replica reports `cpu_pct > cpu_above` for
2 consecutive heartbeats → add one replica on the least-loaded node.

**Scale-down trigger:** All nodes report `cpu_pct < cpu_below` for 4 consecutive
heartbeats → remove one replica (KILL sent to the most-loaded node's instance).

Scale events do not exceed the `min`/`max` bounds.

---

## 9. Health Checks

The node runs a health probe thread for every managed process.

```
health {
  check     GET /health 200   # HTTP GET probe — expects 200 OK
  interval  10s               # probe every 10 seconds
  timeout   2s                # probe timeout
  retries   3                 # consecutive failures before action
}
```

**Probe sequence:**
1. TCP connect to `127.0.0.1:<port>`
2. Send `GET <path> HTTP/1.0\r\n\r\n`
3. Read response, check status code
4. If timeout or wrong status: increment failure counter
5. After `retries` consecutive failures: `SIGTERM` → 2s grace → `SIGKILL`
6. Process is marked inactive; Conductor detects dead replica and relaunches

Currently supports HTTP GET. TCP-only checks (no path): set `check GET / 200`.

---

## 10. Service Registry

The Tower (`skr8tr_reg`) is a lightweight UDP service registry. Workloads with a
`port` field are registered automatically when launched and deregistered when killed.

**Manual registration** (for external processes or legacy services):
```bash
echo -n "REGISTER|myservice|192.168.68.100|9000" > /dev/udp/127.0.0.1/7772
```

**List all registered services:**
```bash
exec 3<>/dev/udp/127.0.0.1/7772
echo -n "LIST" >&3
sleep 0.5; cat <&3; exec 3>&-
```

```
OK|LIST|2|api-server:192.168.68.51:8080:28,api-server:192.168.68.52:8080:24
```

TTL resets on each REGISTER. Stale entries (no re-registration for TTL seconds)
are reaped automatically.

**Round-robin load balancing:** Each `LOOKUP` returns the next replica in sequence.
With 3 replicas of `api-server`, three successive lookups return `.51`, `.52`, `.53`.

---

## 11. HTTP Ingress

`skr8tr_ingress` is an HTTP/1.1 reverse proxy that sits in front of your workload
mesh. It resolves backends dynamically via the Tower — no static nginx config.

### Starting the ingress

```bash
# Single route: all traffic → api-server
nohup bin/skr8tr_ingress \
  --listen 80 \
  --tower 127.0.0.1 \
  --route /:api-server \
  > /tmp/ingress.log 2>&1 &

# Multiple routes (longest prefix wins):
nohup bin/skr8tr_ingress \
  --listen 80 \
  --tower 127.0.0.1 \
  --route /api:api-service \
  --route /static:static-site \
  --route /:frontend \
  > /tmp/ingress.log 2>&1 &
```

### Route matching

Routes are sorted by prefix length (descending). The longest matching prefix wins:

| Request path    | Route table                        | Matched backend |
|-----------------|------------------------------------|-----------------|
| `/api/users`    | `/api`, `/`                        | `api-service`   |
| `/static/a.js`  | `/api`, `/static`, `/`             | `static-site`   |
| `/`             | `/api`, `/static`, `/`             | `frontend`      |
| `/unknown`      | `/api` only                        | `404 Not Found` |

### Backend resolution

For each request the ingress:
1. Matches the route prefix → service name
2. Sends `LOOKUP|<service>` to the Tower (UDP 7772)
3. Tower returns `OK|LOOKUP|<name>|<ip>|<port>` (round-robin across replicas)
4. Ingress connects to `<ip>:<port>`, forwards request, proxies response
5. On backend failure: retries Tower lookup up to 3 times before returning `503`

The Tower's round-robin means each request can land on a different replica —
built-in load balancing with no configuration.

### Headers injected

```
X-Forwarded-For: <client_ip>
X-Real-IP: <client_ip>
```

### TLS

The ingress speaks plain HTTP internally. TLS termination belongs at the cloud
load balancer (AWS ALB, GCP HTTPS LB, Cloudflare Proxy). This is the standard
production pattern — terminate at the edge, run HTTP internally.

### Flags

```
--listen <port>          TCP port to accept connections on  (default: 80)
--tower  <host>          Tower hostname or IP               (default: 127.0.0.1)
--route  <prefix:svc>    Route rule. Repeat for multiple routes.
--workers <n>            Max concurrent connections         (default: 64)
```

---

## 12. Rolling Updates

Rolling updates replace replicas one at a time with no full downtime gap.

```bash
# After updating your binary / manifest:
skr8tr --key ~/.skr8tr/signing.sec rollout api-server.skr8tr
```

**What happens inside the Conductor:**
1. Reads updated manifest — new `exec`, `args`, `env` fields
2. Increments the internal generation counter for the workload
3. Spawns a background thread (one per rollout):
   - For each current (old-gen) replica:
     - Launch new-gen replica (LAUNCH to best node)
     - Wait `ROLLOUT_WAIT_S = 8` seconds
     - KILL old-gen replica
4. Returns `OK|ROLLOUT|<app>` immediately — rollout runs asynchronously

**During rollout:** `skr8tr list` shows both old-gen and new-gen replicas while
the settle window is active.

**Port collision safety:** If the old port is still bound when the new replica
starts, the Conductor selects a node that does not already have that port claimed.

**Abort / emergency:** To stop a misbehaving rollout, evict the workload entirely
and resubmit the previous manifest version:
```bash
skr8tr down api-server
skr8tr up api-server-v1.skr8tr
```

---

## 13. Logs

Each managed process has a per-process ring buffer: last 200 lines of combined
stdout/stderr. The ring is stored in-process on the node — no disk writes.

```bash
# Via CLI:
skr8tr logs api-server

# Via raw UDP (if you know the node IP):
exec 3<>/dev/udp/192.168.68.51/7775
echo -n "LOGS|api-server" >&3
sleep 0.5; cat <&3; exec 3>&-
```

To stream logs continuously, wrap in a loop:
```bash
while true; do skr8tr logs api-server; sleep 5; done
```

**Log persistence:** Logs are in-memory only. They are lost if the node restarts.
For durable logs, have your workload write to a file or push to a log aggregator.

---

## 14. Troubleshooting

### Workload shows `pending` in `skr8tr list`

The Conductor sent the LAUNCH command but the node's reply was not received.

Checks:
```bash
# Is the node running?
skr8tr nodes

# Is the node reachable on port 7775?
exec 3<>/dev/udp/<node_ip>/7775; echo -n "PING" >&3; sleep 0.5; cat <&3; exec 3>&-

# Check what the node thinks it's running:
exec 3<>/dev/udp/<node_ip>/7775; echo -n "STATUS" >&3; sleep 0.5; cat <&3; exec 3>&-
```

### Conductor says `ERR|no eligible nodes`

No live nodes in the table. Either no nodes are running, or they've expired.

```bash
skr8tr nodes          # should show at least one node
skr8tr ping           # confirms conductor is up
```

Start a node if none are listed:
```bash
nohup bin/skr8tr_node > /tmp/node.log 2>&1 &
sleep 6               # wait one heartbeat cycle
skr8tr nodes          # should now appear
```

### App exits immediately / shows in `list` then disappears

The binary is either:
- Missing or not executable — check the path in the manifest
- Exits with error — check logs: `skr8tr logs <app>`
- Missing required `args` — add `args` to the manifest (e.g. `args 3600` for `sleep`)

Test the binary manually on the node before submitting:
```bash
ssh <node_ip> "/absolute/path/to/bin --your --args"
```

### `make` fails: `oqs.h: No such file or directory`

liboqs headers not found by pkg-config. Use env vars:
```bash
OQS_INCDIR=$(find /nix/store -name "oqs.h" | head -1 | xargs dirname | xargs dirname)
OQS_LIBDIR=$(find /nix/store -name "liboqs.so" | head -1 | xargs dirname)
C_INCLUDE_PATH="$OQS_INCDIR" LIBRARY_PATH="$OQS_LIBDIR" make
```

### Conductor loses state after restart

State is written to `/tmp/skr8tr_conductor.state`. If `/tmp` was cleared, workloads
must be resubmitted. If the Conductor is restarted without clearing `/tmp`, it
automatically replays all manifest paths on startup.

### `skr8tr` CLI: `no response from 127.0.0.1:7771`

Conductor is not running or is on a different host.
```bash
# Check if it's running locally:
pgrep -a skr8tr_sched

# Start it:
nohup bin/skr8tr_sched > /tmp/sched.log 2>&1 &

# If running on a remote host:
skr8tr --conductor 192.168.68.51 ping
```

---

## 15. Port Map

| Port | Protocol | Component        | Purpose                                            |
|------|----------|------------------|----------------------------------------------------|
| 80   | TCP      | skr8tr_ingress   | HTTP ingress reverse proxy (configurable)          |
| 7770 | UDP      | All nodes        | Heartbeat mesh — `HEARTBEAT\|id\|cpu\|ram` broadcasts |
| 7771 | UDP      | Conductor        | Operator commands: SUBMIT, EVICT, ROLLOUT, LIST    |
| 7772 | UDP      | Tower            | Service registry: REGISTER, LOOKUP, LIST           |
| 7774 | TCP      | skr8tr_serve     | Static file server (port is configurable)          |
| 7775 | UDP      | Fleet nodes      | Node commands: LAUNCH, KILL, STATUS, LOGS          |

All ports are configurable at binary startup. The defaults above are the mesh
convention — change them consistently across all daemons if you need different ports.

---

*Skr8tr — The k8s Killer. Sovereign. Masterless. 20KB control plane.*
