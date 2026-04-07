/*
 * skr8tr_node.c — Skr8tr Fleet Node Daemon
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * Responsibilities:
 *   - ML-DSA-65 ephemeral PQC identity at boot
 *   - UDP listener port 7770: LAUNCH | KILL | STATUS | PING | LOGS | EXEC
 *   - Heartbeat broadcast every 5s: node_id, cpu%, ram_free_mb
 *   - Process launch: bare binary, static serve, WASM, or VM (QEMU/Firecracker)
 *   - Log capture: per-process stdout/stderr ring buffer (last 200 lines)
 *   - Health check enforcement: HTTP GET probe → kill+relaunch on failure
 *   - Tower auto-registration: REGISTER on launch, DEREGISTER on kill
 *   - Restart policy: always / on-failure / never — per manifest declaration
 *   - Graceful drain: configurable SIGTERM → SIGKILL window (drain Ns)
 *   - Persistent volumes: host directories mkdir'd and injected as env vars
 *   - Prometheus metrics: HTTP endpoint on port 9100 (/metrics)
 *   - DIED broadcast: sent to Conductor when process dies unexpectedly
 *   - EXEC|app|cmd: fork+exec a shell command, return stdout to caller
 *   - --run <manifest.skr8tr>: parse and launch on startup
 *   - --tower <host>: Tower address for auto-registration (default: 127.0.0.1)
 *   - --conductor <host>: Conductor address for DIED broadcasts (default: 127.0.0.1)
 *
 * Wire protocol (UDP 7770):
 *   LAUNCH|name=<n>|bin=<b>|port=<p>|env=<K=V,...>
 *   KILL|<app_name>
 *   STATUS
 *   LOGS|<app_name>
 *   PING
 *   EXEC|<app_name>|<shell_command>
 *
 * Prometheus metrics (TCP 9100):
 *   GET /metrics  →  Prometheus text format
 */

#include "../core/fabric.h"
#include "../parser/skrmaker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <oqs/oqs.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SKRTR_PORT            7770   /* mesh broadcast / heartbeat send port */
#define SKRTR_CMD_PORT        7775   /* node command port: LAUNCH/KILL/STATUS/PING/LOGS/EXEC */
#define TOWER_PORT            7772
#define CONDUCTOR_PORT        7771
#define METRICS_PORT          9100   /* Prometheus scrape endpoint */
#define HEARTBEAT_INTERVAL_S  5
#define MAX_MANAGED_PROCS     256
#define NODE_ID_HEX_LEN       33
#define MAX_ENV_INJECT        64
#define LOG_RING_LINES        200
#define LOG_LINE_LEN          256
#define HEALTH_CHECK_INTERVAL 10     /* seconds between health probes */
#define DRAIN_DEFAULT_S       2      /* default SIGTERM → SIGKILL window */

/* -------------------------------------------------------------------------
 * Log ring buffer — per process
 * ---------------------------------------------------------------------- */

typedef struct {
    char lines[LOG_RING_LINES][LOG_LINE_LEN];
    int  head;    /* next write index */
    int  count;   /* total lines stored (capped at LOG_RING_LINES) */
} LogRing;

/* -------------------------------------------------------------------------
 * Process table
 * ---------------------------------------------------------------------- */

typedef struct {
    char               name[128];
    pid_t              pid;
    int                active;
    int                port;
    int                log_pipe_r;      /* read end of stdout/stderr pipe */
    pthread_t          log_tid;
    LogRing            logs;

    /* health tracking */
    char               health_check[512];   /* "GET /path 200" */
    char               health_interval[32];
    int                health_retries_max;
    int                health_failures;
    time_t             health_last_check;

    /* lifecycle */
    SkrtrRestartPolicy restart_policy;
    int                drain_s;            /* SIGTERM grace window in seconds */
    int                killed_intentionally; /* set to 1 by KILL handler */
    int                last_exit_status;     /* from waitpid() for on-failure check */

    /* copy of original launch params for restart policy relaunch */
    SkrProc            relaunch;
} ManagedProc;

static ManagedProc     g_procs[MAX_MANAGED_PROCS];
static pthread_mutex_t g_procs_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Node identity
 * ---------------------------------------------------------------------- */

static char g_node_id[NODE_ID_HEX_LEN];

static int node_identity_init(void) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!sig) {
        uint8_t rand_id[16];
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, rand_id, 16) != 16) {
            snprintf(g_node_id, sizeof(g_node_id), "deadbeefdeadbeef00000000deadbeef");
            if (fd >= 0) close(fd);
            return 0;
        }
        close(fd);
        for (int i = 0; i < 16; i++)
            snprintf(g_node_id + i * 2, 3, "%02x", rand_id[i]);
        return 1;
    }
    uint8_t* pub  = malloc(sig->length_public_key);
    uint8_t* priv = malloc(sig->length_secret_key);
    if (!pub || !priv) { free(pub); free(priv); OQS_SIG_free(sig); return 0; }
    OQS_SIG_keypair(sig, pub, priv);
    for (int i = 0; i < 16; i++)
        snprintf(g_node_id + i * 2, 3, "%02x", pub[i]);
    g_node_id[32] = '\0';
    memset(priv, 0, sig->length_secret_key);
    free(pub); free(priv); OQS_SIG_free(sig);
    printf("[node] PQC identity: ML-DSA-65 node_id=%s\n", g_node_id);
    return 1;
}

/* -------------------------------------------------------------------------
 * Tower registration
 * ---------------------------------------------------------------------- */

static char g_tower_host[64]     = "127.0.0.1";
static char g_conductor_host[64] = "127.0.0.1";
static int  g_sock = -1;

static void tower_register(const char* name, int port) {
    if (port <= 0) return;
    char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        struct sockaddr_in dst = {
            .sin_family = AF_INET,
            .sin_port   = htons(1),
        };
        inet_pton(AF_INET, g_tower_host, &dst.sin_addr);
        if (connect(probe, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(probe, (struct sockaddr*)&local, &len) == 0)
                inet_ntop(AF_INET, &local.sin_addr, my_ip, sizeof(my_ip));
        }
        close(probe);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "REGISTER|%.64s|%s|%d", name, my_ip, port);
    fabric_send(g_sock, g_tower_host, TOWER_PORT, cmd, strlen(cmd));
    printf("[node] tower: registered %s at %s:%d\n", name, my_ip, port);
}

