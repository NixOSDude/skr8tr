/*
 * skrmaker.c — SkrtrMaker Manifest Parser
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every daemon reads from this module.
 *
 * Parses .skr8tr manifest files into SkrProc descriptor structs.
 * Supports both indentation syntax and brace syntax — both are valid.
 *
 * Skr8tr is process-agnostic. It launches binaries. Source language,
 * data formats, and analytics frameworks are not Skr8tr's concern.
 *
 * Zero external dependencies. Pure C23.
 *
 * Public API:
 *   SkrProc* skrmaker_parse(const char* path, char* err, size_t err_len);
 *   void     skrmaker_free(SkrProc* proc);
 *   void     skrmaker_dump(const SkrProc* proc);
 */

#include "skrmaker.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* -------------------------------------------------------------------------
 * Internal parser state
 * ---------------------------------------------------------------------- */

#define SKRMAKER_MAX_LINE 1024

typedef struct {
    FILE*        fp;
    const char*  path;
    char         line[SKRMAKER_MAX_LINE];
    int          lineno;
    char*        err;
    size_t       err_len;
    int          pushed_back;
} Parser;

/* -------------------------------------------------------------------------
 * Low-level line reader
 * ---------------------------------------------------------------------- */

static char* read_line(Parser* p) {
    if (p->pushed_back) {
        p->pushed_back = 0;
        return p->line;
    }

    while (fgets(p->line, sizeof(p->line), p->fp)) {
        p->lineno++;

        /* strip trailing whitespace */
        char* end = p->line + strlen(p->line) - 1;
        while (end >= p->line && (*end == '\n' || *end == '\r' ||
               *end == ' ' || *end == '\t'))
            *end-- = '\0';

        /* skip blank and comment lines */
        char* s = p->line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '#') continue;

        /* strip inline comments (outside quotes) */
        int in_quote = 0;
        for (char* c = s; *c; c++) {
            if (*c == '"') in_quote = !in_quote;
            if (!in_quote && *c == '#') { *c = '\0'; break; }
        }
        end = p->line + strlen(p->line) - 1;
        while (end >= p->line && (*end == ' ' || *end == '\t'))
            *end-- = '\0';

        return p->line;
    }
    return NULL;
}

static void push_back(Parser* p) { p->pushed_back = 1; }

static void parse_error(Parser* p, const char* msg) {
    snprintf(p->err, p->err_len, "%s:%d: %s", p->path, p->lineno, msg);
}

/* -------------------------------------------------------------------------
 * Token helpers
 * ---------------------------------------------------------------------- */

static inline char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int indent_of(const char* line) {
    int depth = 0;
    for (const char* c = line; *c == ' ' || *c == '\t'; c++)
        depth += (*c == '\t') ? 2 : 1;
    return depth;
}

static void split_kv(char* line, char** key_out, char** val_out) {
    char* s = ltrim(line);
    *key_out = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) {
        *s++ = '\0';
        *val_out = ltrim(s);
        if (**val_out == '"') {
            (*val_out)++;
            char* q = strchr(*val_out, '"');
            if (q) *q = '\0';
        }
    } else {
        *val_out = s;
    }
}

/* -------------------------------------------------------------------------
 * Value parsers
 * ---------------------------------------------------------------------- */

static int64_t parse_ram(const char* s) {
    char* end;
    double v = strtod(s, &end);
    if (end == s) return -1;
    char* u = ltrim(end);
    if      (!strcasecmp(u, "kb")) return (int64_t)(v * 1024);
    else if (!strcasecmp(u, "mb")) return (int64_t)(v * 1024 * 1024);
    else if (!strcasecmp(u, "gb")) return (int64_t)(v * 1024 * 1024 * 1024);
    else                           return (int64_t)v;
}

static int parse_pct(const char* s) {
    return (int)strtol(s, NULL, 10);
}

/* -------------------------------------------------------------------------
 * Block parsers
 * ---------------------------------------------------------------------- */

static int parse_build(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "run")) {
            if (proc->build.run_count < SKRMAKER_MAX_RUNS)
                strncpy(proc->build.run[proc->build.run_count++], val,
                        SKRMAKER_CMD_LEN - 1);
        } else if (!strcmp(key, "out")) {
            strncpy(proc->build.out, val, sizeof(proc->build.out) - 1);
        }
    }
    return 1;
}

static int parse_serve(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "static")) {
            strncpy(proc->serve.static_dir, val, sizeof(proc->serve.static_dir) - 1);
            proc->serve.is_static = 1;
        } else if (!strcmp(key, "port")) {
            proc->serve.port = (int)strtol(val, NULL, 10);
        } else if (!strcmp(key, "proxy")) {
            strncpy(proc->serve.proxy_target, val, sizeof(proc->serve.proxy_target) - 1);
        }
    }
    return 1;
}

