/*
 * skrmaker.c — SkrtrMaker Manifest Parser
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every daemon reads from this module.
 *
 * Parses .skr8tr manifest files into LambProc descriptor structs.
 * Supports both indentation syntax and brace syntax — both are valid.
 *
 * Zero external dependencies. Pure C23.
 *
 * Public API:
 *   LambProc* skrmaker_parse(const char* path, char* err, size_t err_len);
 *   void      skrmaker_free(LambProc* proc);
 *   void      skrmaker_dump(const LambProc* proc);
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

#define SKRMAKER_MAX_LINE    1024
#define SKRMAKER_MAX_RUNS    16
#define SKRMAKER_MAX_ENV     64
#define SKRMAKER_MAX_INPUTS  16
#define SKRMAKER_MAX_OUTPUTS 16

typedef struct {
    FILE*        fp;
    const char*  path;
    char         line[SKRMAKER_MAX_LINE];
    int          lineno;
    char*        err;
    size_t       err_len;
    int          pushed_back;   /* 1 if current line should be re-read */
} Parser;

/* -------------------------------------------------------------------------
 * Low-level line reader
 * ---------------------------------------------------------------------- */

/* Returns pointer to first non-whitespace char; strips trailing whitespace
 * and inline comments. Returns NULL on EOF. */
static char* read_line(Parser* p) {
    if (p->pushed_back) {
        p->pushed_back = 0;
        return p->line;
    }

    while (fgets(p->line, sizeof(p->line), p->fp)) {
        p->lineno++;

        /* strip trailing newline / whitespace */
        char* end = p->line + strlen(p->line) - 1;
        while (end >= p->line && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t'))
            *end-- = '\0';

        /* skip blank lines and comment lines */
        char* s = p->line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '#') continue;

        /* strip inline comments — outside of quoted strings only */
        int in_quote = 0;
        for (char* c = s; *c; c++) {
            if (*c == '"') in_quote = !in_quote;
            if (!in_quote && *c == '#') { *c = '\0'; break; }
        }
        /* re-strip trailing whitespace after comment removal */
        end = p->line + strlen(p->line) - 1;
        while (end >= p->line && (*end == ' ' || *end == '\t'))
            *end-- = '\0';

        return p->line;
    }
    return NULL;
}

static void push_back(Parser* p) {
    p->pushed_back = 1;
}

static void parse_error(Parser* p, const char* msg) {
    snprintf(p->err, p->err_len, "%s:%d: %s", p->path, p->lineno, msg);
}

/* -------------------------------------------------------------------------
 * Token helpers
 * ---------------------------------------------------------------------- */

/* Returns pointer to first non-whitespace char */
static inline char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Returns indent depth in spaces (tabs count as 2) */
static int indent_of(const char* line) {
    int depth = 0;
    for (const char* c = line; *c == ' ' || *c == '\t'; c++)
        depth += (*c == '\t') ? 2 : 1;
    return depth;
}

/* Extract key and value from "key value" or "key  value" line.
 * key_out and val_out are pointers into the line buffer (not copies). */
static void split_kv(char* line, char** key_out, char** val_out) {
    char* s = ltrim(line);
    *key_out = s;
    /* advance past key token */
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) {
        *s++ = '\0';
        *val_out = ltrim(s);
        /* unquote if quoted */
        if (**val_out == '"') {
            (*val_out)++;
            char* q = strchr(*val_out, '"');
            if (q) *q = '\0';
        }
    } else {
        *val_out = s; /* empty string */
    }
}

/* -------------------------------------------------------------------------
 * RAM parser — "256mb" → bytes
 * ---------------------------------------------------------------------- */
static int64_t parse_ram(const char* s) {
    char* end;
    double v = strtod(s, &end);
    if (end == s) return -1;
    char* u = ltrim(end);
    if      (!strcasecmp(u, "kb")) return (int64_t)(v * 1024);
    else if (!strcasecmp(u, "mb")) return (int64_t)(v * 1024 * 1024);
    else if (!strcasecmp(u, "gb")) return (int64_t)(v * 1024 * 1024 * 1024);
    else                           return (int64_t)v;  /* bare bytes */
}

/* Percent string "70%" → integer 70 */
static int parse_pct(const char* s) {
    return (int)strtol(s, NULL, 10);
}

/* -------------------------------------------------------------------------
 * Block parsers — parse indented or brace-delimited sub-blocks
 * ---------------------------------------------------------------------- */

/* Returns the minimum indent that constitutes a child of `parent_indent`.
 * In brace mode, reads until matching `}`. */

