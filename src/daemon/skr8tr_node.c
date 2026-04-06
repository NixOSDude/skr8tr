/*
 * skr8tr_node.c — Skr8tr Fleet Node Daemon
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * Responsibilities:
 *   - ML-DSA-65 ephemeral PQC identity at boot
 *   - UDP listener port 7770: LAUNCH | KILL | STATUS | PING | LOGS
 *   - Heartbeat broadcast every 5s: node_id, cpu%, ram_free_mb
 *   - Process launch: bare binary, static serve, WASM, or VM (QEMU/Firecracker)
 *   - Log capture: per-process stdout/stderr ring buffer (last 200 lines)
 *   - Health check enforcement: HTTP GET probe → kill+relaunch on failure
 *   - Tower auto-registration: REGISTER on launch, DEREGISTER on kill
 *   - --run <manifest.skr8tr>: parse and launch on startup
 *   - --tower <host>: Tower address for auto-registration (default: 127.0.0.1)
 *
 * Wire protocol (UDP 7770):
 *   LAUNCH|name=<n>|bin=<b>|port=<p>|env=<K=V,...>
 *   KILL|<app_name>
 *   STATUS
 *   LOGS|<app_name>
 *   PING
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

#define SKRTR_PORT           7770   /* mesh broadcast / heartbeat send port */
#define SKRTR_CMD_PORT       7775   /* node command port: LAUNCH/KILL/STATUS/PING/LOGS */
#define TOWER_PORT           7772
#define HEARTBEAT_INTERVAL_S 5
#define MAX_MANAGED_PROCS    256
#define NODE_ID_HEX_LEN      33
#define MAX_ENV_INJECT       64
#define LOG_RING_LINES       200
#define LOG_LINE_LEN         256
#define HEALTH_CHECK_INTERVAL 10   /* seconds between health probes */

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
    char     name[128];
    pid_t    pid;
    int      active;
    int      port;
    int      log_pipe_r;      /* read end of stdout/stderr pipe */
    pthread_t log_tid;
    LogRing  logs;
    /* health tracking */
    char     health_check[512];   /* "GET /path 200" */
    char     health_interval[32];
    int      health_retries_max;
    int      health_failures;
    time_t   health_last_check;
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

static char g_tower_host[64] = "127.0.0.1";
static int  g_sock = -1;