static void tower_deregister(const char* name, int port) {
    if (port <= 0) return;
    char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        struct sockaddr_in dst = {
            .sin_family = AF_INET,
            .sin_port   = htons(1),
        };
        inet_pton(AF_INET, g_tower_host, &dst.sin_addr);
        if (connect(probe, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(probe, (struct sockaddr*)&local, &len) == 0)
                inet_ntop(AF_INET, &local.sin_addr, my_ip, sizeof(my_ip));
        }
        close(probe);
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "DEREGISTER|%.64s|%s|%d", name, my_ip, port);
    fabric_send(g_sock, g_tower_host, TOWER_PORT, cmd, strlen(cmd));
}

/* -------------------------------------------------------------------------
 * DIED broadcast — notify Conductor of unexpected process death
 * ---------------------------------------------------------------------- */

static void died_broadcast(const char* name, int exit_status) {
    if (g_sock < 0) return;
    char msg[256];
    snprintf(msg, sizeof(msg), "DIED|%.127s|%s|%d", name, g_node_id, exit_status);
    fabric_send(g_sock, g_conductor_host, CONDUCTOR_PORT, msg, strlen(msg));
    printf("[node] DIED broadcast: %s exit=%d\n", name, exit_status);
}

/* -------------------------------------------------------------------------
 * System metrics
 * ---------------------------------------------------------------------- */

static int cpu_percent(void) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long u1, n1, s1, i1, r1;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu", &u1, &n1, &s1, &i1, &r1) != 5)
        { fclose(f); return 0; }
    fclose(f);
    struct timespec ts = { .tv_sec=0, .tv_nsec=100000000L };
    nanosleep(&ts, NULL);
    f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long u2, n2, s2, i2, r2;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu", &u2, &n2, &s2, &i2, &r2) != 5)
        { fclose(f); return 0; }
    fclose(f);
    unsigned long long active = (u2+n2+s2)-(u1+n1+s1);
    unsigned long long total  = active + (i2-i1);
    return total ? (int)((active*100)/total) : 0;
}

static long ram_free_mb(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128]; long kb = 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb / 1024;
}

/* -------------------------------------------------------------------------
 * Prometheus metrics endpoint — TCP port 9100
 *
 * Serves Prometheus text format 0.0.4 on any HTTP GET request.
 * Scrape with: curl http://<node>:9100/metrics
 *              or any standard Prometheus scrape config.
 *
 * Metrics exposed:
 *   skr8tr_node_info{node_id="..."} 1
 *   skr8tr_node_cpu_percent
 *   skr8tr_node_ram_free_mb
 *   skr8tr_node_processes_active
 *   skr8tr_process_health_failures{app="..."}
 *   skr8tr_process_restart_count{app="..."}
 * ---------------------------------------------------------------------- */

/* Per-process restart counter — incremented on each policy-driven relaunch */
static int g_restart_count[MAX_MANAGED_PROCS];

static void* metrics_thread(void* arg) {
    (void)arg;

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        fprintf(stderr, "[node] metrics: socket failed: %s\n", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(METRICS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[node] metrics: bind %d failed: %s\n",
                METRICS_PORT, strerror(errno));
        close(server);
        return NULL;
    }
    listen(server, 8);
    printf("[node] Prometheus metrics: http://0.0.0.0:%d/metrics\n", METRICS_PORT);

    char req_buf[512];
    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;

        /* Drain the HTTP request — we don't inspect it, respond to any GET */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t _rn = read(client, req_buf, sizeof(req_buf) - 1);
        (void)_rn;

        /* Collect metrics under lock */
        int  active_count   = 0;
        char proc_metrics[4096];
        proc_metrics[0] = '\0';

        pthread_mutex_lock(&g_procs_mu);
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            ManagedProc* mp = &g_procs[i];
            if (!mp->active) continue;
            active_count++;

            /* health failures per app */
            char entry[256];
            snprintf(entry, sizeof(entry),
                "skr8tr_process_health_failures{app=\"%.127s\"} %d\n",
                mp->name, mp->health_failures);
            strncat(proc_metrics, entry,
                    sizeof(proc_metrics) - strlen(proc_metrics) - 1);

            /* restart count per app */
            snprintf(entry, sizeof(entry),
                "skr8tr_process_restart_count{app=\"%.127s\"} %d\n",
                mp->name, g_restart_count[i]);
            strncat(proc_metrics, entry,
                    sizeof(proc_metrics) - strlen(proc_metrics) - 1);
        }
        pthread_mutex_unlock(&g_procs_mu);

        int cpu  = cpu_percent();
        long ram = ram_free_mb();

        /* Build Prometheus text body */
        char body[8192];
        int blen = snprintf(body, sizeof(body),
            "# HELP skr8tr_node_info Node identity\n"
            "# TYPE skr8tr_node_info gauge\n"
            "skr8tr_node_info{node_id=\"%s\"} 1\n"
            "# HELP skr8tr_node_cpu_percent CPU utilization percent (0-100)\n"
            "# TYPE skr8tr_node_cpu_percent gauge\n"
            "skr8tr_node_cpu_percent %d\n"
            "# HELP skr8tr_node_ram_free_mb Free RAM in megabytes\n"
            "# TYPE skr8tr_node_ram_free_mb gauge\n"
            "skr8tr_node_ram_free_mb %ld\n"
            "# HELP skr8tr_node_processes_active Number of active managed processes\n"
            "# TYPE skr8tr_node_processes_active gauge\n"
            "skr8tr_node_processes_active %d\n"
            "# HELP skr8tr_process_health_failures Health probe failure count per app\n"
            "# TYPE skr8tr_process_health_failures gauge\n"
            "# HELP skr8tr_process_restart_count Policy-driven restart count per app\n"
            "# TYPE skr8tr_process_restart_count counter\n"
            "%s",
            g_node_id, cpu, ram, active_count, proc_metrics);

        char http_resp[8192 + 256];
        int rlen = snprintf(http_resp, sizeof(http_resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            blen, body);

        ssize_t _wn = write(client, http_resp, (size_t)rlen);
        (void)_wn;
        close(client);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Health check — HTTP GET probe
 * ---------------------------------------------------------------------- */

/* Returns 1 if health check passes, 0 on failure.
 * check_str format: "GET /path <expected_code>" */
static int health_probe(const char* check_str, int port) {
    char method[16], path[256]; int expected_code = 200;
    if (sscanf(check_str, "%15s %255s %d", method, path, &expected_code) < 2)
        return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return 0;
    }

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        method, path);
    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); return 0; }

    char resp[64] = {0};
    int  n = (int)read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (n <= 0) return 0;

    int actual_code = 0;
    sscanf(resp, "HTTP/%*s %d", &actual_code);
    return actual_code == expected_code;
}

