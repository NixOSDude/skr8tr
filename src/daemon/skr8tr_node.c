/*
 * skr8tr_node.c — Skr8tr Fleet Node Daemon
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. This is the worker fabric of the mesh.
 *
 * Responsibilities:
 *   - ML-DSA-65 ephemeral PQC identity generated at boot
 *   - UDP listener on port 7770 (mesh command channel)
 *   - Commands: LAUNCH | KILL | STATUS | PING
 *   - Heartbeat broadcast every 5s: node_id, cpu%, ram_free_mb
 *   - Process table: tracks all managed child processes
 *   - --run <manifest.skr8tr>: parse and launch immediately on startup
 *
 * Wire protocol (UDP datagrams, newline-terminated):
 *   LAUNCH|name=<app>|bin=<path>|port=<n>|env=<K=V,...>
 *   KILL|<app_name>
 *   STATUS
 *   PING
 *
 * Responses:
 *   OK|LAUNCHED|<app_name>|<pid>
 *   OK|KILLED|<app_name>
 *   OK|STATUS|<n_procs>|<name:pid:active,...>
 *   OK|PONG|<node_id>
 *   ERR|<reason>
 *
 * Heartbeat broadcast (to 255.255.255.255:7770):
 *   HEARTBEAT|<node_id>|<cpu_pct>|<ram_free_mb>
 */

#include "../core/fabric.h"
#include "../parser/skrmaker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <oqs/oqs.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SKRTR_PORT           7770
#define HEARTBEAT_INTERVAL_S 5
#define MAX_MANAGED_PROCS    256
#define NODE_ID_HEX_LEN      33    /* 16 bytes → 32 hex chars + NUL */
#define MAX_ENV_INJECT       64

/* -------------------------------------------------------------------------
 * Process table
 * ---------------------------------------------------------------------- */

typedef struct {
    char  name[128];
    pid_t pid;
    int   active;       /* 1 = running, 0 = slot free */
    int   port;
} ManagedProc;

static ManagedProc   g_procs[MAX_MANAGED_PROCS];
static pthread_mutex_t g_procs_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Node identity
 * ---------------------------------------------------------------------- */

static char g_node_id[NODE_ID_HEX_LEN];   /* hex of first 16B of ML-DSA-65 pubkey */

/* Generate ephemeral ML-DSA-65 identity. Node ID is the hex of the first
 * 16 bytes of the public key — compact, collision-resistant, PQC-rooted. */
static int node_identity_init(void) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!sig) {
        fprintf(stderr, "[node] WARN: liboqs ML-DSA-65 unavailable, "
                        "falling back to random identity\n");
        /* Fallback: 16 random bytes from /dev/urandom */
        uint8_t rand_id[16];
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, rand_id, 16) != 16) {
            snprintf(g_node_id, sizeof(g_node_id), "deadbeefdeadbeef");
            if (fd >= 0) close(fd);
            return 0;
        }
        close(fd);
        for (int i = 0; i < 16; i++)
            snprintf(g_node_id + i * 2, 3, "%02x", rand_id[i]);
        return 1;
    }

    uint8_t* pub_key  = malloc(sig->length_public_key);
    uint8_t* priv_key = malloc(sig->length_secret_key);
    if (!pub_key || !priv_key) {
        free(pub_key); free(priv_key);
        OQS_SIG_free(sig);
        return 0;
    }

    if (OQS_SIG_keypair(sig, pub_key, priv_key) != OQS_SUCCESS) {
        free(pub_key); free(priv_key);
        OQS_SIG_free(sig);
        return 0;
    }

    /* Node ID: hex of first 16 bytes of public key */
    for (int i = 0; i < 16; i++)
        snprintf(g_node_id + i * 2, 3, "%02x", pub_key[i]);
    g_node_id[32] = '\0';

    /* Zero and free key material — ephemeral, not stored */
    memset(priv_key, 0, sig->length_secret_key);
    free(pub_key);
    free(priv_key);
    OQS_SIG_free(sig);

    printf("[node] PQC identity: ML-DSA-65 node_id=%s\n", g_node_id);
    return 1;
}

/* -------------------------------------------------------------------------
 * System metrics
 * ---------------------------------------------------------------------- */

static int cpu_percent(void) {
    /* Read /proc/stat for one-shot CPU % estimate across 100ms */
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;

    unsigned long long u1, n1, s1, i1, rest1;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu",
               &u1, &n1, &s1, &i1, &rest1) != 5) {
        fclose(f); return 0;
    }
    fclose(f);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
    nanosleep(&ts, NULL);

    f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long u2, n2, s2, i2, rest2;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu",
               &u2, &n2, &s2, &i2, &rest2) != 5) {
        fclose(f); return 0;
    }
    fclose(f);

    unsigned long long active = (u2 + n2 + s2) - (u1 + n1 + s1);
    unsigned long long total  = active + (i2 - i1);
    if (total == 0) return 0;
    return (int)((active * 100) / total);
}

