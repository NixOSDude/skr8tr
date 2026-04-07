/*
 * skr8tr_sched.c — The Conductor — Masterless Capacity Scheduler
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. This is the scheduling brain of the mesh.
 *
 * Responsibilities:
 *   - Listens on UDP 7771 (Conductor channel) for operator commands
 *   - Listens on UDP 7770 (mesh) for HEARTBEAT datagrams from all nodes
 *   - Maintains a live node table: node_id → { ip, cpu%, ram_free_mb, last_seen }
 *   - Expires nodes silent for >NODE_EXPIRY_S seconds (dead node detection)
 *   - Accepts workload submissions, assigns to least-loaded eligible node
 *   - Tracks replica placement: which node runs which replica of which app
 *   - Detects dead replicas (node expired) and relaunches on a healthy node
 *   - Scale-up: cpu% > cpu_above for ≥2 consecutive heartbeats → add replica
 *   - Scale-down: cpu% < cpu_below for ≥4 consecutive heartbeats → remove replica
 *   - No leader election. No SPOF. Stateless — any node can run the Conductor.
 *
 * Wire protocol — port 7771:
 *   SUBMIT|<manifest_path>          → schedule workload from manifest file
 *   LIST                            → list all tracked workloads + placements
 *   NODES                           → list all live nodes + metrics
 *   EVICT|<app_name>                → remove all replicas of an app
 *
 * Responses:
 *   OK|SUBMITTED|<app_name>|<node_id>
 *   OK|LIST|<n>|<app:node:pid,...>
 *   OK|NODES|<n>|<node_id:ip:cpu:ram,...>
 *   OK|EVICTED|<app_name>
 *   ERR|<reason>
 *
 * SSoA LEVEL 1 — Foundation Anchor
 */

#include "../core/fabric.h"
#include "../core/skrauth.h"
#include "../core/skr8tr_audit.h"
#include "../core/skr8tr_syslog.h"
#include "../parser/skrmaker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SCHED_PORT          7771
#define MESH_PORT           7770   /* heartbeat receive port */
#define NODE_CMD_PORT       7775   /* node command port: LAUNCH/KILL/STATUS */
#define NODE_EXPIRY_S       15
#define MAX_NODES           256
#define MAX_WORKLOADS       256
#define MAX_REPLICAS        64
#define SCALE_UP_CYCLES     2    /* consecutive high-CPU heartbeats before scale-up */
#define SCALE_DOWN_CYCLES   4    /* consecutive low-CPU heartbeats before scale-down */
#define REBALANCE_INTERVAL  5    /* seconds between replica health checks */
#define STATE_FILE          "/tmp/skr8tr_conductor.state"  /* persistent workload state */

/* -------------------------------------------------------------------------
 * PQC auth state — ML-DSA-65 command signing gate
 * ---------------------------------------------------------------------- */

static char g_pubkey_path[512] = {0};
static int  g_auth_enabled     = 0;

/* -------------------------------------------------------------------------
 * Node table — live view of the mesh
 * ---------------------------------------------------------------------- */

typedef struct {
    char     node_id[33];       /* hex node identity */
    char     ip[INET_ADDRSTRLEN];
    int      cpu_pct;
    long     ram_free_mb;
    time_t   last_seen;
    int      active;
    int      high_cpu_cycles;   /* consecutive cycles above cpu_above */
    int      low_cpu_cycles;    /* consecutive cycles below cpu_below */
    /* port collision tracking — prevents two apps binding the same port */
    int      used_ports[64];
    int      used_port_count;
} NodeEntry;

static NodeEntry         g_nodes[MAX_NODES];
static pthread_mutex_t   g_nodes_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Replica placement table
 * ---------------------------------------------------------------------- */

typedef struct {
    char  app_name[128];
    char  node_id[33];
    int   pid;
    int   port;        /* port claimed on the node (0 = none) */
    int   generation;  /* rollout generation; old gens get killed during rollout */
    int   active;
} Placement;

/* -------------------------------------------------------------------------
 * Workload table — desired state
 * ---------------------------------------------------------------------- */

typedef struct {
    char            app_name[128];
    char            manifest_path[512]; /* original .skr8tr path — for state persistence */
    SkrProc         spec;             /* parsed manifest */
    Placement       replicas[MAX_REPLICAS];
    int             replica_count;
    int             active;
    int             current_gen;     /* rollout generation counter */
    int             rolling;         /* 1 while a rollout is in progress */
} Workload;

static Workload          g_workloads[MAX_WORKLOADS];
static pthread_mutex_t   g_workloads_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Persistent state — survive Conductor restarts
 * ---------------------------------------------------------------------- */

static int g_state_loading = 0;  /* suppresses state_save during state_load replay */

/* Forward declarations */
static int        submit_workload(const char* manifest_path, char* resp, size_t resp_len);
static NodeEntry* node_find(const char* node_id);
static NodeEntry* node_least_loaded_for_port(int port);
static void       node_port_release(NodeEntry* n, int port);
static int        launch_replica(Workload* wl, NodeEntry* node);

/* -------------------------------------------------------------------------
 * Rolling update — zero-downtime replacement of all replicas
 *
 * ROLLOUT|<manifest_path>
 *   For each running replica:
 *     1. Launch 1 new replica with new spec (port-aware placement)
 *     2. Wait ROLLOUT_WAIT_S seconds (health settle time)
 *     3. Send KILL to the oldest old-generation replica
 * Runs in a dedicated thread so the main loop stays responsive.
 * ---------------------------------------------------------------------- */

#define ROLLOUT_WAIT_S           8    /* fixed settle window — fallback when no health check */
#define ROLLOUT_READY_TIMEOUT_S  120  /* max seconds to poll readiness probe per replica */
#define ROLLOUT_PROBE_INTERVAL_S 2    /* seconds between readiness probe attempts */

typedef struct {
    char manifest_path[512];
} RolloutArg;

/* -------------------------------------------------------------------------
 * HTTP readiness probe — TCP connect to ip:port, issue an HTTP GET, verify
 * the response status code matches.  Mirrors health_probe() on the node but
 * targets a remote host so the Conductor can drive rollout gating.
 *
 * check_str format: "GET /path <expected_code>"
 * Returns 1 on success, 0 on any failure (connect, timeout, wrong code).
 * ---------------------------------------------------------------------- */

