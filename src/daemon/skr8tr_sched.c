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
#include "../parser/skrmaker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
    char            manifest_path[512]; /* original .skr8tr path — for state persistence */
    SkrProc         spec;             /* parsed manifest */
    Placement       replicas[MAX_REPLICAS];
    int             replica_count;
    int             active;
} Workload;

static Workload          g_workloads[MAX_WORKLOADS];
static pthread_mutex_t   g_workloads_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Persistent state — survive Conductor restarts
 * ---------------------------------------------------------------------- */

static int g_state_loading = 0;  /* suppresses state_save during state_load replay */

/* Forward declaration — defined later in this file */
static int submit_workload(const char* manifest_path, char* resp, size_t resp_len);

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
    if (env_str[0] && sp->args[0])
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d|args=%s|env=%s",
                 sp->name, sp->bin, sp->port, sp->args, env_str);
    else if (sp->args[0])
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d|args=%s",
                 sp->name, sp->bin, sp->port, sp->args);
    else if (env_str[0])
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d|env=%s",
                 sp->name, sp->bin, sp->port, env_str);
    else
        snprintf(cmd, sizeof(cmd), "LAUNCH|name=%s|bin=%s|port=%d",
                 sp->name, sp->bin, sp->port);

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
    pl->pid    = (int)real_pid;
    pl->active = 1;
    wl->replica_count++;

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
    /* ---------------------------------------------------------------
     * PQC auth gate — verify ML-DSA-65 signature on mutating commands.
     * SUBMIT and EVICT require a valid signature when auth is enabled.
     * Read-only commands (NODES, LIST, PING) are always permitted.
     * --------------------------------------------------------------- */
    char bare_cmd[FABRIC_MTU];
    const char *effective_cmd = cmd;

    if (g_auth_enabled) {
        int needs_auth = (!strncmp(cmd, "SUBMIT|", 7) ||
                          !strncmp(cmd, "EVICT|",  6));
        if (needs_auth) {
            if (skrauth_verify(cmd, g_pubkey_path,
                               bare_cmd, sizeof(bare_cmd)) != 0) {
                fprintf(stderr,
                        "[sched] UNAUTHORIZED command from %s — "
                        "sign with: skr8tr --key ~/.skr8tr/signing.sec\n",
                        src->ip);
                snprintf(resp, resp_len,
                         "ERR|UNAUTHORIZED — sign commands with: "
                         "skr8tr --key ~/.skr8tr/signing.sec");
                return;
            }
            effective_cmd = bare_cmd;
        }
    }

    if (!strncmp(effective_cmd, "SUBMIT|", 7)) {
        submit_workload(effective_cmd + 7, resp, resp_len);
        return;
    }

    if (!strncmp(effective_cmd, "EVICT|", 6)) {
        evict_workload(effective_cmd + 6, resp, resp_len);
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
    /* Parse --pubkey <path> */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--pubkey"))
            snprintf(g_pubkey_path, sizeof(g_pubkey_path), "%s", argv[i + 1]);
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