/* -------------------------------------------------------------------------
 * Process table helpers
 * ---------------------------------------------------------------------- */

static ManagedProc* proc_find(const char* name) {
    for (int i = 0; i < MAX_MANAGED_PROCS; i++)
        if (g_procs[i].active && !strcmp(g_procs[i].name, name))
            return &g_procs[i];
    return NULL;
}

static ManagedProc* proc_alloc(void) {
    for (int i = 0; i < MAX_MANAGED_PROCS; i++)
        if (!g_procs[i].active) return &g_procs[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * Volume provisioning — mkdir host_path, inject env var post-fork
 * ---------------------------------------------------------------------- */

static void volume_provision(const char* path) {
    /* Recursive mkdir: create each component of the path */
    char tmp[SKRMAKER_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* -------------------------------------------------------------------------
 * Restart accounting
 * ---------------------------------------------------------------------- */

static void restart_count_inc(const ManagedProc* mp) {
    int idx = (int)(mp - g_procs);
    if (idx >= 0 && idx < MAX_MANAGED_PROCS) g_restart_count[idx]++;
}

/* -------------------------------------------------------------------------
 * proc_reap — collect zombie children, handle restart policy, DIED broadcast
 * ---------------------------------------------------------------------- */

/* Collects one batch of dead children.  For each:
 *   - if killed_intentionally: just mark inactive.
 *   - if restart_policy demands relaunch: copy relaunch params, mark inactive,
 *     relaunch outside the lock (avoids re-entrant launch_proc deadlock).
 *   - if no restart: broadcast DIED to Conductor.
 *
 * Returns list of SkrProc copies that need relaunching (max 8 in one pass).
 * Caller is responsible for relaunching them after releasing the mutex. */

typedef struct { SkrProc proc; int idx; } RelaunchItem;

static void proc_reap(void) {
    int status;
    pid_t pid;

    /* collect all dead children */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

        /* Find the managed slot */
        pthread_mutex_lock(&g_procs_mu);
        int found = 0;
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            ManagedProc* mp = &g_procs[i];
            if (!mp->active || mp->pid != pid) continue;
            found = 1;

            char name[128];
            snprintf(name, sizeof(name), "%s", mp->name);
            int port        = mp->port;
            int intentional = mp->killed_intentionally;

            SkrtrRestartPolicy policy = mp->restart_policy;
            SkrProc relaunch_copy     = mp->relaunch;

            /* Mark slot free */
            tower_deregister(mp->name, mp->port);
            if (mp->log_pipe_r >= 0) { close(mp->log_pipe_r); mp->log_pipe_r = -1; }
            mp->active = 0;
            mp->pid    = 0;
            mp->killed_intentionally = 0;
            pthread_mutex_unlock(&g_procs_mu);

            printf("[node] process exited: %s (pid %d, exit=%d)\n",
                   name, pid, exit_code);

            if (intentional) break;

            /* Determine restart action */
            int do_restart = 0;
            if      (policy == SKRTR_RESTART_ALWAYS)     do_restart = 1;
            else if (policy == SKRTR_RESTART_ON_FAILURE)  do_restart = (exit_code != 0);

            if (do_restart) {
                printf("[node] restarting: %s (policy=%s)\n",
                       name,
                       policy == SKRTR_RESTART_ALWAYS ? "always" : "on-failure");

                /* Re-use the relaunch SkrProc copy */
                /* launch_proc will re-populate a new slot */
                /* Small sleep to avoid tight restart loops */
                struct timespec wait = { .tv_sec=1, .tv_nsec=0 };
                nanosleep(&wait, NULL);

                /* Find our slot index for restart count */
                pthread_mutex_lock(&g_procs_mu);
                /* locate the slot by name to get index — slot may be same or different */
                for (int j = 0; j < MAX_MANAGED_PROCS; j++) {
                    if (!g_procs[j].active && !g_procs[j].name[0]) {
                        restart_count_inc(&g_procs[j]);
                        break;
                    }
                }
                pthread_mutex_unlock(&g_procs_mu);

                /* Launch outside lock — launch_proc takes the mutex internally */
                char lerr[256] = {0};
                extern pid_t launch_proc(const SkrProc*, char*, size_t);
                if (launch_proc(&relaunch_copy, lerr, sizeof(lerr)) < 0)
                    fprintf(stderr, "[node] restart failed: %s: %s\n", name, lerr);
            } else {
                /* Unexpected death — notify Conductor */
                (void)port;
                died_broadcast(name, exit_code);
            }

            pthread_mutex_lock(&g_procs_mu);
            break;
        }
        if (!found) pthread_mutex_unlock(&g_procs_mu);
    }
}

/* -------------------------------------------------------------------------
 * Log reader thread — captures stdout/stderr of a managed process
 * ---------------------------------------------------------------------- */

typedef struct { int pipe_fd; int proc_idx; } LogThreadArg;

static void log_ring_push(LogRing* ring, const char* line) {
    snprintf(ring->lines[ring->head], LOG_LINE_LEN, "%s", line);
    ring->head = (ring->head + 1) % LOG_RING_LINES;
    if (ring->count < LOG_RING_LINES) ring->count++;
}

static void* log_reader_thread(void* arg) {
    LogThreadArg* la = arg;
    int    fd  = la->pipe_fd;
    int    idx = la->proc_idx;
    free(la);

    char   line[LOG_LINE_LEN];
    int    pos = 0;
    char   c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || pos >= LOG_LINE_LEN - 1) {
            line[pos] = '\0';
            if (pos > 0) {
                pthread_mutex_lock(&g_procs_mu);
                if (g_procs[idx].active)
                    log_ring_push(&g_procs[idx].logs, line);
                pthread_mutex_unlock(&g_procs_mu);
            }
            pos = 0;
        } else {
            line[pos++] = c;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Free env array
 * ---------------------------------------------------------------------- */

static void free_env_array(char** env, int count) {
    if (!env) return;
    for (int i = 0; i < count; i++) free(env[i]);
    free(env);
}

/* -------------------------------------------------------------------------
 * cgroups v2 resource limits
 *
 * Creates /sys/fs/cgroup/skr8tr/<name>/ and:
 *   - writes pid to cgroup.procs
 *   - sets memory.max if ram_bytes > 0
 *   - sets cpu.max if cpu_cores > 0 (quota = cpu_cores × 100000 / 100000)
 *
 * Fails gracefully — cgroups v2 unavailable (non-Linux, containers without
 * the cgroup ns mounted) is not a fatal condition.
 * ---------------------------------------------------------------------- */

static void cgroup_apply(const char* name, pid_t pid,
                          int64_t ram_bytes, int cpu_cores) {
    if (mkdir("/sys/fs/cgroup/skr8tr", 0755) < 0 && errno != EEXIST) return;

    char dir[320];
    snprintf(dir, sizeof(dir), "/sys/fs/cgroup/skr8tr/%.127s", name);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) return;

    char path[384];
    snprintf(path, sizeof(path), "%s/cgroup.procs", dir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)pid);
    fclose(f);

    if (ram_bytes > 0) {
        snprintf(path, sizeof(path), "%s/memory.max", dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%lld\n", (long long)ram_bytes);
            fclose(f);
            printf("[node] cgroup: %s memory.max = %lld MB\n",
                   name, (long long)(ram_bytes / (1024 * 1024)));
        }
    }

    if (cpu_cores > 0) {
        snprintf(path, sizeof(path), "%s/cpu.max", dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d 100000\n", cpu_cores * 100000);
            fclose(f);
            printf("[node] cgroup: %s cpu.max = %d core(s)\n",
                   name, cpu_cores);
        }
    }
}

/* -------------------------------------------------------------------------
 * VM launch — build hypervisor argv and exec
 * ---------------------------------------------------------------------- */

static pid_t launch_vm(const SkrProc* sp, ManagedProc* slot,
                       char* err, size_t err_len) {
    const SkrtrVM* vm = &sp->vm;

    const char* hyp = vm->hypervisor[0] ? vm->hypervisor
                                        : "qemu-system-x86_64";

    int is_firecracker = (strstr(hyp, "firecracker") != NULL);

    char* argv[32];
    int   ai = 0;
    char  mem_s[32], smp_s[32];

    if (is_firecracker) {
        char cfg_path[256];
        snprintf(cfg_path, sizeof(cfg_path), "/tmp/skrtr_fc_%.127s.json", sp->name);
        FILE* f = fopen(cfg_path, "w");
        if (!f) {
            snprintf(err, err_len, "cannot write firecracker config: %s",
                     strerror(errno));
            return -1;
        }
        fprintf(f,
            "{\n"
            "  \"boot-source\": { \"kernel_image_path\": \"%s\","
            " \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off\" },\n"
            "  \"drives\": [{ \"drive_id\": \"rootfs\","
            " \"path_on_host\": \"%s\", \"is_root_device\": true,"
            " \"is_read_only\": false }],\n"
            "  \"machine-config\": { \"vcpu_count\": %d,"
            " \"mem_size_mib\": %d }\n"
            "}\n",
            vm->kernel, vm->rootfs,
            vm->vcpus     > 0 ? vm->vcpus     : 1,
            vm->memory_mb > 0 ? vm->memory_mb : 128);
        fclose(f);
        argv[ai++] = (char*)hyp;
        argv[ai++] = (char*)"--config-file";
        argv[ai++] = cfg_path;
        argv[ai++] = NULL;
    } else {
        snprintf(mem_s, sizeof(mem_s), "%d", vm->memory_mb > 0 ? vm->memory_mb : 128);
        snprintf(smp_s, sizeof(smp_s), "%d", vm->vcpus     > 0 ? vm->vcpus     : 1);
        argv[ai++] = (char*)hyp;
        if (vm->kernel[0]) { argv[ai++] = (char*)"-kernel"; argv[ai++] = (char*)vm->kernel; }
        if (vm->rootfs[0]) { argv[ai++] = (char*)"-hda";    argv[ai++] = (char*)vm->rootfs; }
        argv[ai++] = (char*)"-m";    argv[ai++] = mem_s;
        argv[ai++] = (char*)"-smp";  argv[ai++] = smp_s;
        argv[ai++] = (char*)"-nographic";
        if (!strcmp(vm->net, "user") || !vm->net[0]) {
            argv[ai++] = (char*)"-netdev";
            argv[ai++] = (char*)"user,id=n0";
            argv[ai++] = (char*)"-device";
            argv[ai++] = (char*)"virtio-net-pci,netdev=n0";
        }
        if (sp->port > 0) {
            static char fwd[128];
            snprintf(fwd, sizeof(fwd),
                     "user,id=n0,hostfwd=tcp::%d-:80", sp->port);
            for (int i = 0; i < ai; i++)
                if (argv[i] && !strcmp(argv[i], "user,id=n0"))
                    argv[i] = fwd;
        }
        if (vm->extra_args[0]) argv[ai++] = (char*)vm->extra_args;
        argv[ai++] = NULL;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) < 0) { pipe_fds[0] = -1; pipe_fds[1] = -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]); close(pipe_fds[1]);
        snprintf(err, err_len, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]); close(pipe_fds[1]);
        execv(hyp, argv);
        fprintf(stderr, "[node] execv failed for VM '%s': %s\n",
                sp->name, strerror(errno));
        _exit(127);
    }

    close(pipe_fds[1]);
    slot->log_pipe_r = pipe_fds[0];
    return pid;
}