static long ram_free_mb(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    long free_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %ld kB", &free_kb) == 1) break;
    }
    fclose(f);
    return free_kb / 1024;
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

/* Reap any zombie children — called before LAUNCH and in heartbeat loop */
static void proc_reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_procs_mu);
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            if (g_procs[i].active && g_procs[i].pid == pid) {
                printf("[node] process exited: %s (pid %d)\n",
                       g_procs[i].name, pid);
                g_procs[i].active = 0;
                g_procs[i].pid    = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_procs_mu);
    }
}

/* -------------------------------------------------------------------------
 * LAUNCH — fork/exec a workload from a LambProc descriptor
 * ---------------------------------------------------------------------- */

static void free_env_array(char** env, int count) {
    if (!env) return;
    for (int i = 0; i < count; i++) free(env[i]);
    free(env);
}

/*
 * launch_proc — fork and exec a workload described by a LambProc.
 * Populates a ManagedProc slot.
 * Returns pid on success, -1 on error.
 */
static pid_t launch_proc(const LambProc* lp, char* err, size_t err_len) {
    /* Determine the binary path.
     * Static serve workloads exec skr8tr_serve from the same directory
     * as this node binary, passing --dir and --port arguments. */
    char serve_bin[600];
    char serve_dir_arg[512];
    char serve_port_arg[32];
    char* serve_argv[6];   /* skr8tr_serve --dir <d> --port <p> NULL */

    const char* bin = lp->bin[0] ? lp->bin : NULL;

    if (!bin && lp->serve.is_static) {
        /* Locate skr8tr_serve next to the running node binary */
        char self_path[512];
        ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (self_len > 0) {
            self_path[self_len] = '\0';
            char* last_slash    = strrchr(self_path, '/');
            if (last_slash) {
                *(last_slash + 1) = '\0';
                snprintf(serve_bin, sizeof(serve_bin), "%sskr8tr_serve", self_path);
            }
        } else {
            strncpy(serve_bin, "skr8tr_serve", sizeof(serve_bin) - 1);
        }

        int serve_port = lp->serve.port ? lp->serve.port : 7773;
        snprintf(serve_dir_arg,  sizeof(serve_dir_arg),  "%s",
                 lp->serve.static_dir[0] ? lp->serve.static_dir : ".");
        snprintf(serve_port_arg, sizeof(serve_port_arg), "%d", serve_port);

        serve_argv[0] = serve_bin;
        serve_argv[1] = (char*)"--dir";
        serve_argv[2] = serve_dir_arg;
        serve_argv[3] = (char*)"--port";
        serve_argv[4] = serve_port_arg;
        serve_argv[5] = NULL;

        bin = serve_bin;
    }

    if (!bin || bin[0] == '\0') {
        snprintf(err, err_len, "no binary path for app '%s'", lp->name);
        return -1;
    }

    proc_reap();

    pthread_mutex_lock(&g_procs_mu);
    ManagedProc* slot = proc_alloc();
    if (!slot) {
        pthread_mutex_unlock(&g_procs_mu);
        snprintf(err, err_len, "process table full");
        return -1;
    }
    /* Reserve the slot before fork so heartbeat sees it immediately */
    snprintf(slot->name, sizeof(slot->name), "%s", lp->name);
    slot->pid    = 0;
    slot->active = 0;   /* mark active after fork succeeds */
    slot->port   = lp->port;
    pthread_mutex_unlock(&g_procs_mu);

    /* argv: use serve_argv for static workloads, otherwise { bin, NULL } */
    char* argv_simple[] = { (char*)bin, NULL };
    char** argv_exec = lp->serve.is_static && !lp->bin[0]
                     ? serve_argv : argv_simple;

    /* Build envp from LambProc env block */
    char** injected_env = NULL;
    int    injected_count = 0;

    /* Construct K=V strings from the LambProc env array */
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
            injected_env[injected_count] = NULL;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        free_env_array(injected_env, injected_count);
        snprintf(err, err_len, "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child — set process group so we can SIGTERM the whole group */
        setpgid(0, 0);

        /* Inject PORT env var so apps can read their assigned port */
        if (lp->port) {
            char port_env[32];
            snprintf(port_env, sizeof(port_env), "PORT=%d", lp->port);
            putenv(port_env);
        }

        /* Inject any declared env vars */
        if (injected_env) {
            for (int i = 0; injected_env[i]; i++)
                putenv(injected_env[i]);
        }

        execv(bin, argv_exec);
        /* If we reach here, exec failed */
        fprintf(stderr, "[node] exec failed for '%s': %s\n",
                bin, strerror(errno));
        _exit(127);
    }

    /* Parent */
    free_env_array(injected_env, injected_count);

    pthread_mutex_lock(&g_procs_mu);
    slot->pid    = pid;
    slot->active = 1;
    pthread_mutex_unlock(&g_procs_mu);

    printf("[node] launched: %s  bin=%s  pid=%d\n", lp->name, bin, pid);
    return pid;
}