static int parse_health(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "check"))
            strncpy(proc->health.check, val, sizeof(proc->health.check) - 1);
        else if (!strcmp(key, "interval"))
            strncpy(proc->health.interval, val, sizeof(proc->health.interval) - 1);
        else if (!strcmp(key, "timeout"))
            strncpy(proc->health.timeout, val, sizeof(proc->health.timeout) - 1);
        else if (!strcmp(key, "retries"))
            proc->health.retries = (int)strtol(val, NULL, 10);
    }
    return 1;
}

static int parse_scale(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if      (!strcmp(key, "min"))       proc->scale.min       = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "max"))       proc->scale.max       = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "cpu-above")) proc->scale.cpu_above = parse_pct(val);
        else if (!strcmp(key, "cpu-below")) proc->scale.cpu_below = parse_pct(val);
    }
    return 1;
}

static int parse_env(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (proc->env_count < SKRMAKER_MAX_ENV) {
            int i = proc->env_count++;
            strncpy(proc->env[i].key, key, sizeof(proc->env[i].key) - 1);
            strncpy(proc->env[i].val, val, sizeof(proc->env[i].val) - 1);
        }
    }
    return 1;
}

static int parse_vm(Parser* p, SkrProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if      (!strcmp(key, "hypervisor")) strncpy(proc->vm.hypervisor, val, sizeof(proc->vm.hypervisor) - 1);
        else if (!strcmp(key, "kernel"))     strncpy(proc->vm.kernel,     val, sizeof(proc->vm.kernel)     - 1);
        else if (!strcmp(key, "rootfs"))     strncpy(proc->vm.rootfs,     val, sizeof(proc->vm.rootfs)     - 1);
        else if (!strcmp(key, "vcpus"))      proc->vm.vcpus     = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "memory"))     proc->vm.memory_mb = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "net"))        strncpy(proc->vm.net,        val, sizeof(proc->vm.net)        - 1);
        else if (!strcmp(key, "args"))       strncpy(proc->vm.extra_args, val, sizeof(proc->vm.extra_args) - 1);
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Top-level `app` block parser
 * ---------------------------------------------------------------------- */

static int parse_app(Parser* p, SkrProc* proc) {
    char* line;
    int   app_indent = 2;

    while ((line = read_line(p))) {
        char* s = ltrim(line);

        if (*s == '}') return 1;

        if (!strncmp(s, "app ", 4) || !strcmp(s, "app")) {
            push_back(p);
            return 1;
        }

        char* key; char* val;
        split_kv(line, &key, &val);

        if      (!strcmp(key, "port"))     proc->port      = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "replicas")) proc->replicas  = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "ram"))      proc->ram_bytes = parse_ram(val);
        else if (!strcmp(key, "cpu"))      proc->cpu_cores = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "bin"))      strncpy(proc->bin,  val, sizeof(proc->bin)  - 1);
        else if (!strcmp(key, "exec"))     strncpy(proc->bin,  val, sizeof(proc->bin)  - 1);
        else if (!strcmp(key, "args"))     strncpy(proc->args, val, sizeof(proc->args) - 1);
        else if (!strcmp(key, "type")) {
            if      (!strcmp(val, "job"))     proc->workload_type = SKRTR_TYPE_JOB;
            else if (!strcmp(val, "service")) proc->workload_type = SKRTR_TYPE_SERVICE;
            else if (!strcmp(val, "wasm"))    proc->workload_type = SKRTR_TYPE_WASM;
            else if (!strcmp(val, "vm"))      proc->workload_type = SKRTR_TYPE_VM;
        }
        else if (!strcmp(key, "build"))  parse_build(p, proc, app_indent);
        else if (!strcmp(key, "serve"))  parse_serve(p, proc, app_indent);
        else if (!strcmp(key, "health")) parse_health(p, proc, app_indent);
        else if (!strcmp(key, "scale"))  parse_scale(p, proc, app_indent);
        else if (!strcmp(key, "env"))    parse_env(p, proc, app_indent);
        else if (!strcmp(key, "vm"))     parse_vm(p, proc, app_indent);
        /* unknown keys silently accepted for forward compatibility */
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