/* -------------------------------------------------------------------------
 * Launch a workload — binary, WASM, or VM
 * ---------------------------------------------------------------------- */

pid_t launch_proc(const SkrProc* lp, char* err, size_t err_len) {
    proc_reap();

    pthread_mutex_lock(&g_procs_mu);
    ManagedProc* slot = proc_alloc();
    if (!slot) {
        pthread_mutex_unlock(&g_procs_mu);
        snprintf(err, err_len, "process table full");
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->name, sizeof(slot->name), "%s", lp->name);
    slot->port          = lp->port;
    slot->log_pipe_r    = -1;
    slot->restart_policy = lp->restart_policy;
    slot->drain_s        = lp->drain_s;

    /* Store full launch params for restart policy relaunch */
    slot->relaunch = *lp;
    slot->relaunch.next = NULL;   /* no linked list in stored copy */

    /* Copy health config into slot for periodic checking */
    if (lp->health.check[0]) {
        snprintf(slot->health_check,    sizeof(slot->health_check),    "%s", lp->health.check);
        snprintf(slot->health_interval, sizeof(slot->health_interval), "%s", lp->health.interval);
        slot->health_retries_max = lp->health.retries > 0
                                 ? lp->health.retries : 3;
        slot->health_failures    = 0;
        slot->health_last_check  = time(NULL);
    }
    pthread_mutex_unlock(&g_procs_mu);

    /* Provision volumes — mkdir host paths before fork */
    for (int i = 0; i < lp->volume_count; i++) {
        if (lp->volumes[i].host_path[0]) {
            volume_provision(lp->volumes[i].host_path);
            printf("[node] volume: provisioned %s → $%s\n",
                   lp->volumes[i].host_path, lp->volumes[i].env_var);
        }
    }

    pid_t pid = -1;

    if (lp->workload_type == SKRTR_TYPE_VM) {
        pthread_mutex_lock(&g_procs_mu);
        pid = launch_vm(lp, slot, err, err_len);
        pthread_mutex_unlock(&g_procs_mu);
    } else {
        /* Process launch path — binary, WASM (via wasmtime), or JOB */
        char serve_bin[600], serve_dir_arg[512], serve_port_arg[32];
        char* serve_argv[6];
        const char* bin = lp->bin[0] ? lp->bin : NULL;

        if (!bin && lp->serve.is_static) {
            char self_path[512];
            ssize_t sl = readlink("/proc/self/exe", self_path, sizeof(self_path)-1);
            if (sl > 0) {
                self_path[sl] = '\0';
                char* slash = strrchr(self_path, '/');
                if (slash) {
                    *(slash+1) = '\0';
                    snprintf(serve_bin, sizeof(serve_bin),
                             "%sskr8tr_serve", self_path);
                }
            } else {
                strncpy(serve_bin, "skr8tr_serve", sizeof(serve_bin)-1);
            }
            int sport = lp->serve.port ? lp->serve.port : 7773;
            snprintf(serve_dir_arg,  sizeof(serve_dir_arg),  "%s",
                     lp->serve.static_dir[0] ? lp->serve.static_dir : ".");
            snprintf(serve_port_arg, sizeof(serve_port_arg), "%d", sport);
            serve_argv[0] = serve_bin;
            serve_argv[1] = (char*)"--dir";
            serve_argv[2] = serve_dir_arg;
            serve_argv[3] = (char*)"--port";
            serve_argv[4] = serve_port_arg;
            serve_argv[5] = NULL;
            bin = serve_bin;
        }

        if (!bin || !bin[0]) {
            snprintf(err, err_len, "no binary path for app '%s'", lp->name);
            pthread_mutex_lock(&g_procs_mu);
            slot->active = 0;
            pthread_mutex_unlock(&g_procs_mu);
            return -1;
        }

        /* Build argv */
        static char  args_copy[512];
        static char* argv_with_args[64];
        char** argv_exec;
        if (lp->serve.is_static && !lp->bin[0]) {
            argv_exec = serve_argv;
        } else if (lp->args[0]) {
            snprintf(args_copy, sizeof(args_copy), "%s", lp->args);
            int ai2 = 0;
            argv_with_args[ai2++] = (char*)bin;
            char* tok = strtok(args_copy, " ");
            while (tok && ai2 < 62) {
                argv_with_args[ai2++] = tok;
                tok = strtok(NULL, " ");
            }
            argv_with_args[ai2] = NULL;
            argv_exec = argv_with_args;
        } else {
            static char* argv_simple[] = { NULL, NULL };
            argv_simple[0] = (char*)bin;
            argv_exec = argv_simple;
        }

        /* Build env array */
        char** injected_env = NULL; int injected_count = 0;
        if (lp->env_count > 0) {
            injected_env = calloc(lp->env_count + 1, sizeof(char*));
            if (injected_env) {
                for (int i = 0; i < lp->env_count; i++) {
                    char kv[512];
                    snprintf(kv, sizeof(kv), "%s=%s",
                             lp->env[i].key, lp->env[i].val);
                    injected_env[i] = strdup(kv);
                    injected_count++;
                }
            }
        }

        int pipe_fds[2] = {-1, -1};
        if (pipe(pipe_fds) < 0) { pipe_fds[0] = -1; pipe_fds[1] = -1; }

        pid = fork();
        if (pid < 0) {
            close(pipe_fds[0]); close(pipe_fds[1]);
            free_env_array(injected_env, injected_count);
            snprintf(err, err_len, "fork failed: %s", strerror(errno));
            pthread_mutex_lock(&g_procs_mu);
            slot->active = 0;
            pthread_mutex_unlock(&g_procs_mu);
            return -1;
        }
        if (pid == 0) {
            setpgid(0, 0);
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[0]); close(pipe_fds[1]);

            /* PORT env var */
            if (lp->port) {
                char port_env[32];
                snprintf(port_env, sizeof(port_env), "PORT=%d", lp->port);
                putenv(port_env);
            }

            /* Declared env vars */
            if (injected_env)
                for (int i = 0; injected_env[i]; i++) putenv(injected_env[i]);

            /* Secrets — injected post-fork, never logged, never in UDP payload */
            for (int si = 0; si < lp->secret_count; si++) {
                char kv[SKRMAKER_ENV_KEY + SKRMAKER_ENV_VAL + 2];
                snprintf(kv, sizeof(kv), "%s=%s",
                         lp->secrets[si].key, lp->secrets[si].val);
                putenv(strdup(kv));
            }

            /* Volumes — inject host path as env var */
            for (int vi = 0; vi < lp->volume_count; vi++) {
                if (!lp->volumes[vi].env_var[0] || !lp->volumes[vi].host_path[0]) continue;
                char kv[SKRMAKER_VOL_ENV + SKRMAKER_PATH_LEN + 2];
                snprintf(kv, sizeof(kv), "%s=%s",
                         lp->volumes[vi].env_var, lp->volumes[vi].host_path);
                putenv(strdup(kv));
            }

            execv(bin, argv_exec);
            fprintf(stderr, "[node] exec failed '%s': %s\n",
                    bin, strerror(errno));
            _exit(127);
        }
        close(pipe_fds[1]);
        free_env_array(injected_env, injected_count);

        pthread_mutex_lock(&g_procs_mu);
        slot->log_pipe_r = pipe_fds[0];
        pthread_mutex_unlock(&g_procs_mu);
    }

    if (pid < 0) return -1;

    pthread_mutex_lock(&g_procs_mu);
    slot->pid    = pid;
    slot->active = 1;
    int proc_idx = (int)(slot - g_procs);
    int log_fd   = slot->log_pipe_r;
    pthread_mutex_unlock(&g_procs_mu);

    /* Apply cgroups v2 resource limits */
    if (lp->ram_bytes > 0 || lp->cpu_cores > 0)
        cgroup_apply(lp->name, pid, lp->ram_bytes, lp->cpu_cores);

    /* Start log reader thread */
    if (log_fd >= 0) {
        LogThreadArg* la = malloc(sizeof(LogThreadArg));
        if (la) {
            la->pipe_fd  = log_fd;
            la->proc_idx = proc_idx;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&slot->log_tid, &attr, log_reader_thread, la);
            pthread_attr_destroy(&attr);
        }
    }

    printf("[node] launched: %s  pid=%d  type=%s  restart=%s  drain=%ds\n",
           lp->name, pid,
           lp->workload_type == SKRTR_TYPE_VM      ? "vm"      :
           lp->workload_type == SKRTR_TYPE_WASM     ? "wasm"    :
           lp->workload_type == SKRTR_TYPE_JOB      ? "job"     : "service",
           lp->restart_policy == SKRTR_RESTART_ALWAYS     ? "always"     :
           lp->restart_policy == SKRTR_RESTART_ON_FAILURE  ? "on-failure" : "never",
           lp->drain_s > 0 ? lp->drain_s : DRAIN_DEFAULT_S);

    /* Auto-register with Tower */
    int reg_port = lp->serve.port ? lp->serve.port
                 : lp->port       ? lp->port
                 : 0;
    if (reg_port > 0)
        tower_register(lp->name, reg_port);

    return pid;
}