/* Parse `build` block */
static int parse_build(Parser* p, LambProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        /* brace-close exits block */
        char* s = ltrim(line);
        if (*s == '}') return 1;

        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "run")) {
            if (proc->build.run_count < SKRMAKER_MAX_RUNS) {
                strncpy(proc->build.run[proc->build.run_count++], val,
                        SKRMAKER_CMD_LEN - 1);
            }
        } else if (!strcmp(key, "out")) {
            strncpy(proc->build.out, val, sizeof(proc->build.out) - 1);
        }
    }
    return 1;
}

/* Parse `serve` block */
static int parse_serve(Parser* p, LambProc* proc, int parent_indent) {
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

/* Parse `health` block */
static int parse_health(Parser* p, LambProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);

        if (!strcmp(key, "check")) {
            /* "GET /path 200" */
            strncpy(proc->health.check, val, sizeof(proc->health.check) - 1);
        } else if (!strcmp(key, "interval")) {
            strncpy(proc->health.interval, val, sizeof(proc->health.interval) - 1);
        } else if (!strcmp(key, "timeout")) {
            strncpy(proc->health.timeout, val, sizeof(proc->health.timeout) - 1);
        } else if (!strcmp(key, "retries")) {
            proc->health.retries = (int)strtol(val, NULL, 10);
        }
    }
    return 1;
}

/* Parse `scale` block */
static int parse_scale(Parser* p, LambProc* proc, int parent_indent) {
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

/* Parse `env` block */
static int parse_env(Parser* p, LambProc* proc, int parent_indent) {
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

/* Parse `input` block (LambdaC job) */
static int parse_input(Parser* p, LambProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        /* "ldb  frame_name  as  alias" */
        char* key; char* val;
        split_kv(line, &key, &val);
        if (!strcmp(key, "ldb") && proc->input_count < SKRMAKER_MAX_INPUTS) {
            int i = proc->input_count++;
            /* val is "frame_name  as  alias" or just "frame_name" */
            char* as_ptr = strstr(val, " as ");
            if (as_ptr) {
                int len = (int)(as_ptr - val);
                strncpy(proc->inputs[i].name,  val,       (size_t)(len < 63 ? len : 63));
                proc->inputs[i].name[len < 63 ? len : 63] = '\0';
                strncpy(proc->inputs[i].alias, as_ptr + 4, sizeof(proc->inputs[i].alias) - 1);
            } else {
                strncpy(proc->inputs[i].name, val, sizeof(proc->inputs[i].name) - 1);
            }
            proc->inputs[i].type = SKRTR_IO_LDB;
        }
    }
    return 1;
}

/* Parse `output` block (LambdaC job) */
static int parse_output(Parser* p, LambProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);
        if (!strcmp(key, "ldb") && proc->output_count < SKRMAKER_MAX_OUTPUTS) {
            int i = proc->output_count++;
            strncpy(proc->outputs[i].name, val, sizeof(proc->outputs[i].name) - 1);
            proc->outputs[i].type = SKRTR_IO_LDB;
        }
    }
    return 1;
}

/* Parse `on-complete` block */
static int parse_on_complete(Parser* p, LambProc* proc, int parent_indent) {
    char* line;
    while ((line = read_line(p))) {
        char* s = ltrim(line);
        if (*s == '}') return 1;
        int ind = indent_of(line);
        if (ind <= parent_indent) { push_back(p); return 1; }

        char* key; char* val;
        split_kv(line, &key, &val);
        if (!strcmp(key, "webhook"))
            strncpy(proc->on_complete_webhook, val, sizeof(proc->on_complete_webhook) - 1);
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Top-level `app` block parser
 * ---------------------------------------------------------------------- */
static int parse_app(Parser* p, LambProc* proc) {
    /* app keyword already consumed; app name is in proc->name */
    char* line;
    int   app_indent = 2;  /* default: children are indented 2+ spaces */

    while ((line = read_line(p))) {
        char* s = ltrim(line);

        /* closing brace — end of app block */
        if (*s == '}') return 1;

        /* a new top-level `app` starts a new workload — push back */
        if (!strncmp(s, "app ", 4) || !strcmp(s, "app")) {
            push_back(p);
            return 1;
        }

        int ind = indent_of(line);
        (void)ind; /* indent check flexible — we trust the block parsers */

        char* key; char* val;
        split_kv(line, &key, &val);

        if      (!strcmp(key, "port"))     proc->port     = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "replicas")) proc->replicas = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "ram"))      proc->ram_bytes = parse_ram(val);
        else if (!strcmp(key, "cpu"))      proc->cpu_cores = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "bin"))      strncpy(proc->bin, val, sizeof(proc->bin) - 1);
        else if (!strcmp(key, "lang"))     strncpy(proc->lang, val, sizeof(proc->lang) - 1);
        else if (!strcmp(key, "src"))      strncpy(proc->src, val, sizeof(proc->src) - 1);
        else if (!strcmp(key, "nodes"))    proc->nodes = (int)strtol(val, NULL, 10);
        else if (!strcmp(key, "gpu"))      proc->gpu_optional = !strcmp(val, "optional") ? 1 : 0;
        else if (!strcmp(key, "type")) {
            if      (!strcmp(val, "job"))     proc->workload_type = SKRTR_TYPE_JOB;
            else if (!strcmp(val, "service")) proc->workload_type = SKRTR_TYPE_SERVICE;
            else if (!strcmp(val, "wasm"))    proc->workload_type = SKRTR_TYPE_WASM;
        }
        else if (!strcmp(key, "build"))       parse_build(p, proc, app_indent);
        else if (!strcmp(key, "serve"))       parse_serve(p, proc, app_indent);
        else if (!strcmp(key, "health"))      parse_health(p, proc, app_indent);
        else if (!strcmp(key, "scale"))       parse_scale(p, proc, app_indent);
        else if (!strcmp(key, "env"))         parse_env(p, proc, app_indent);
        else if (!strcmp(key, "input"))       parse_input(p, proc, app_indent);
        else if (!strcmp(key, "output"))      parse_output(p, proc, app_indent);
        else if (!strcmp(key, "on-complete")) parse_on_complete(p, proc, app_indent);
        /* unknown keys are silently accepted for forward compatibility */
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

