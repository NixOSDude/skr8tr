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
#include "../parser/skrmaker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SCHED_PORT          7771
#define MESH_PORT           7770
#define NODE_EXPIRY_S       15
#define MAX_NODES           256
#define MAX_WORKLOADS       256
#define MAX_REPLICAS        64
#define SCALE_UP_CYCLES     2    /* consecutive high-CPU heartbeats before scale-up */
#define SCALE_DOWN_CYCLES   4    /* consecutive low-CPU heartbeats before scale-down */
#define REBALANCE_INTERVAL  5    /* seconds between replica health checks */

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
    int   active;
} Placement;

/* -------------------------------------------------------------------------
 * Workload table — desired state
 * ---------------------------------------------------------------------- */

typedef struct {
    char            app_name[128];
    SkrProc         spec;             /* parsed manifest */
    Placement       replicas[MAX_REPLICAS];
    int             replica_count;
    int             active;
} Workload;

static Workload          g_workloads[MAX_WORKLOADS];
static pthread_mutex_t   g_workloads_mu = PTHREAD_MUTEX_INITIALIZER;

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

/* Pick the least-loaded live node (lowest cpu_pct, breaking ties by ram_free_mb).
 * Returns NULL if no live nodes. */
static NodeEntry* node_least_loaded(void) {
    time_t    now  = time(NULL);
    NodeEntry* best = NULL;
    for (int i = 0; i < MAX_NODES; i++) {
        NodeEntry* n = &g_nodes[i];
        if (!n->active || (now - n->last_seen) >= NODE_EXPIRY_S) continue;
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

    char cmd[FABRIC_MTU];
    if (env_str[0])
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d|env=%s",
                 sp->name, sp->bin, sp->port, env_str);
    else
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d",
                 sp->name, sp->bin, sp->port);

    if (fabric_send(send_sock, node->ip, MESH_PORT, cmd, strlen(cmd)) < 0) {
        fprintf(stderr, "[sched] send LAUNCH to %s failed: %s\n",
                node->ip, strerror(errno));
        return 0;
    }

    /* Optimistically record the placement — node will confirm via STATUS */
    Placement* pl = placement_alloc(wl);
    if (!pl) { fprintf(stderr, "[sched] placement table full\n"); return 0; }
    snprintf(pl->app_name, sizeof(pl->app_name), "%s", sp->name);
    snprintf(pl->node_id,  sizeof(pl->node_id),  "%s", node->node_id);
    pl->pid    = 0;     /* pid known after first STATUS response */
    pl->active = 1;
    wl->replica_count++;

    printf("[sched] launched replica: %s → node %s (%s)\n",
           sp->name, node->node_id, node->ip);
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

        snprintf(wl->app_name, sizeof(wl->app_name), "%s", sp->name);
        wl->spec   = *sp;
        wl->active = 1;
        int desired    = sp->replicas > 0 ? sp->replicas : 1;
        int have       = placement_count(wl);

        pthread_mutex_lock(&g_nodes_mu);
        char first_node_id[33] = {0};

        for (int r = have; r < desired; r++) {
            NodeEntry* node = node_least_loaded();
            if (!node) {
                printf("[sched] WARNING: no live nodes available for %s\n",
                       sp->name);
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
                    fabric_send(send_sock, g_nodes[n].ip, MESH_PORT,
                                cmd, strlen(cmd));
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
                            NodeEntry* target = node_least_loaded();
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
                            fabric_send(send_sock, nd->ip, MESH_PORT,
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
                NodeEntry* node = node_least_loaded();
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
    if (!strncmp(cmd, "SUBMIT|", 7)) {
        const char* path = cmd + 7;
        submit_workload(path, resp, resp_len);
        return;
    }

    if (!strncmp(cmd, "EVICT|", 6)) {
        evict_workload(cmd + 6, resp, resp_len);
        return;
    }

    if (!strcmp(cmd, "NODES")) {
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

    if (!strcmp(cmd, "LIST")) {
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

    if (!strcmp(cmd, "PING")) {
        snprintf(resp, resp_len, "OK|PONG|conductor");
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
    /* Parse fields */
    char node_id[33]  = {0};
    int  cpu_pct      = 0;
    long ram_free_mb  = 0;

    if (sscanf(msg, "HEARTBEAT|%32[^|]|%d|%ld",
               node_id, &cpu_pct, &ram_free_mb) != 3)
        return;

    pthread_mutex_lock(&g_nodes_mu);
    NodeEntry* nd = node_upsert(node_id, src->ip);
    if (nd) {
        nd->cpu_pct     = cpu_pct;
        nd->ram_free_mb = ram_free_mb;
        nd->last_seen   = time(NULL);
    }
    pthread_mutex_unlock(&g_nodes_mu);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    printf("[sched] Skr8tr Conductor starting...\n");

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

        /* Heartbeat on mesh socket */
        if (FD_ISSET(mesh_sock, &rfds)) {
            int n = fabric_recv(mesh_sock, buf, sizeof(buf) - 1, &src);
            if (n > 0) {
                buf[n] = '\0';
                if (!strncmp(buf, "HEARTBEAT|", 10))
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