/* -------------------------------------------------------------------------
 * UDP command handler
 * ---------------------------------------------------------------------- */

static void handle_command(const char* cmd, size_t cmd_len,
                            char* resp, size_t resp_len) {
    (void)cmd_len;

    /* ── PING ── */
    if (!strncmp(cmd, "PING", 4)) {
        snprintf(resp, resp_len, "OK|PONG|%s", g_node_id);
        return;
    }

    /* ── STATUS ── */
    if (!strncmp(cmd, "STATUS", 6)) {
        pthread_mutex_lock(&g_procs_mu);
        int active = 0;
        char list[2048] = {0};
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            if (!g_procs[i].active) continue;
            active++;
            char entry[160];
            snprintf(entry, sizeof(entry), "%.127s:%d", g_procs[i].name, g_procs[i].pid);
            if (list[0]) strncat(list, ",", sizeof(list)-strlen(list)-1);
            strncat(list, entry, sizeof(list)-strlen(list)-1);
        }
        pthread_mutex_unlock(&g_procs_mu);
        snprintf(resp, resp_len, "OK|STATUS|%d|%s", active, list);
        return;
    }

    /* ── LOGS ── */
    if (!strncmp(cmd, "LOGS|", 5)) {
        char name[128];
        snprintf(name, sizeof(name), "%.127s", cmd + 5);
        char* nl = strchr(name, '\n'); if (nl) *nl = '\0';

        pthread_mutex_lock(&g_procs_mu);
        ManagedProc* mp = proc_find(name);
        if (!mp) {
            pthread_mutex_unlock(&g_procs_mu);
            snprintf(resp, resp_len, "ERR|not found: %.127s", name);
            return;
        }

        char out[FABRIC_MTU - 64];
        out[0] = '\0';
        int count = mp->logs.count;
        int start = count < LOG_RING_LINES
                  ? 0
                  : mp->logs.head;

        for (int i = 0; i < count && (int)strlen(out) < (int)sizeof(out) - LOG_LINE_LEN; i++) {
            int idx = (start + i) % LOG_RING_LINES;
            strncat(out, mp->logs.lines[idx],
                    sizeof(out) - strlen(out) - 2);
            strncat(out, "\n", sizeof(out) - strlen(out) - 1);
        }
        pthread_mutex_unlock(&g_procs_mu);

        snprintf(resp, resp_len, "OK|LOGS|%.127s|%d|%s", name, count, out);
        return;
    }

    /* ── KILL ── */
    if (!strncmp(cmd, "KILL|", 5)) {
        char name[128];
        snprintf(name, sizeof(name), "%.127s", cmd + 5);
        char* nl = strchr(name, '\n'); if (nl) *nl = '\0';

        pthread_mutex_lock(&g_procs_mu);
        ManagedProc* mp = proc_find(name);
        if (!mp) {
            pthread_mutex_unlock(&g_procs_mu);
            snprintf(resp, resp_len, "ERR|not found: %.127s", name);
            return;
        }
        pid_t pid  = mp->pid;
        int   port = mp->port;

        /* Mark intentional kill so proc_reap does not relaunch or broadcast DIED */
        mp->killed_intentionally = 1;

        /* Use configured drain window, fall back to DRAIN_DEFAULT_S */
        int drain = mp->drain_s > 0 ? mp->drain_s : DRAIN_DEFAULT_S;
        pthread_mutex_unlock(&g_procs_mu);

        /* Graceful drain: SIGTERM → wait → SIGKILL */
        kill(-pid, SIGTERM);
        printf("[node] drain: SIGTERM → %s (pid %d), waiting %ds\n",
               name, pid, drain);
        struct timespec ts = { .tv_sec = drain, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        kill(-pid, SIGKILL);

        tower_deregister(name, port);

        pthread_mutex_lock(&g_procs_mu);
        mp->active = 0; mp->pid = 0;
        pthread_mutex_unlock(&g_procs_mu);

        printf("[node] killed: %s (pid %d, drain=%ds)\n", name, pid, drain);
        snprintf(resp, resp_len, "OK|KILLED|%.127s", name);
        return;
    }

    /* ── EXEC ── */
    if (!strncmp(cmd, "EXEC|", 5)) {
        /* Format: EXEC|<app_name>|<shell_command> */
        char app[128] = {0};
        char shell_cmd[512] = {0};
        const char* rest = cmd + 5;
        const char* sep  = strchr(rest, '|');
        if (!sep) {
            snprintf(resp, resp_len, "ERR|EXEC requires EXEC|app|cmd");
            return;
        }
        size_t alen = (size_t)(sep - rest);
        if (alen >= sizeof(app)) alen = sizeof(app) - 1;
        memcpy(app, rest, alen);
        app[alen] = '\0';
        strncpy(shell_cmd, sep + 1, sizeof(shell_cmd) - 1);
        /* strip trailing newline */
        char* nl = strchr(shell_cmd, '\n'); if (nl) *nl = '\0';

        /* Verify app is running */
        pthread_mutex_lock(&g_procs_mu);
        ManagedProc* mp = proc_find(app);
        if (!mp) {
            pthread_mutex_unlock(&g_procs_mu);
            snprintf(resp, resp_len, "ERR|EXEC: app not running: %.127s", app);
            return;
        }
        pthread_mutex_unlock(&g_procs_mu);

        /* Fork, exec sh -c <cmd>, capture stdout */
        int pipe_fds[2];
        if (pipe(pipe_fds) < 0) {
            snprintf(resp, resp_len, "ERR|EXEC: pipe failed: %s", strerror(errno));
            return;
        }

        pid_t epid = fork();
        if (epid < 0) {
            close(pipe_fds[0]); close(pipe_fds[1]);
            snprintf(resp, resp_len, "ERR|EXEC: fork failed: %s", strerror(errno));
            return;
        }
        if (epid == 0) {
            close(pipe_fds[0]);
            dup2(pipe_fds[1], STDOUT_FILENO);
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[1]);
            execl("/bin/sh", "sh", "-c", shell_cmd, (char*)NULL);
            _exit(127);
        }
        close(pipe_fds[1]);

        /* Collect output — capped at resp buffer */
        char out[FABRIC_MTU - 128];
        out[0] = '\0';
        int pos = 0;
        char buf[256];
        int n;
        while ((n = (int)read(pipe_fds[0], buf, sizeof(buf))) > 0) {
            int room = (int)sizeof(out) - pos - 1;
            if (room <= 0) break;
            if (n > room) n = room;
            memcpy(out + pos, buf, (size_t)n);
            pos += n;
            out[pos] = '\0';
        }
        close(pipe_fds[0]);
        waitpid(epid, NULL, 0);

        snprintf(resp, resp_len, "OK|EXEC|%.127s|%s", app, out);
        return;
    }

    /* ── LAUNCH ── */
    if (!strncmp(cmd, "LAUNCH|", 7)) {
        char name[128]={0}, bin[256]={0}, port_s[16]={0}, env_s[512]={0};
        #define FE(hay,key,out,olen) do { \
            const char* _p = strstr((hay),(key)); \
            if (_p) { _p+=strlen(key); const char* _e=strchr(_p,'|'); \
                      size_t _l=_e?(size_t)(_e-_p):strlen(_p); \
                      if(_l>=(olen)){_l=(olen)-1;} memcpy((out),_p,_l); (out)[_l]='\0'; } \
        } while(0)

        char args_s[512] = {0};
        FE(cmd+7,"name=",name, sizeof(name));
        FE(cmd+7,"bin=", bin,  sizeof(bin));
        FE(cmd+7,"port=",port_s,sizeof(port_s));
        FE(cmd+7,"args=",args_s,sizeof(args_s));
        FE(cmd+7,"env=", env_s, sizeof(env_s));
        #undef FE

        if (!name[0] || !bin[0]) {
            snprintf(resp, resp_len, "ERR|LAUNCH requires name= and bin=");
            return;
        }
        SkrProc lp = {0};
        snprintf(lp.name, sizeof(lp.name), "%s", name);
        snprintf(lp.bin,  sizeof(lp.bin),  "%s", bin);
        snprintf(lp.args, sizeof(lp.args), "%s", args_s);
        lp.port          = port_s[0] ? (int)strtol(port_s, NULL, 10) : 0;
        lp.workload_type = SKRTR_TYPE_SERVICE;
        lp.restart_policy = SKRTR_RESTART_NEVER;

        if (env_s[0]) {
            char* copy = strdup(env_s);
            char* tok  = strtok(copy, ",");
            while (tok && lp.env_count < SKRMAKER_MAX_ENV) {
                char* eq = strchr(tok, '=');
                if (eq) {
                    int i = lp.env_count++;
                    *eq = '\0';
                    strncpy(lp.env[i].key, tok,  sizeof(lp.env[i].key)-1);
                    strncpy(lp.env[i].val, eq+1, sizeof(lp.env[i].val)-1);
                }
                tok = strtok(NULL, ",");
            }
            free(copy);
        }

        char lerr[256]={0};
        pid_t pid = launch_proc(&lp, lerr, sizeof(lerr));
        if (pid < 0) snprintf(resp, resp_len, "ERR|LAUNCH failed: %.200s", lerr);
        else         snprintf(resp, resp_len, "OK|LAUNCHED|%.127s|%d", name, pid);
        return;
    }

    snprintf(resp, resp_len, "ERR|unknown command");
}