LambProc* skrmaker_parse(const char* path, char* err, size_t err_len) {
    if (!path || !err || err_len == 0) return NULL;
    err[0] = '\0';

    FILE* fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, err_len, "skrmaker: cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    /* Allocate result list — support multiple `app` blocks in one file */
    LambProc* head = NULL;
    LambProc* tail = NULL;
    int       count = 0;

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

            LambProc* proc = calloc(1, sizeof(LambProc));
            if (!proc) {
                parse_error(&p, "out of memory");
                fclose(fp);
                skrmaker_free(head);
                return NULL;
            }

            strncpy(proc->name, val, sizeof(proc->name) - 1);
            proc->workload_type = SKRTR_TYPE_SERVICE; /* default */
            proc->replicas      = 1;

            /* look ahead: if next non-blank is `{`, enter brace mode */
            char* next = read_line(&p);
            if (next) {
                char* ns = ltrim(next);
                if (*ns != '{') push_back(&p); /* not a brace — push back */
                /* if it IS a `{`, we consume it and proceed */
            }

            if (!parse_app(&p, proc)) {
                parse_error(&p, "error in app block");
                free(proc);
                fclose(fp);
                skrmaker_free(head);
                return NULL;
            }

            /* append to linked list */
            proc->next = NULL;
            if (!head) { head = proc; tail = proc; }
            else       { tail->next = proc; tail = proc; }
            count++;
        }
        /* top-level keys outside an `app` block are silently ignored */
    }

    fclose(fp);

    if (count == 0) {
        snprintf(err, err_len, "skrmaker: no `app` blocks found in '%s'", path);
        return NULL;
    }

    return head;
}

void skrmaker_free(LambProc* proc) {
    while (proc) {
        LambProc* next = proc->next;
        free(proc);
        proc = next;
    }
}

void skrmaker_dump(const LambProc* proc) {
    for (const LambProc* p = proc; p; p = p->next) {
        printf("app         %s\n", p->name);
        printf("  type      %s\n",
               p->workload_type == SKRTR_TYPE_SERVICE ? "service" :
               p->workload_type == SKRTR_TYPE_JOB     ? "job"     :
               p->workload_type == SKRTR_TYPE_WASM    ? "wasm"    : "unknown");
        if (p->port)      printf("  port      %d\n",   p->port);
        if (p->replicas)  printf("  replicas  %d\n",   p->replicas);
        if (p->ram_bytes) printf("  ram       %lld B\n", (long long)p->ram_bytes);
        if (p->cpu_cores) printf("  cpu       %d\n",   p->cpu_cores);
        if (p->bin[0])    printf("  bin       %s\n",   p->bin);
        if (p->lang[0])   printf("  lang      %s\n",   p->lang);
        if (p->src[0])    printf("  src       %s\n",   p->src);
        if (p->nodes)     printf("  nodes     %d\n",   p->nodes);

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

        for (int i = 0; i < p->input_count; i++) {
            if (i == 0) printf("  input\n");
            if (p->inputs[i].alias[0])
                printf("    ldb  %s  as  %s\n", p->inputs[i].name, p->inputs[i].alias);
            else
                printf("    ldb  %s\n", p->inputs[i].name);
        }

        for (int i = 0; i < p->output_count; i++) {
            if (i == 0) printf("  output\n");
            printf("    ldb  %s\n", p->outputs[i].name);
        }

        if (p->on_complete_webhook[0])
            printf("  on-complete\n    webhook  %s\n", p->on_complete_webhook);

        if (p->next) printf("\n");
    }
}