static void tower_register(const char* name, int port) {
    if (port <= 0) return;
    /* Get our local IP by connecting a UDP socket (no data sent) */
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
 * Health check — HTTP GET probe
 * ---------------------------------------------------------------------- */

/* Returns 1 if health check passes, 0 on failure.
 * check_str format: "GET /path <expected_code>" */
static int health_probe(const char* check_str, int port) {
    char method[16], path[256]; int expected_code = 200;
    if (sscanf(check_str, "%15s %255s %d", method, path, &expected_code) < 2)
        return 0;

    /* TCP connect to localhost:port */
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

static void proc_reap(void) {
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_procs_mu);
        for (int i = 0; i < MAX_MANAGED_PROCS; i++) {
            if (g_procs[i].active && g_procs[i].pid == pid) {
                printf("[node] process exited: %s (pid %d)\n",
                       g_procs[i].name, pid);
                tower_deregister(g_procs[i].name, g_procs[i].port);
                g_procs[i].active = 0;
                g_procs[i].pid    = 0;
                if (g_procs[i].log_pipe_r >= 0) {
                    close(g_procs[i].log_pipe_r);
                    g_procs[i].log_pipe_r = -1;
                }
                break;
            }
        }
        pthread_mutex_unlock(&g_procs_mu);
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
 * VM launch — build hypervisor argv and exec
 * QEMU: qemu-system-x86_64 -kernel K -hda R -m M -smp N -nographic
 * Firecracker: firecracker --config-file <generated json>
 * ---------------------------------------------------------------------- */

static pid_t launch_vm(const SkrProc* sp, ManagedProc* slot,
                       char* err, size_t err_len) {
    const SkrtrVM* vm = &sp->vm;

    /* Determine hypervisor — default to qemu-system-x86_64 */
    const char* hyp = vm->hypervisor[0] ? vm->hypervisor
                                        : "qemu-system-x86_64";

    int is_firecracker = (strstr(hyp, "firecracker") != NULL);

    /* Build argv */
    char* argv[32];
    int   ai = 0;
    char  mem_s[32], smp_s[32];

    if (is_firecracker) {
        /* Firecracker expects a JSON config — write one to /tmp */
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
        /* QEMU */
        snprintf(mem_s, sizeof(mem_s), "%d", vm->memory_mb > 0 ? vm->memory_mb : 128);
        snprintf(smp_s, sizeof(smp_s), "%d", vm->vcpus     > 0 ? vm->vcpus     : 1);
        argv[ai++] = (char*)hyp;
        if (vm->kernel[0]) { argv[ai++] = (char*)"-kernel"; argv[ai++] = (char*)vm->kernel; }
        if (vm->rootfs[0]) { argv[ai++] = (char*)"-hda";    argv[ai++] = (char*)vm->rootfs; }
        argv[ai++] = (char*)"-m";    argv[ai++] = mem_s;
        argv[ai++] = (char*)"-smp";  argv[ai++] = smp_s;
        argv[ai++] = (char*)"-nographic";
        /* network */
        if (!strcmp(vm->net, "user") || !vm->net[0]) {
            argv[ai++] = (char*)"-netdev";
            argv[ai++] = (char*)"user,id=n0";
            argv[ai++] = (char*)"-device";
            argv[ai++] = (char*)"virtio-net-pci,netdev=n0";
        }
        if (sp->port > 0) {
            /* Forward guest port 80 to host port sp->port via QEMU user net */
            static char fwd[128];
            snprintf(fwd, sizeof(fwd),
                     "user,id=n0,hostfwd=tcp::%d-:80", sp->port);
            /* replace the generic user netdev */
            for (int i = 0; i < ai; i++)
                if (argv[i] && !strcmp(argv[i], "user,id=n0"))
                    argv[i] = fwd;
        }
        if (vm->extra_args[0]) {
            argv[ai++] = (char*)vm->extra_args;
        }
        argv[ai++] = NULL;
    }

    /* Set up log pipe */
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
 * Launch a workload — binary or VM
 * ---------------------------------------------------------------------- */

static pid_t launch_proc(const SkrProc* lp, char* err, size_t err_len) {
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
    slot->port        = lp->port;
    slot->log_pipe_r  = -1;

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

    pid_t pid = -1;

    if (lp->workload_type == SKRTR_TYPE_VM) {
        /* VM launch path */
        pthread_mutex_lock(&g_procs_mu);
        pid = launch_vm(lp, slot, err, err_len);
        pthread_mutex_unlock(&g_procs_mu);
    } else {
        /* Process launch path */
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

        char* argv_simple[] = { (char*)bin, NULL };
        char** argv_exec    = (lp->serve.is_static && !lp->bin[0])
                            ? serve_argv : argv_simple;

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

        /* Pipe for log capture */
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
            if (lp->port) {
                char port_env[32];
                snprintf(port_env, sizeof(port_env), "PORT=%d", lp->port);
                putenv(port_env);
            }
            if (injected_env)
                for (int i = 0; injected_env[i]; i++) putenv(injected_env[i]);
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

    printf("[node] launched: %s  pid=%d  type=%s\n",
           lp->name, pid,
           lp->workload_type == SKRTR_TYPE_VM      ? "vm"      :
           lp->workload_type == SKRTR_TYPE_WASM     ? "wasm"    :
           lp->workload_type == SKRTR_TYPE_JOB      ? "job"     : "service");

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
            snprintf(entry, sizeof(entry), "%.127s:%d", g_procs[i].name, g_procs[i].pid);
            if (list[0]) strncat(list, ",", sizeof(list)-strlen(list)-1);
            strncat(list, entry, sizeof(list)-strlen(list)-1);
        }
        pthread_mutex_unlock(&g_procs_mu);
        snprintf(resp, resp_len, "OK|STATUS|%d|%s", active, list);
        return;
    }

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

        /* Build log output: last `count` lines from ring buffer */
        char out[FABRIC_MTU - 64];
        out[0] = '\0';
        int count = mp->logs.count;
        int start = count < LOG_RING_LINES
                  ? 0
                  : mp->logs.head;   /* oldest line when full */

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
        pthread_mutex_unlock(&g_procs_mu);

        kill(-pid, SIGTERM);
        struct timespec ts = { .tv_sec=2, .tv_nsec=0 };
        nanosleep(&ts, NULL);
        kill(-pid, SIGKILL);

        tower_deregister(name, port);

        pthread_mutex_lock(&g_procs_mu);
        mp->active = 0; mp->pid = 0;
        pthread_mutex_unlock(&g_procs_mu);

        printf("[node] killed: %s (pid %d)\n", name, pid);
        snprintf(resp, resp_len, "OK|KILLED|%.127s", name);
        return;
    }

    if (!strncmp(cmd, "LAUNCH|", 7)) {
        char name[128]={0}, bin[256]={0}, port_s[16]={0}, env_s[512]={0};
        /* field_extract helper */
        #define FE(hay,key,out,olen) do { \
            const char* _p = strstr((hay),(key)); \
            if (_p) { _p+=strlen(key); const char* _e=strchr(_p,'|'); \
                      size_t _l=_e?(size_t)(_e-_p):strlen(_p); \
                      if(_l>=(olen)){_l=(olen)-1;} memcpy((out),_p,_l); (out)[_l]='\0'; } \
        } while(0)

        FE(cmd+7,"name=",name, sizeof(name));
        FE(cmd+7,"bin=", bin,  sizeof(bin));
        FE(cmd+7,"port=",port_s,sizeof(port_s));
        FE(cmd+7,"env=", env_s, sizeof(env_s));
        #undef FE

        if (!name[0] || !bin[0]) {
            snprintf(resp, resp_len, "ERR|LAUNCH requires name= and bin=");
            return;
        }
        SkrProc lp = {0};
        snprintf(lp.name, sizeof(lp.name), "%s", name);
        snprintf(lp.bin,  sizeof(lp.bin),  "%s", bin);
        lp.port          = port_s[0] ? (int)strtol(port_s, NULL, 10) : 0;
        lp.workload_type = SKRTR_TYPE_SERVICE;

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

        char err[256]={0};
        pid_t pid = launch_proc(&lp, err, sizeof(err));
        if (pid < 0) snprintf(resp, resp_len, "ERR|LAUNCH failed: %.200s", err);
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

            /* Parse interval (e.g. "30s") */
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

    /* Parse --tower <host> argument */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--tower"))
            snprintf(g_tower_host, sizeof(g_tower_host), "%s", argv[i+1]);
    }

    printf("[node] Skr8tr Fleet Node starting...\n");
    node_identity_init();

    /* Mesh socket — heartbeat broadcast send / receives from conductor */
    g_sock = fabric_bind(SKRTR_PORT);
    if (g_sock < 0) {
        fprintf(stderr, "[node] FATAL: cannot bind port %d: %s\n",
                SKRTR_PORT, strerror(errno));
        return 1;
    }

    /* Command socket — dedicated port for operator and conductor commands */
    int cmd_sock = fabric_bind(SKRTR_CMD_PORT);
    if (cmd_sock < 0) {
        fprintf(stderr, "[node] FATAL: cannot bind port %d: %s\n",
                SKRTR_CMD_PORT, strerror(errno));
        return 1;
    }

    printf("[node] mesh=UDP:%d  cmd=UDP:%d  tower=%s:%d\n",
           SKRTR_PORT, SKRTR_CMD_PORT, g_tower_host, TOWER_PORT);

    pthread_t hb_tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&hb_tid, &attr, heartbeat_thread, NULL);
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

        /* Mesh socket — heartbeat ingestion (conductor sends LAUNCH/KILL here too) */
        if (FD_ISSET(g_sock, &rfds)) {
            int n = fabric_recv(g_sock, buf, sizeof(buf)-1, &src);
            if (n > 0) {
                buf[n] = '\0';
                if (!strncmp(buf, "HEARTBEAT|", 10)) continue; /* drop own broadcasts */
                resp[0] = '\0';
                handle_command(buf, (size_t)n, resp, sizeof(resp));
                if (resp[0])
                    fabric_send(g_sock, src.ip, src.port, resp, strlen(resp));
            }
        }

        /* Command socket — direct operator queries (STATUS/PING/LOGS etc.) */
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