static int remote_health_probe(const char* ip, int port,
                                const char* check_str) {
    char method[16], path[256]; int expected_code = 200;
    if (sscanf(check_str, "%15s %255s %d", method, path, &expected_code) < 2)
        return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) { close(fd); return 0; }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return 0;
    }

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        method, path, ip);
    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); return 0; }

    char resp[64] = {0};
    int n = (int)read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (n <= 0) return 0;

    int actual_code = 0;
    sscanf(resp, "HTTP/%*s %d", &actual_code);
    return actual_code == expected_code;
}

static void* rollout_thread(void* arg) {
    RolloutArg* ra = arg;
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s", ra->manifest_path);
    free(ra);

    char err[512] = {0};
    SkrProc* procs = skrmaker_parse(manifest_path, err, sizeof(err));
    if (!procs) {
        fprintf(stderr, "[sched] rollout parse failed: %s\n", err);
        return NULL;
    }

    for (SkrProc* sp = procs; sp; sp = sp->next) {
        pthread_mutex_lock(&g_workloads_mu);

        /* Find the workload */
        Workload* wl = NULL;
        for (int i = 0; i < MAX_WORKLOADS; i++) {
            if (g_workloads[i].active &&
                !strcmp(g_workloads[i].app_name, sp->name)) {
                wl = &g_workloads[i];
                break;
            }
        }

        if (!wl) {
            pthread_mutex_unlock(&g_workloads_mu);
            fprintf(stderr, "[sched] rollout: app '%s' not found — "
                    "use 'up' first\n", sp->name);
            continue;
        }

        /* Bump generation and update spec */
        int old_gen = wl->current_gen;
        wl->current_gen++;
        wl->spec        = *sp;
        wl->rolling     = 1;
        int old_desired = sp->replicas > 0 ? sp->replicas : 1;
        snprintf(wl->manifest_path, sizeof(wl->manifest_path), "%s",
                 manifest_path);

        printf("[sched] rollout: %s  gen %d → %d  replicas=%d\n",
               sp->name, old_gen, wl->current_gen, old_desired);

        pthread_mutex_unlock(&g_workloads_mu);

        /* Rolling: for each old-gen replica, launch one new then kill the old */
        for (int r = 0; r < old_desired; r++) {
            /* Launch one new-gen replica — capture node IP before unlock */
            pthread_mutex_lock(&g_nodes_mu);
            pthread_mutex_lock(&g_workloads_mu);
            NodeEntry* nd = node_least_loaded_for_port(sp->port);
            char new_node_ip[INET_ADDRSTRLEN] = {0};
            if (nd) {
                snprintf(new_node_ip, sizeof(new_node_ip), "%s", nd->ip);
                launch_replica(wl, nd);
            } else {
                fprintf(stderr, "[sched] rollout: no eligible node for "
                        "replica %d of %s\n", r+1, sp->name);
            }
            pthread_mutex_unlock(&g_workloads_mu);
            pthread_mutex_unlock(&g_nodes_mu);

            /* Wait for the new replica to become ready.
             * If a health check is declared, poll it with a configurable
             * timeout derived from health.interval (e.g. "30s").
             * Fall back to the fixed ROLLOUT_WAIT_S window otherwise. */
            if (sp->health.check[0] && new_node_ip[0] && sp->port > 0) {
                int settle_s = ROLLOUT_READY_TIMEOUT_S;
                int parsed_s = 0;
                sscanf(sp->health.interval, "%d", &parsed_s);
                if (parsed_s > 0) settle_s = parsed_s;

                printf("[sched] rollout: probing %s at %s:%d "
                       "(timeout %ds)...\n",
                       sp->name, new_node_ip, sp->port, settle_s);
                time_t deadline = time(NULL) + settle_s;
                int ready = 0;
                while (time(NULL) < deadline) {
                    if (remote_health_probe(new_node_ip, sp->port,
                                            sp->health.check)) {
                        ready = 1;
                        printf("[sched] rollout: %s is ready\n", sp->name);
                        break;
                    }
                    sleep(ROLLOUT_PROBE_INTERVAL_S);
                }
                if (!ready)
                    fprintf(stderr,
                            "[sched] rollout: %s readiness timeout "
                            "after %ds — proceeding with rollover\n",
                            sp->name, settle_s);
            } else {
                sleep(ROLLOUT_WAIT_S);
            }

            /* Kill the oldest old-gen replica */
            pthread_mutex_lock(&g_nodes_mu);
            pthread_mutex_lock(&g_workloads_mu);
            for (int i = 0; i < MAX_REPLICAS; i++) {
                Placement* pl = &wl->replicas[i];
                if (!pl->active || pl->generation != old_gen) continue;

                NodeEntry* kill_nd = node_find(pl->node_id);
                if (kill_nd) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "KILL|%.127s", sp->name);
                    int ks = fabric_bind(0);
                    if (ks >= 0) {
                        fabric_send(ks, kill_nd->ip, NODE_CMD_PORT,
                                    cmd, strlen(cmd));
                        close(ks);
                    }
                    if (pl->port > 0)
                        node_port_release(kill_nd, pl->port);
                }
                pl->active = 0;
                wl->replica_count--;
                printf("[sched] rollout: killed old gen replica %d of %s\n",
                       r+1, sp->name);
                break;
            }
            pthread_mutex_unlock(&g_workloads_mu);
            pthread_mutex_unlock(&g_nodes_mu);
        }

        pthread_mutex_lock(&g_workloads_mu);
        wl->rolling = 0;
        pthread_mutex_unlock(&g_workloads_mu);

        printf("[sched] rollout: %s complete\n", sp->name);
    }

    skrmaker_free(procs);
    return NULL;
}

static void rollout_workload(const char* manifest_path,
                              char* resp, size_t resp_len) {
    RolloutArg* ra = malloc(sizeof(RolloutArg));
    if (!ra) { snprintf(resp, resp_len, "ERR|out of memory"); return; }
    snprintf(ra->manifest_path, sizeof(ra->manifest_path), "%s", manifest_path);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, rollout_thread, ra) != 0) {
        free(ra);
        snprintf(resp, resp_len, "ERR|rollout thread failed");
        pthread_attr_destroy(&attr);
        return;
    }
    pthread_attr_destroy(&attr);

    /* Extract app name from manifest for the response */
    char parse_err[128] = {0};
    SkrProc* procs = skrmaker_parse(manifest_path, parse_err, sizeof(parse_err));
    if (procs) {
        snprintf(resp, resp_len, "OK|ROLLOUT|%s|rolling", procs->name);
        skrmaker_free(procs);
    } else {
        snprintf(resp, resp_len, "OK|ROLLOUT|unknown|rolling");
    }
}

/* Write all active workload manifest paths to the state file.
 * Must be called OUTSIDE any held mutex — acquires g_workloads_mu internally. */