SkrProc* skrmaker_parse(const char* path, char* err, size_t err_len) {
    if (!path || !err || err_len == 0) return NULL;
    err[0] = '\0';

    FILE* fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, err_len, "skrmaker: cannot open '%s': %s",
                 path, strerror(errno));
        return NULL;
    }

    SkrProc* head  = NULL;
    SkrProc* tail  = NULL;
    int      count = 0;

    Parser p = {
        .fp          = fp,
        .path        = path,
        .lineno      = 0,
        .err         = err,
        .err_len     = err_len,
        .pushed_back = 0,
    };

    char* line;
    while ((line = read_line(&p))) {
        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "app")) {
            if (!val || val[0] == '\0') {
                parse_error(&p, "app name missing");
                fclose(fp);
                skrmaker_free(head);
                return NULL;
            }

            SkrProc* proc = calloc(1, sizeof(SkrProc));
            if (!proc) {
                parse_error(&p, "out of memory");
                fclose(fp);
                skrmaker_free(head);
                return NULL;
            }

            strncpy(proc->name, val, sizeof(proc->name) - 1);
            proc->workload_type = SKRTR_TYPE_SERVICE;
            proc->replicas      = 1;

            /* look ahead: consume `{` if present (brace syntax) */
            char* next = read_line(&p);
            if (next) {
                char* ns = ltrim(next);
                if (*ns != '{') push_back(&p);
            }

            if (!parse_app(&p, proc)) {
                parse_error(&p, "error in app block");
                free(proc);
                fclose(fp);
                skrmaker_free(head);
                return NULL;
            }

            proc->next = NULL;
            if (!head) { head = proc; tail = proc; }
            else       { tail->next = proc; tail = proc; }
            count++;
        }
    }

    fclose(fp);

    if (count == 0) {
        snprintf(err, err_len,
                 "skrmaker: no `app` blocks found in '%s'", path);
        return NULL;
    }

    return head;
}

void skrmaker_free(SkrProc* proc) {
    while (proc) {
        SkrProc* next = proc->next;
        free(proc);
        proc = next;
    }
}

void skrmaker_dump(const SkrProc* proc) {
    for (const SkrProc* p = proc; p; p = p->next) {
        printf("app         %s\n", p->name);
        printf("  type      %s\n",
               p->workload_type == SKRTR_TYPE_SERVICE ? "service" :
               p->workload_type == SKRTR_TYPE_JOB     ? "job"     :
               p->workload_type == SKRTR_TYPE_WASM    ? "wasm"    :
               p->workload_type == SKRTR_TYPE_VM      ? "vm"      : "unknown");
        if (p->port)      printf("  port      %d\n",   p->port);
        if (p->replicas)  printf("  replicas  %d\n",   p->replicas);
        if (p->ram_bytes) printf("  ram       %lld B\n", (long long)p->ram_bytes);
        if (p->cpu_cores) printf("  cpu       %d\n",   p->cpu_cores);
        if (p->bin[0])    printf("  bin       %s\n",   p->bin);

        if (p->build.run_count) {
            printf("  build\n");
            for (int i = 0; i < p->build.run_count; i++)
                printf("    run     %s\n", p->build.run[i]);
            if (p->build.out[0])
                printf("    out     %s\n", p->build.out);
        }

        if (p->serve.is_static || p->serve.port) {
            printf("  serve\n");
            if (p->serve.is_static)       printf("    static  %s\n", p->serve.static_dir);
            if (p->serve.port)            printf("    port    %d\n", p->serve.port);
            if (p->serve.proxy_target[0]) printf("    proxy   %s\n", p->serve.proxy_target);
        }

        if (p->health.check[0]) {
            printf("  health\n");
            printf("    check     %s\n", p->health.check);
            if (p->health.interval[0]) printf("    interval  %s\n", p->health.interval);
            if (p->health.timeout[0])  printf("    timeout   %s\n", p->health.timeout);
            if (p->health.retries)     printf("    retries   %d\n", p->health.retries);
        }

        if (p->scale.max) {
            printf("  scale\n");
            printf("    min         %d\n", p->scale.min);
            printf("    max         %d\n", p->scale.max);
            printf("    cpu-above   %d%%\n", p->scale.cpu_above);
            printf("    cpu-below   %d%%\n", p->scale.cpu_below);
        }

        for (int i = 0; i < p->env_count; i++) {
            if (i == 0) printf("  env\n");
            printf("    %s  %s\n", p->env[i].key, p->env[i].val);
        }

        if (p->workload_type == SKRTR_TYPE_VM && p->vm.kernel[0]) {
            printf("  vm\n");
            if (p->vm.hypervisor[0]) printf("    hypervisor  %s\n", p->vm.hypervisor);
            if (p->vm.kernel[0])     printf("    kernel      %s\n", p->vm.kernel);
            if (p->vm.rootfs[0])     printf("    rootfs      %s\n", p->vm.rootfs);
            if (p->vm.vcpus)         printf("    vcpus       %d\n", p->vm.vcpus);
            if (p->vm.memory_mb)     printf("    memory      %d MB\n", p->vm.memory_mb);
            if (p->vm.net[0])        printf("    net         %s\n", p->vm.net);
            if (p->vm.extra_args[0]) printf("    args        %s\n", p->vm.extra_args);
        }

        if (p->next) printf("\n");
    }
}