/* -------------------------------------------------------------------------
 * Command parser for UDP datagrams
 *
 * LAUNCH|name=<app>|bin=<path>|port=<n>|env=<K=V,...>
 * KILL|<app_name>
 * STATUS
 * PING
 * ---------------------------------------------------------------------- */

/* Extract value for "key=value" within a pipe-delimited token string.
 * `haystack` is the full datagram. `key` is e.g. "name=".
 * Writes value into `out` (up to `out_len - 1` chars). */
static void field_extract(const char* haystack, const char* key,
                           char* out, size_t out_len) {
    out[0] = '\0';
    const char* p = strstr(haystack, key);
    if (!p) return;
    p += strlen(key);
    const char* end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void handle_command(const char* cmd, size_t cmd_len,
                            char* resp, size_t resp_len) {
    (void)cmd_len;

    if (!strncmp(cmd, "PING", 4)) {
        snprintf(resp, resp_len, "OK|PONG|%s", g_node_id);
        return;
    }

    if (!strncmp(cmd, "STATUS", 6)) {
        pthread_mutex_lock(&g_procs_mu);
        int active = 0;
        char list[2048] = {0};
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            if (!g_procs[i].active) continue;
            active++;
            char entry[160];
            snprintf(entry, sizeof(entry), "%.127s:%d",
                     g_procs[i].name, g_procs[i].pid);
            if (list[0]) strncat(list, ",", sizeof(list) - strlen(list) - 1);
            strncat(list, entry, sizeof(list) - strlen(list) - 1);
        }
        pthread_mutex_unlock(&g_procs_mu);
        snprintf(resp, resp_len, "OK|STATUS|%d|%s", active, list);
        return;
    }

    if (!strncmp(cmd, "KILL|", 5)) {
        const char* app_name = cmd + 5;
        char name[128] = {0};
        /* strip trailing newline */
        snprintf(name, sizeof(name), "%.127s", app_name);
        char* nl = strchr(name, '\n');
        if (nl) *nl = '\0';

        pthread_mutex_lock(&g_procs_mu);
        ManagedProc* mp = proc_find(name);
        if (!mp) {
            pthread_mutex_unlock(&g_procs_mu);
            snprintf(resp, resp_len, "ERR|not found: %s", name);
            return;
        }
        pid_t pid = mp->pid;
        pthread_mutex_unlock(&g_procs_mu);

        /* SIGTERM to process group */
        kill(-pid, SIGTERM);

        /* Brief wait then SIGKILL if still alive */
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        kill(-pid, SIGKILL);

        pthread_mutex_lock(&g_procs_mu);
        mp->active = 0;
        mp->pid    = 0;
        pthread_mutex_unlock(&g_procs_mu);

        printf("[node] killed: %s (pid %d)\n", name, pid);
        snprintf(resp, resp_len, "OK|KILLED|%s", name);
        return;
    }

    if (!strncmp(cmd, "LAUNCH|", 7)) {
        /* Parse fields from the datagram */
        char name[128] = {0};
        char bin[256]  = {0};
        char port_s[16]= {0};
        char env_s[512]= {0};

        field_extract(cmd + 7, "name=", name,  sizeof(name));
        field_extract(cmd + 7, "bin=",  bin,   sizeof(bin));
        field_extract(cmd + 7, "port=", port_s,sizeof(port_s));
        field_extract(cmd + 7, "env=",  env_s, sizeof(env_s));

        if (!name[0] || !bin[0]) {
            snprintf(resp, resp_len, "ERR|LAUNCH requires name= and bin=");
            return;
        }

        /* Build a minimal LambProc from the extracted fields */
        LambProc lp = {0};
        snprintf(lp.name, sizeof(lp.name), "%s", name);
        snprintf(lp.bin,  sizeof(lp.bin),  "%s", bin);
        lp.port         = port_s[0] ? (int)strtol(port_s, NULL, 10) : 0;
        lp.workload_type = SKRTR_TYPE_SERVICE;

        /* Parse env K=V,K2=V2 into LambProc env array */
        if (env_s[0]) {
            char* copy = strdup(env_s);
            char* tok  = strtok(copy, ",");
            while (tok && lp.env_count < SKRMAKER_MAX_ENV) {
                char* eq = strchr(tok, '=');
                if (eq) {
                    int i = lp.env_count++;
                    *eq = '\0';
                    strncpy(lp.env[i].key, tok, sizeof(lp.env[i].key) - 1);
                    strncpy(lp.env[i].val, eq + 1, sizeof(lp.env[i].val) - 1);
                }
                tok = strtok(NULL, ",");
            }
            free(copy);
        }

        char err[256] = {0};
        pid_t pid = launch_proc(&lp, err, sizeof(err));
        if (pid < 0)
            snprintf(resp, resp_len, "ERR|LAUNCH failed: %s", err);
        else
            snprintf(resp, resp_len, "OK|LAUNCHED|%s|%d", name, pid);
        return;
    }

    snprintf(resp, resp_len, "ERR|unknown command");
}