static void state_save(void) {
    if (g_state_loading) return;
    FILE* f = fopen(STATE_FILE, "w");
    if (!f) {
        fprintf(stderr, "[sched] WARNING: cannot write state file %s: %s\n",
                STATE_FILE, strerror(errno));
        return;
    }
    pthread_mutex_lock(&g_workloads_mu);
    for (int i = 0; i < MAX_WORKLOADS; i++) {
        if (g_workloads[i].active && g_workloads[i].manifest_path[0])
            fprintf(f, "SUBMIT|%s\n", g_workloads[i].manifest_path);
    }
    pthread_mutex_unlock(&g_workloads_mu);
    fclose(f);
}

/* Read state file and re-submit all workloads.
 * Called once at startup — before the main loop. */
static void state_load(char* resp, size_t resp_len) {
    FILE* f = fopen(STATE_FILE, "r");
    if (!f) return;   /* no state file = fresh start */
    g_state_loading = 1;
    char line[520];
    int  restored = 0;
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (!strncmp(line, "SUBMIT|", 7) && line[7]) {
            resp[0] = '\0';
            submit_workload(line + 7, resp, resp_len);
            printf("[sched] state restore: %s\n", resp);
            restored++;
        }
    }
    fclose(f);
    g_state_loading = 0;
    if (restored)
        printf("[sched] restored %d workload(s) from %s\n", restored, STATE_FILE);
}

/* -------------------------------------------------------------------------
 * Node table helpers
 * ---------------------------------------------------------------------- */

/* Find or allocate a node slot by node_id. Returns NULL if table full. */
static NodeEntry* node_upsert(const char* node_id, const char* ip) {
    NodeEntry* free_slot = NULL;
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].active && !strcmp(g_nodes[i].node_id, node_id))
            return &g_nodes[i];
        if (!g_nodes[i].active && !free_slot)
            free_slot = &g_nodes[i];
    }
    if (!free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->node_id, sizeof(free_slot->node_id), "%s", node_id);
    snprintf(free_slot->ip,      sizeof(free_slot->ip),      "%s", ip);
    free_slot->active = 1;
    return free_slot;
}

/* Find a live node by node_id. Must be called with g_nodes_mu held. */
static NodeEntry* node_find(const char* node_id) {
    for (int i = 0; i < MAX_NODES; i++)
        if (g_nodes[i].active && !strcmp(g_nodes[i].node_id, node_id))
            return &g_nodes[i];
    return NULL;
}

/* Port tracking helpers — must be called with g_nodes_mu held. */
static int node_port_in_use(const NodeEntry* n, int port) {
    if (port <= 0) return 0;
    for (int i = 0; i < n->used_port_count; i++)
        if (n->used_ports[i] == port) return 1;
    return 0;
}

static void node_port_claim(NodeEntry* n, int port) {
    if (port <= 0 || node_port_in_use(n, port)) return;
    if (n->used_port_count < 64)
        n->used_ports[n->used_port_count++] = port;
}

static void node_port_release(NodeEntry* n, int port) {
    if (port <= 0) return;
    for (int i = 0; i < n->used_port_count; i++) {
        if (n->used_ports[i] == port) {
            n->used_ports[i] = n->used_ports[--n->used_port_count];
            return;
        }
    }
}

/* Pick the least-loaded live node that does NOT already have `port` bound.
 * port=0 means no port constraint (any node eligible).
 * Returns NULL if no eligible nodes. Must be called with g_nodes_mu held. */
static NodeEntry* node_least_loaded_for_port(int port) {
    time_t    now  = time(NULL);
    NodeEntry* best = NULL;
    for (int i = 0; i < MAX_NODES; i++) {
        NodeEntry* n = &g_nodes[i];
        if (!n->active || (now - n->last_seen) >= NODE_EXPIRY_S) continue;
        if (port > 0 && node_port_in_use(n, port)) continue;
        if (!best ||
            n->cpu_pct < best->cpu_pct ||
            (n->cpu_pct == best->cpu_pct && n->ram_free_mb > best->ram_free_mb))
            best = n;
    }
    return best;
}


/* -------------------------------------------------------------------------
 * Placement helpers
 * ---------------------------------------------------------------------- */

static int placement_count(const Workload* wl) {
    int n = 0;
    for (int i = 0; i < MAX_REPLICAS; i++)
        if (wl->replicas[i].active) n++;
    return n;
}

static Placement* placement_alloc(Workload* wl) {
    for (int i = 0; i < MAX_REPLICAS; i++)
        if (!wl->replicas[i].active) return &wl->replicas[i];
    return NULL;
}