/* -------------------------------------------------------------------------
 * Heartbeat + health check thread
 * ---------------------------------------------------------------------- */

static void* heartbeat_thread(void* arg) {
    (void)arg;
    char msg[256];
    while (1) {
        proc_reap();

        /* Health check all processes with a configured probe */
        time_t now = time(NULL);
        pthread_mutex_lock(&g_procs_mu);
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            ManagedProc* mp = &g_procs[i];
            if (!mp->active || !mp->health_check[0] || mp->port <= 0) continue;

            int interval_s = 30;
            sscanf(mp->health_interval, "%d", &interval_s);
            if (interval_s < 5) interval_s = 5;

            if ((now - mp->health_last_check) < interval_s) continue;
            mp->health_last_check = now;

            int port = mp->port;
            char check[512];
            strncpy(check, mp->health_check, sizeof(check)-1);
            pthread_mutex_unlock(&g_procs_mu);

            int ok = health_probe(check, port);

            pthread_mutex_lock(&g_procs_mu);
            if (!mp->active) { pthread_mutex_unlock(&g_procs_mu); goto next_hb; }
            if (ok) {
                mp->health_failures = 0;
            } else {
                mp->health_failures++;
                printf("[node] health FAIL %d/%d: %s\n",
                       mp->health_failures, mp->health_retries_max, mp->name);
                if (mp->health_failures >= mp->health_retries_max) {
                    printf("[node] health evict: %s (pid %d)\n",
                           mp->name, mp->pid);
                    kill(-mp->pid, SIGKILL);
                    mp->active = 0;
                    mp->pid    = 0;
                }
            }
            pthread_mutex_unlock(&g_procs_mu);
            pthread_mutex_lock(&g_procs_mu);
        }
        pthread_mutex_unlock(&g_procs_mu);

next_hb:
        {
            int cpu = cpu_percent();
            long ram = ram_free_mb();
            snprintf(msg, sizeof(msg), "HEARTBEAT|%s|%d|%ld",
                     g_node_id, cpu, ram);
            fabric_broadcast(g_sock, SKRTR_PORT, msg, strlen(msg));
        }
        sleep(HEARTBEAT_INTERVAL_S);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * --run <manifest>
 * ---------------------------------------------------------------------- */

static void run_manifest(const char* path) {
    char err[512]={0};
    SkrProc* procs = skrmaker_parse(path, err, sizeof(err));
    if (!procs) { fprintf(stderr, "[node] --run: %s\n", err); return; }
    for (SkrProc* p = procs; p; p = p->next) {
        char launch_err[256]={0};
        if (launch_proc(p, launch_err, sizeof(launch_err)) < 0)
            fprintf(stderr, "[node] --run: launch failed '%s': %s\n",
                    p->name, launch_err);
    }
    skrmaker_free(procs);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    /* Parse CLI arguments */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--tower"))
            snprintf(g_tower_host, sizeof(g_tower_host), "%s", argv[i+1]);
        if (!strcmp(argv[i], "--conductor"))
            snprintf(g_conductor_host, sizeof(g_conductor_host), "%s", argv[i+1]);
    }

    printf("[node] Skr8tr Fleet Node starting...\n");
    node_identity_init();

    /* Mesh socket */
    g_sock = fabric_bind(SKRTR_PORT);
    if (g_sock < 0) {
        fprintf(stderr, "[node] FATAL: cannot bind port %d: %s\n",
                SKRTR_PORT, strerror(errno));
        return 1;
    }

    /* Command socket */
    int cmd_sock = fabric_bind(SKRTR_CMD_PORT);
    if (cmd_sock < 0) {
        fprintf(stderr, "[node] FATAL: cannot bind port %d: %s\n",
                SKRTR_CMD_PORT, strerror(errno));
        return 1;
    }

    printf("[node] mesh=UDP:%d  cmd=UDP:%d  tower=%s:%d  conductor=%s:%d\n",
           SKRTR_PORT, SKRTR_CMD_PORT,
           g_tower_host, TOWER_PORT,
           g_conductor_host, CONDUCTOR_PORT);

    /* Heartbeat + health check thread */
    pthread_t hb_tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&hb_tid, &attr, heartbeat_thread, NULL);
    pthread_attr_destroy(&attr);

    /* Prometheus metrics thread */
    pthread_t metrics_tid;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&metrics_tid, &attr, metrics_thread, NULL);
    pthread_attr_destroy(&attr);

    /* --run <manifest> flags */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--run"))
            run_manifest(argv[i+1]);

    printf("[node] node_id=%s  ready\n", g_node_id);

    char buf[FABRIC_MTU], resp[FABRIC_MTU];
    FabricAddr src;
    int max_fd = cmd_sock > g_sock ? cmd_sock : g_sock;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_sock,   &rfds);
        FD_SET(cmd_sock, &rfds);

        int ready = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) { if (errno == EINTR) continue; continue; }

        if (FD_ISSET(g_sock, &rfds)) {
            int n = fabric_recv(g_sock, buf, sizeof(buf)-1, &src);
            if (n > 0) {
                buf[n] = '\0';
                if (!strncmp(buf, "HEARTBEAT|", 10)) continue;
                resp[0] = '\0';
                handle_command(buf, (size_t)n, resp, sizeof(resp));
                if (resp[0])
                    fabric_send(g_sock, src.ip, src.port, resp, strlen(resp));
            }
        }

        if (FD_ISSET(cmd_sock, &rfds)) {
            int n = fabric_recv(cmd_sock, buf, sizeof(buf)-1, &src);
            if (n > 0) {
                buf[n] = '\0';
                resp[0] = '\0';
                handle_command(buf, (size_t)n, resp, sizeof(resp));
                if (resp[0])
                    fabric_send(cmd_sock, src.ip, src.port, resp, strlen(resp));
            }
        }
    }
    return 0;
}