/* -------------------------------------------------------------------------
 * Heartbeat thread — broadcasts every HEARTBEAT_INTERVAL_S seconds
 * ---------------------------------------------------------------------- */

static int g_sock = -1;

static void* heartbeat_thread(void* arg) {
    (void)arg;
    char msg[256];
    while (1) {
        proc_reap();
        int cpu = cpu_percent();
        long ram = ram_free_mb();
        snprintf(msg, sizeof(msg), "HEARTBEAT|%s|%d|%ld",
                 g_node_id, cpu, ram);
        fabric_broadcast(g_sock, SKRTR_PORT, msg, strlen(msg));
        sleep(HEARTBEAT_INTERVAL_S);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * --run <manifest.skr8tr> : parse and launch on startup
 * ---------------------------------------------------------------------- */

static void run_manifest(const char* path) {
    char err[512] = {0};
    LambProc* procs = skrmaker_parse(path, err, sizeof(err));
    if (!procs) {
        fprintf(stderr, "[node] --run: parse failed: %s\n", err);
        return;
    }

    for (LambProc* p = procs; p; p = p->next) {
        char launch_err[256] = {0};
        pid_t pid = launch_proc(p, launch_err, sizeof(launch_err));
        if (pid < 0)
            fprintf(stderr, "[node] --run: launch failed for '%s': %s\n",
                    p->name, launch_err);
    }

    skrmaker_free(procs);
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    /* Ignore SIGPIPE — UDP writes to a closed socket should not abort */
    signal(SIGPIPE, SIG_IGN);
    /* Automatically reap children — avoids zombie accumulation */
    signal(SIGCHLD, SIG_DFL);

    printf("[node] Skr8tr Fleet Node starting...\n");

    /* Generate ephemeral PQC identity */
    node_identity_init();

    /* Bind mesh socket */
    g_sock = fabric_bind(SKRTR_PORT);
    if (g_sock < 0) {
        fprintf(stderr, "[node] FATAL: cannot bind UDP port %d: %s\n",
                SKRTR_PORT, strerror(errno));
        return 1;
    }
    printf("[node] listening on UDP port %d\n", SKRTR_PORT);

    /* Start heartbeat thread */
    pthread_t hb_tid;
    if (pthread_create(&hb_tid, NULL, heartbeat_thread, NULL) != 0) {
        fprintf(stderr, "[node] FATAL: cannot start heartbeat thread\n");
        return 1;
    }
    pthread_detach(hb_tid);

    /* --run <manifest> on startup */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--run")) {
            run_manifest(argv[i + 1]);
            i++;  /* skip the path argument */
        }
    }

    printf("[node] node_id=%s  ready\n", g_node_id);

    /* Command loop */
    char buf[FABRIC_MTU];
    char resp[FABRIC_MTU];

    for (;;) {
        FabricAddr src;
        int n = fabric_recv(g_sock, buf, sizeof(buf) - 1, &src);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[node] recv error: %s\n", strerror(errno));
            continue;
        }
        buf[n] = '\0';

        /* Ignore our own heartbeat broadcasts */
        if (!strncmp(buf, "HEARTBEAT|", 10)) continue;

        resp[0] = '\0';
        handle_command(buf, (size_t)n, resp, sizeof(resp));

        if (resp[0])
            fabric_send(g_sock, src.ip, src.port, resp, strlen(resp));
    }

    return 0;
}