/* Remove all placements for a given node (called when node expires) */
static void placement_evict_node(const char* node_id) {
    for (int w = 0; w < MAX_WORKLOADS; w++) {
        if (!g_workloads[w].active) continue;
        for (int r = 0; r < MAX_REPLICAS; r++) {
            Placement* pl = &g_workloads[w].replicas[r];
            if (pl->active && !strcmp(pl->node_id, node_id)) {
                printf("[sched] replica lost: %s on node %s\n",
                       pl->app_name, node_id);
                pl->active = 0;
                g_workloads[w].replica_count--;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Launch a replica of a workload onto a specific node
 * ---------------------------------------------------------------------- */

static int send_sock = -1;   /* fabric send socket */

static int launch_replica(Workload* wl, NodeEntry* node) {
    const SkrProc* sp = &wl->spec;

    /* Build LAUNCH command: LAUNCH|name=<n>|bin=<b>|port=<p>|env=<K=V,...> */
    char env_str[1024] = {0};
    for (int i = 0; i < sp->env_count; i++) {
        if (i > 0) strncat(env_str, ",", sizeof(env_str) - strlen(env_str) - 1);
        char kv[320];
        snprintf(kv, sizeof(kv), "%s=%s", sp->env[i].key, sp->env[i].val);
        strncat(env_str, kv, sizeof(env_str) - strlen(env_str) - 1);
    }

    /* Encode restart policy and drain timeout */
    const char* restart_str =
        sp->restart_policy == SKRTR_RESTART_ON_FAILURE ? "on-failure" :
        sp->restart_policy == SKRTR_RESTART_NEVER      ? "never"      : "always";
    int drain_s = sp->drain_timeout_s > 0 ? sp->drain_timeout_s : 5;

    char cmd[FABRIC_MTU];
    /* Build base command with required fields */
    int clen = snprintf(cmd, sizeof(cmd),
                        "LAUNCH|name=%s|bin=%s|port=%d",
                        sp->name, sp->bin, sp->port);
    if (sp->args[0] && clen < (int)sizeof(cmd) - 1)
        clen += snprintf(cmd + clen, sizeof(cmd) - (size_t)clen,
                         "|args=%s", sp->args);
    if (env_str[0] && clen < (int)sizeof(cmd) - 1)
        clen += snprintf(cmd + clen, sizeof(cmd) - (size_t)clen,
                         "|env=%s", env_str);
    if (clen < (int)sizeof(cmd) - 1)
        clen += snprintf(cmd + clen, sizeof(cmd) - (size_t)clen,
                         "|restart=%s|drain=%d", restart_str, drain_s);
    (void)clen;

    /* Use a dedicated ephemeral socket for the LAUNCH round-trip so we can
     * read the OK|LAUNCHED|name|pid reply without touching the main cmd_sock. */
    int lsock = fabric_bind(0);
    if (lsock < 0) {
        fprintf(stderr, "[sched] launch_replica: cannot create socket: %s\n",
                strerror(errno));
        return 0;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(lsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (fabric_send(lsock, node->ip, NODE_CMD_PORT, cmd, strlen(cmd)) < 0) {
        fprintf(stderr, "[sched] send LAUNCH to %s failed: %s\n",
                node->ip, strerror(errno));
        close(lsock);
        return 0;
    }

    /* Read the node's LAUNCHED reply to capture the real PID */
    char reply[256] = {0};
    int  rn = fabric_recv(lsock, reply, sizeof(reply) - 1, NULL);
    close(lsock);

    pid_t real_pid = 0;
    if (rn > 0) {
        reply[rn] = '\0';
        /* Format: OK|LAUNCHED|<name>|<pid> */
        const char* pid_field = NULL;
        int pipes = 0;
        for (int i = 0; i < rn; i++) {
            if (reply[i] == '|' && ++pipes == 3) { pid_field = reply + i + 1; break; }
        }
        if (pid_field) real_pid = (pid_t)strtol(pid_field, NULL, 10);
    }

    /* Record the placement with the real PID */
    Placement* pl = placement_alloc(wl);
    if (!pl) { fprintf(stderr, "[sched] placement table full\n"); return 0; }
    snprintf(pl->app_name, sizeof(pl->app_name), "%s", sp->name);
    snprintf(pl->node_id,  sizeof(pl->node_id),  "%s", node->node_id);
    pl->pid        = (int)real_pid;
    pl->port       = sp->port;
    pl->generation = wl->current_gen;
    pl->active     = 1;
    wl->replica_count++;

    /* Claim the port on this node — prevents collision for future placements */
    if (sp->port > 0)
        node_port_claim(node, sp->port);

    printf("[sched] launched replica: %s → node %s (%s) pid=%d\n",
           sp->name, node->node_id, node->ip, (int)real_pid);
    return 1;
}

/* -------------------------------------------------------------------------
 * SUBMIT — parse manifest and schedule initial replicas
 * ---------------------------------------------------------------------- */

static int submit_workload(const char* manifest_path,
                           char* resp, size_t resp_len) {
    char err[512] = {0};
    SkrProc* procs = skrmaker_parse(manifest_path, err, sizeof(err));
    if (!procs) {
        snprintf(resp, resp_len, "ERR|parse failed: %s", err);
        return 0;
    }

    int submitted = 0;
    for (SkrProc* sp = procs; sp; sp = sp->next) {

        /* Find or allocate workload slot */
        pthread_mutex_lock(&g_workloads_mu);
        Workload* wl = NULL;
        for (int i = 0; i < MAX_WORKLOADS; i++) {
            if (g_workloads[i].active &&
                !strcmp(g_workloads[i].app_name, sp->name)) {
                wl = &g_workloads[i];
                break;
            }
        }
        if (!wl) {
            for (int i = 0; i < MAX_WORKLOADS; i++) {
                if (!g_workloads[i].active) {
                    wl = &g_workloads[i];
                    memset(wl, 0, sizeof(*wl));
                    break;
                }
            }
        }
        if (!wl) {
            pthread_mutex_unlock(&g_workloads_mu);
            snprintf(resp, resp_len, "ERR|workload table full");
            skrmaker_free(procs);
            return 0;
        }

        snprintf(wl->app_name,      sizeof(wl->app_name),      "%s", sp->name);
        snprintf(wl->manifest_path, sizeof(wl->manifest_path), "%s", manifest_path);
        wl->spec   = *sp;
        wl->active = 1;
        int desired    = sp->replicas > 0 ? sp->replicas : 1;
        int have       = placement_count(wl);

        pthread_mutex_lock(&g_nodes_mu);
        char first_node_id[33] = {0};

        for (int r = have; r < desired; r++) {
            NodeEntry* node = node_least_loaded_for_port(sp->port);
            if (!node) {
                printf("[sched] WARNING: no eligible nodes for %s"
                       " (port %d may already be in use on all nodes)\n",
                       sp->name, sp->port);
                break;
            }
            if (launch_replica(wl, node)) {
                if (!first_node_id[0])
                    snprintf(first_node_id, sizeof(first_node_id),
                             "%s", node->node_id);
                submitted++;
            }
        }

        pthread_mutex_unlock(&g_nodes_mu);
        pthread_mutex_unlock(&g_workloads_mu);

        if (first_node_id[0])
            snprintf(resp, resp_len, "OK|SUBMITTED|%s|%s",
                     sp->name, first_node_id);
        else
            snprintf(resp, resp_len,
                     "OK|SUBMITTED|%s|pending (no live nodes)",  sp->name);
    }

    skrmaker_free(procs);
    state_save();    /* persist updated workload set */
    return submitted;
}

/* -------------------------------------------------------------------------
 * EVICT — remove all replicas of a workload
 * ---------------------------------------------------------------------- */

static void evict_workload(const char* app_name,
                           char* resp, size_t resp_len) {
    pthread_mutex_lock(&g_workloads_mu);
    pthread_mutex_lock(&g_nodes_mu);

    for (int w = 0; w < MAX_WORKLOADS; w++) {
        Workload* wl = &g_workloads[w];
        if (!wl->active || strcmp(wl->app_name, app_name)) continue;

        for (int r = 0; r < MAX_REPLICAS; r++) {
            Placement* pl = &wl->replicas[r];
            if (!pl->active) continue;

            /* Find the node IP and send KILL */
            for (int n = 0; n < MAX_NODES; n++) {
                if (g_nodes[n].active &&
                    !strcmp(g_nodes[n].node_id, pl->node_id)) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "KILL|%.127s", app_name);
                    fabric_send(send_sock, g_nodes[n].ip, NODE_CMD_PORT,
                                cmd, strlen(cmd));
                    /* Release port claim */
                    if (pl->port > 0)
                        node_port_release(&g_nodes[n], pl->port);
                    break;
                }
            }
            pl->active = 0;
        }
        wl->active        = 0;
        wl->replica_count = 0;
        snprintf(resp, resp_len, "OK|EVICTED|%.127s", app_name);

        pthread_mutex_unlock(&g_nodes_mu);
        pthread_mutex_unlock(&g_workloads_mu);
        state_save();   /* persist updated workload set */
        return;
    }

    pthread_mutex_unlock(&g_nodes_mu);
    pthread_mutex_unlock(&g_workloads_mu);
    snprintf(resp, resp_len, "ERR|not found: %.127s", app_name);
}

/* -------------------------------------------------------------------------
 * Rebalancer thread — runs every REBALANCE_INTERVAL seconds
 *
 * 1. Expire dead nodes and evict their placements
 * 2. Reconcile replica counts (relaunch under-replicated workloads)
 * 3. Scale up / scale down based on cpu thresholds
 * ---------------------------------------------------------------------- */

static void* rebalancer_thread(void* arg) {
    (void)arg;
    while (1) {
        sleep(REBALANCE_INTERVAL);

        pthread_mutex_lock(&g_nodes_mu);
        pthread_mutex_lock(&g_workloads_mu);

        /* Step 1: expire dead nodes, evict their placements */
        time_t now = time(NULL);
        for (int i = 0; i < MAX_NODES; i++) {
            NodeEntry* nd = &g_nodes[i];
            if (!nd->active) continue;
            if ((now - nd->last_seen) >= NODE_EXPIRY_S) {
                printf("[sched] node expired: %s (%s)\n",
                       nd->node_id, nd->ip);
                placement_evict_node(nd->node_id);
                nd->used_port_count = 0;   /* all ports freed with the node */
                nd->active = 0;
            }
        }

        /* Step 2 + 3: reconcile each workload */
        for (int w = 0; w < MAX_WORKLOADS; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active) continue;

            SkrProc* sp    = &wl->spec;
            int desired    = sp->replicas > 0 ? sp->replicas : 1;
            int have       = placement_count(wl);

            /* Scale policy: check per-node CPU trends */
            int scale_min = sp->scale.min > 0 ? sp->scale.min : 1;
            int scale_max = sp->scale.max > 0 ? sp->scale.max : desired;
            int cpu_above = sp->scale.cpu_above;
            int cpu_below = sp->scale.cpu_below;

            if (cpu_above > 0) {
                /* Check if any node hosting this workload is hot */
                for (int r = 0; r < MAX_REPLICAS; r++) {
                    Placement* pl = &wl->replicas[r];
                    if (!pl->active) continue;
                    for (int n = 0; n < MAX_NODES; n++) {
                        NodeEntry* nd = &g_nodes[n];
                        if (!nd->active ||
                            strcmp(nd->node_id, pl->node_id)) continue;

                        if (nd->cpu_pct > cpu_above) {
                            nd->high_cpu_cycles++;
                            nd->low_cpu_cycles = 0;
                        } else if (cpu_below > 0 &&
                                   nd->cpu_pct < cpu_below) {
                            nd->low_cpu_cycles++;
                            nd->high_cpu_cycles = 0;
                        } else {
                            nd->high_cpu_cycles = 0;
                            nd->low_cpu_cycles  = 0;
                        }

                        /* Scale up */
                        if (nd->high_cpu_cycles >= SCALE_UP_CYCLES &&
                            have < scale_max) {
                            NodeEntry* target = node_least_loaded_for_port(sp->port);
                            if (target && target != nd) {
                                printf("[sched] scale-up: %s (cpu %d%% > %d%%)\n",
                                       sp->name, nd->cpu_pct, cpu_above);
                                launch_replica(wl, target);
                                nd->high_cpu_cycles = 0;
                                have++;
                            }
                        }

                        /* Scale down */
                        if (nd->low_cpu_cycles >= SCALE_DOWN_CYCLES &&
                            have > scale_min) {
                            printf("[sched] scale-down: %s (cpu %d%% < %d%%)\n",
                                   sp->name, nd->cpu_pct, cpu_below);
                            char cmd[256];
                            snprintf(cmd, sizeof(cmd), "KILL|%s", sp->name);
                            fabric_send(send_sock, nd->ip, NODE_CMD_PORT,
                                        cmd, strlen(cmd));
                            pl->active = 0;
                            wl->replica_count--;
                            nd->low_cpu_cycles = 0;
                            have--;
                        }
                    }
                }
            }

            /* Reconcile: relaunch any under-replicated workloads */
            have = placement_count(wl);
            while (have < desired) {
                NodeEntry* node = node_least_loaded_for_port(sp->port);
                if (!node) break;
                printf("[sched] reconcile: relaunching replica %d/%d of %s\n",
                       have + 1, desired, sp->name);
                if (!launch_replica(wl, node)) break;
                have++;
            }
        }

        pthread_mutex_unlock(&g_workloads_mu);
        pthread_mutex_unlock(&g_nodes_mu);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command handler — port 7771
 * ---------------------------------------------------------------------- */

static void handle_command(const char* cmd, const FabricAddr* src,
                           char* resp, size_t resp_len) {
    /* ---------------------------------------------------------------
     * PQC auth gate — verify ML-DSA-65 signature on mutating commands.
     * SUBMIT and EVICT require a valid signature when auth is enabled.
     * Read-only commands (NODES, LIST, PING) are always permitted.
     * --------------------------------------------------------------- */
    char bare_cmd[FABRIC_MTU];
    const char *effective_cmd = cmd;

    if (g_auth_enabled) {
        int needs_auth = (!strncmp(cmd, "SUBMIT|",  7) ||
                          !strncmp(cmd, "EVICT|",   6) ||
                          !strncmp(cmd, "ROLLOUT|", 8));
        if (needs_auth) {
            if (skrauth_verify(cmd, g_pubkey_path,
                               bare_cmd, sizeof(bare_cmd)) != 0) {
                fprintf(stderr,
                        "[sched] UNAUTHORIZED command from %s — "
                        "sign with: skr8tr --key ~/.skr8tr/signing.sec\n",
                        src->ip);
                skraudit_log(SKRAUDIT_AUTH_FAIL, "", src->ip,
                             "bad or missing ML-DSA-65 signature");
                snprintf(resp, resp_len,
                         "ERR|UNAUTHORIZED — sign commands with: "
                         "skr8tr --key ~/.skr8tr/signing.sec");
                return;
            }
            effective_cmd = bare_cmd;
        }
    }

    if (!strncmp(effective_cmd, "SUBMIT|", 7)) {
        skraudit_log(SKRAUDIT_SUBMIT, effective_cmd + 7, src->ip,
                     "workload submitted");
        submit_workload(effective_cmd + 7, resp, resp_len);
        return;
    }

    if (!strncmp(effective_cmd, "EVICT|", 6)) {
        skraudit_log(SKRAUDIT_EVICT, effective_cmd + 6, src->ip,
                     "workload evicted");
        evict_workload(effective_cmd + 6, resp, resp_len);
        return;
    }

    if (!strncmp(effective_cmd, "ROLLOUT|", 8)) {
        skraudit_log(SKRAUDIT_ROLLOUT, effective_cmd + 8, src->ip,
                     "rolling update initiated");
        rollout_workload(effective_cmd + 8, resp, resp_len);
        return;
    }

    if (!strcmp(effective_cmd, "NODES")) {
        pthread_mutex_lock(&g_nodes_mu);
        time_t now  = time(NULL);
        int    live = 0;
        char   list[4096] = {0};
        for (int i = 0; i < MAX_NODES; i++) {
            NodeEntry* nd = &g_nodes[i];
            if (!nd->active || (now - nd->last_seen) >= NODE_EXPIRY_S)
                continue;
            live++;
            char entry[96];
            snprintf(entry, sizeof(entry), "%.32s:%.15s:%d:%ld",
                     nd->node_id, nd->ip, nd->cpu_pct, nd->ram_free_mb);
            if (list[0]) strncat(list, ",", sizeof(list) - strlen(list) - 1);
            strncat(list, entry, sizeof(list) - strlen(list) - 1);
        }
        pthread_mutex_unlock(&g_nodes_mu);
        snprintf(resp, resp_len, "OK|NODES|%d|%s", live, list);
        return;
    }

    if (!strcmp(effective_cmd, "LIST")) {
        pthread_mutex_lock(&g_workloads_mu);
        int  total = 0;
        char list[4096] = {0};
        for (int w = 0; w < MAX_WORKLOADS; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active) continue;
            for (int r = 0; r < MAX_REPLICAS; r++) {
                Placement* pl = &wl->replicas[r];
                if (!pl->active) continue;
                total++;
                char entry[160];
                snprintf(entry, sizeof(entry), "%.64s:%.32s:%d",
                         pl->app_name, pl->node_id, pl->pid);
                if (list[0]) strncat(list, ",", sizeof(list) - strlen(list) - 1);
                strncat(list, entry, sizeof(list) - strlen(list) - 1);
            }
        }
        pthread_mutex_unlock(&g_workloads_mu);
        snprintf(resp, resp_len, "OK|LIST|%d|%s", total, list);
        return;
    }

    if (!strcmp(effective_cmd, "PING")) {
        snprintf(resp, resp_len, "OK|PONG|conductor");
        return;
    }

    /* ---------------------------------------------------------------
     * EXEC|<app_name>|<shell_cmd>
     * Forward to the first live node hosting the app and relay output.
     * --------------------------------------------------------------- */
    if (!strncmp(effective_cmd, "EXEC|", 5)) {
        const char* rest = effective_cmd + 5;
        const char* pipe_pos = strchr(rest, '|');
        if (!pipe_pos) {
            snprintf(resp, resp_len, "ERR|EXEC requires app|cmd");
            return;
        }
        char app_name[128] = {0};
        size_t alen = (size_t)(pipe_pos - rest);
        if (alen >= sizeof(app_name)) alen = sizeof(app_name) - 1;
        memcpy(app_name, rest, alen);
        const char* shell_cmd = pipe_pos + 1;

        /* Find a node hosting the app */
        char target_ip[INET_ADDRSTRLEN] = {0};
        pthread_mutex_lock(&g_workloads_mu);
        pthread_mutex_lock(&g_nodes_mu);
        for (int w = 0; w < MAX_WORKLOADS && !target_ip[0]; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active || strcmp(wl->app_name, app_name)) continue;
            for (int r = 0; r < MAX_REPLICAS; r++) {
                Placement* pl = &wl->replicas[r];
                if (!pl->active) continue;
                NodeEntry* nd = node_find(pl->node_id);
                if (nd) {
                    snprintf(target_ip, sizeof(target_ip), "%s", nd->ip);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_nodes_mu);
        pthread_mutex_unlock(&g_workloads_mu);

        if (!target_ip[0]) {
            snprintf(resp, resp_len, "ERR|no live node for app: %.127s",
                     app_name);
            return;
        }

        skraudit_log(SKRAUDIT_EXEC, app_name, src->ip, shell_cmd);

        /* Forward EXEC to node CMD port and relay response */
        int esock = fabric_bind(0);
        if (esock < 0) {
            snprintf(resp, resp_len, "ERR|socket: %s", strerror(errno));
            return;
        }
        struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
        setsockopt(esock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char fwd[FABRIC_MTU];
        snprintf(fwd, sizeof(fwd), "EXEC|%.127s|%s", app_name, shell_cmd);
        fabric_send(esock, target_ip, NODE_CMD_PORT, fwd, strlen(fwd));

        char node_resp[FABRIC_MTU] = {0};
        int rn = fabric_recv(esock, node_resp, sizeof(node_resp) - 1, NULL);
        close(esock);
        if (rn > 0) {
            node_resp[rn] = '\0';
            snprintf(resp, resp_len, "%s", node_resp);
        } else {
            snprintf(resp, resp_len, "ERR|EXEC timeout on node %s", target_ip);
        }
        return;
    }

    /* ---------------------------------------------------------------
     * LOGS|<app_name>
     * Aggregate log output from every node hosting a replica of the app.
     * Queries each node's CMD port sequentially, combines the ring
     * buffers, and returns the result in one response datagram.
     * --------------------------------------------------------------- */
    if (!strncmp(effective_cmd, "LOGS|", 5)) {
        const char* app_name = effective_cmd + 5;

        pthread_mutex_lock(&g_workloads_mu);
        pthread_mutex_lock(&g_nodes_mu);

        /* Collect active placements for this app */
        char  node_ips[MAX_REPLICAS][INET_ADDRSTRLEN];
        int   node_count = 0;

        for (int w = 0; w < MAX_WORKLOADS && node_count < MAX_REPLICAS; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active || strcmp(wl->app_name, app_name)) continue;
            for (int r = 0; r < MAX_REPLICAS && node_count < MAX_REPLICAS; r++) {
                Placement* pl = &wl->replicas[r];
                if (!pl->active) continue;
                NodeEntry* nd = node_find(pl->node_id);
                if (nd) {
                    snprintf(node_ips[node_count], INET_ADDRSTRLEN,
                             "%s", nd->ip);
                    node_count++;
                }
            }
        }

        pthread_mutex_unlock(&g_nodes_mu);
        pthread_mutex_unlock(&g_workloads_mu);

        if (node_count == 0) {
            snprintf(resp, resp_len, "ERR|no running replicas: %.127s",
                     app_name);
            return;
        }

        /* Query each node and aggregate */
        char agg[FABRIC_MTU - 256];
        int  agg_len = 0;
        agg[0] = '\0';

        for (int ni = 0; ni < node_count; ni++) {
            int lsock = fabric_bind(0);
            if (lsock < 0) continue;

            struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
            setsockopt(lsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char query[256];
            snprintf(query, sizeof(query), "LOGS|%.127s", app_name);
            fabric_send(lsock, node_ips[ni], NODE_CMD_PORT,
                        query, strlen(query));

            char node_resp[FABRIC_MTU] = {0};
            int  rn = fabric_recv(lsock, node_resp, sizeof(node_resp) - 1,
                                  NULL);
            close(lsock);
            if (rn <= 0) continue;
            node_resp[rn] = '\0';

            /* Format: OK|LOGS|<name>|<count>|<lines> */
            if (strncmp(node_resp, "OK|LOGS|", 8)) continue;
            char* p = node_resp + 8;
            /* skip name field */
            char* f1 = strchr(p, '|'); if (!f1) continue;
            /* skip count field */
            char* f2 = strchr(f1 + 1, '|'); if (!f2) continue;
            const char* lines = f2 + 1;

            int written = snprintf(agg + agg_len,
                                   sizeof(agg) - (size_t)agg_len,
                                   "=== node %s ===\n%s",
                                   node_ips[ni], lines);
            if (written > 0) agg_len += written;
            if (agg_len >= (int)sizeof(agg) - 1) break;
        }

        snprintf(resp, resp_len, "OK|LOGS|%.127s|%d|%s",
                 app_name, node_count, agg);
        return;
    }

    /* ---------------------------------------------------------------
     * AUDIT|<n>
     * Return the last n entries from the cryptographic audit ledger.
     * HIPAA § 164.312(b) — Audit Controls query interface.
     * HITRUST CSF 09.aa  — Audit Log review capability.
     * --------------------------------------------------------------- */
    if (!strncmp(effective_cmd, "AUDIT|", 6)) {
        int n = atoi(effective_cmd + 6);
        if (n <= 0 || n > 1000) n = 50;
        char tail_buf[FABRIC_MTU - 64];
        skraudit_tail(n, tail_buf, sizeof(tail_buf));
        snprintf(resp, resp_len, "OK|AUDIT|%d|%s", n, tail_buf);
        return;
    }

    /* ---------------------------------------------------------------
     * AUDIT_VERIFY
     * Walk the entire audit chain and verify every SHA-256 link.
     * HITRUST CSF 09.ac — Log integrity verification.
     * NIST 800-53 AU-9  — Protection of audit information.
     * --------------------------------------------------------------- */
    if (!strcmp(effective_cmd, "AUDIT_VERIFY")) {
        char verify_err[512] = {0};
        int rc = skraudit_verify_chain(verify_err, sizeof(verify_err));
        if (rc == 0)
            snprintf(resp, resp_len, "OK|AUDIT_VERIFY|%s", verify_err);
        else
            snprintf(resp, resp_len, "ERR|AUDIT_VERIFY|%s", verify_err);
        return;
    }

    snprintf(resp, resp_len, "ERR|unknown command");
    (void)src;
}

/* -------------------------------------------------------------------------
 * Heartbeat processor — called for every HEARTBEAT datagram on port 7770
 *
 * Format: HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb>
 * ---------------------------------------------------------------------- */

static void process_heartbeat(const char* msg, const FabricAddr* src) {
    /* HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb> */
    if (!strncmp(msg, "HEARTBEAT|", 10)) {
        char node_id[33] = {0};
        int  cpu_pct     = 0;
        long ram_mb      = 0;
        if (sscanf(msg, "HEARTBEAT|%32[^|]|%d|%ld",
                   node_id, &cpu_pct, &ram_mb) != 3)
            return;
        pthread_mutex_lock(&g_nodes_mu);
        NodeEntry* nd = node_upsert(node_id, src->ip);
        if (nd) {
            nd->cpu_pct     = cpu_pct;
            nd->ram_free_mb = ram_mb;
            nd->last_seen   = time(NULL);
        }
        pthread_mutex_unlock(&g_nodes_mu);
        return;
    }

    /* DIED|<node_id>|<app_name>|<pid>|<exit_code>
     * Mark the placement inactive so the rebalancer relaunches it. */
    if (!strncmp(msg, "DIED|", 5)) {
        char node_id[33] = {0}, app_name[128] = {0};
        sscanf(msg, "DIED|%32[^|]|%127[^|]", node_id, app_name);
        printf("[sched] DIED: %s on node %s\n", app_name, node_id);
        skraudit_log(SKRAUDIT_NODE_DIED, app_name, node_id,
                     "process exited — rebalancer will relaunch");

        pthread_mutex_lock(&g_workloads_mu);
        for (int w = 0; w < MAX_WORKLOADS; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active || strcmp(wl->app_name, app_name)) continue;
            for (int r = 0; r < MAX_REPLICAS; r++) {
                Placement* pl = &wl->replicas[r];
                if (!pl->active || strcmp(pl->node_id, node_id)) continue;

                pthread_mutex_lock(&g_nodes_mu);
                NodeEntry* nd = node_find(node_id);
                if (nd && pl->port > 0)
                    node_port_release(nd, pl->port);
                pthread_mutex_unlock(&g_nodes_mu);

                pl->active = 0;
                wl->replica_count--;
                break;
            }
        }
        pthread_mutex_unlock(&g_workloads_mu);
        return;
    }

    /* RELAUNCHED|<node_id>|<app_name>|<new_pid>
     * Update the placement table with the new PID from a local node restart. */
    if (!strncmp(msg, "RELAUNCHED|", 11)) {
        char node_id[33] = {0}, app_name[128] = {0};
        int  new_pid = 0;
        sscanf(msg, "RELAUNCHED|%32[^|]|%127[^|]|%d",
               node_id, app_name, &new_pid);
        printf("[sched] RELAUNCHED: %s on node %s (new pid=%d)\n",
               app_name, node_id, new_pid);

        pthread_mutex_lock(&g_workloads_mu);
        for (int w = 0; w < MAX_WORKLOADS; w++) {
            Workload* wl = &g_workloads[w];
            if (!wl->active || strcmp(wl->app_name, app_name)) continue;
            for (int r = 0; r < MAX_REPLICAS; r++) {
                Placement* pl = &wl->replicas[r];
                if (pl->active && !strcmp(pl->node_id, node_id)) {
                    pl->pid = new_pid;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_workloads_mu);
        return;
    }
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    /* Parse flags:
     *   --pubkey     <path>   ML-DSA-65 public key for command authentication
     *   --audit-log  <path>   override default audit log path
     *   --audit-key  <path>   32-byte key file for AES-256-GCM at-rest encryption
     *   --syslog-host <host>  syslog collector hostname/IP (UDP RFC 5426)
     *   --syslog-port <port>  syslog collector port (default 514 / 6514 for TLS)
     *   --syslog-tls          use TLS/TCP syslog (RFC 5425) instead of UDP
     *   --syslog-ca  <path>   PEM CA cert for TLS peer verification
     */
    const char* audit_path    = NULL;
    const char* audit_key     = NULL;
    const char* syslog_host   = NULL;
    int         syslog_port   = 0;
    int         syslog_tls    = 0;
    const char* syslog_ca     = NULL;

    for (int i = 1; i < argc; i++) {
        if (i < argc - 1) {
            if (!strcmp(argv[i], "--pubkey"))
                snprintf(g_pubkey_path, sizeof(g_pubkey_path), "%s", argv[i + 1]);
            if (!strcmp(argv[i], "--audit-log"))  audit_path  = argv[i + 1];
            if (!strcmp(argv[i], "--audit-key"))  audit_key   = argv[i + 1];
            if (!strcmp(argv[i], "--syslog-host")) syslog_host = argv[i + 1];
            if (!strcmp(argv[i], "--syslog-port")) syslog_port = atoi(argv[i + 1]);
            if (!strcmp(argv[i], "--syslog-ca"))  syslog_ca   = argv[i + 1];
        }
        if (!strcmp(argv[i], "--syslog-tls")) syslog_tls = 1;
    }

    /* Enable at-rest encryption BEFORE init so the genesis entry is encrypted */
    if (audit_key)
        skraudit_set_encryption(audit_key);

    /* Initialise cryptographic audit ledger */
    skraudit_init(audit_path);   /* NULL → /var/log/skr8tr_audit.log */

    /* Initialise syslog forwarder — RFC 5426 (UDP) or RFC 5425 (TLS/TCP) */
    if (syslog_host) {
        if (skrsyslog_init(syslog_host, syslog_port, syslog_tls, syslog_ca) == 0)
            skraudit_set_syslog(1);
        else
            fprintf(stderr, "[sched] WARNING: syslog init failed — "
                    "events will not be forwarded to SIEM\n");
    }

    printf("[sched] Skr8tr Conductor starting...\n");

    /* Resolve pubkey — explicit flag, else default filename in cwd */
    if (!g_pubkey_path[0])
        snprintf(g_pubkey_path, sizeof(g_pubkey_path), "%s",
                 SKRAUTH_PUBKEY_FILENAME);

    struct stat _pk_st;
    if (stat(g_pubkey_path, &_pk_st) == 0) {
        g_auth_enabled = 1;
        printf("[sched] PQC auth ENABLED  pubkey: %s\n", g_pubkey_path);
        printf("[sched] SUBMIT and EVICT require ML-DSA-65 signed commands.\n");
        printf("[sched] Operator usage:  skr8tr --key ~/.skr8tr/signing.sec up app.skr8tr\n");
    } else {
        g_auth_enabled = 0;
        printf("[sched] WARNING: no pubkey found at '%s' — "
               "running UNAUTHENTICATED (dev mode).\n", g_pubkey_path);
        printf("[sched] Run 'skrtrkey keygen' and place skrtrview.pub "
               "here to enable auth.\n");
    }

    /* Bind command socket on 7771 */
    int cmd_sock = fabric_bind(SCHED_PORT);
    if (cmd_sock < 0) {
        fprintf(stderr, "[sched] FATAL: cannot bind port %d: %s\n",
                SCHED_PORT, strerror(errno));
        return 1;
    }

    /* Bind mesh socket on 7770 to receive heartbeats */
    int mesh_sock = fabric_bind(MESH_PORT);
    if (mesh_sock < 0) {
        fprintf(stderr, "[sched] FATAL: cannot bind port %d: %s\n",
                MESH_PORT, strerror(errno));
        return 1;
    }

    /* Shared send socket */
    send_sock = cmd_sock;

    printf("[sched] listening: cmd=UDP:%d  mesh=UDP:%d\n",
           SCHED_PORT, MESH_PORT);

    /* Start rebalancer thread */
    pthread_t rebal_tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&rebal_tid, &attr, rebalancer_thread, NULL) != 0) {
        fprintf(stderr, "[sched] FATAL: cannot start rebalancer thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);

    /* Restore workloads from previous run */
    {
        char restore_resp[FABRIC_MTU];
        state_load(restore_resp, sizeof(restore_resp));
    }

    printf("[sched] Conductor ready — watching for nodes and workloads\n");

    /* Main loop — multiplex cmd_sock and mesh_sock with select() */
    char     buf[FABRIC_MTU];
    char     resp[FABRIC_MTU];
    FabricAddr src;

    int max_fd = cmd_sock > mesh_sock ? cmd_sock : mesh_sock;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cmd_sock,  &rfds);
        FD_SET(mesh_sock, &rfds);

        int ready = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[sched] select error: %s\n", strerror(errno));
            continue;
        }

        /* Mesh socket — heartbeats + DIED + RELAUNCHED from nodes */
        if (FD_ISSET(mesh_sock, &rfds)) {
            int n = fabric_recv(mesh_sock, buf, sizeof(buf) - 1, &src);
            if (n > 0) {
                buf[n] = '\0';
                process_heartbeat(buf, &src);
            }
        }

        /* Operator command on cmd socket */
        if (FD_ISSET(cmd_sock, &rfds)) {
            int n = fabric_recv(cmd_sock, buf, sizeof(buf) - 1, &src);
            if (n > 0) {
                buf[n] = '\0';
                resp[0] = '\0';
                handle_command(buf, &src, resp, sizeof(resp));
                if (resp[0])
                    fabric_send(cmd_sock, src.ip, src.port,
                                resp, strlen(resp));
            }
        }
    }

    return 0;
}
