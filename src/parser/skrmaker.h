/*
 * skrmaker.h — SkrtrMaker Manifest Parser — Public API
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 0 — Sovereign Anchor (struct definitions)
 * These structs are the canonical LambProc descriptor. All daemons
 * read from this header. Do not modify without full downstream audit.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SKRMAKER_NAME_LEN   128
#define SKRMAKER_PATH_LEN   256
#define SKRMAKER_CMD_LEN    512
#define SKRMAKER_URL_LEN    256
#define SKRMAKER_ENV_KEY    64
#define SKRMAKER_ENV_VAL    256
#define SKRMAKER_MAX_RUNS   16
#define SKRMAKER_MAX_ENV    64
#define SKRMAKER_MAX_IO     16

/* -------------------------------------------------------------------------
 * Workload type
 * ---------------------------------------------------------------------- */

typedef enum {
    SKRTR_TYPE_SERVICE = 0,   /* long-running process (default) */
    SKRTR_TYPE_JOB,           /* run-to-completion batch job */
    SKRTR_TYPE_WASM,          /* WASM module via wasmtime */
} SkrtrWorkloadType;

/* -------------------------------------------------------------------------
 * I/O descriptor (for LambdaC jobs: input/output LDB frames)
 * ---------------------------------------------------------------------- */

typedef enum {
    SKRTR_IO_LDB = 0,
    SKRTR_IO_FILE,
} SkrtrIOType;

typedef struct {
    char        name[SKRMAKER_NAME_LEN];
    char        alias[SKRMAKER_NAME_LEN];   /* "as alias" — may be empty */
    SkrtrIOType type;
} SkrtrIO;

/* -------------------------------------------------------------------------
 * Sub-structs
 * ---------------------------------------------------------------------- */

typedef struct {
    char run[SKRMAKER_MAX_RUNS][SKRMAKER_CMD_LEN];
    int  run_count;
    char out[SKRMAKER_PATH_LEN];   /* build output directory */
} SkrtrBuild;

typedef struct {
    int  is_static;
    char static_dir[SKRMAKER_PATH_LEN];
    int  port;
    char proxy_target[SKRMAKER_URL_LEN];
} SkrtrServe;

typedef struct {
    char check[SKRMAKER_CMD_LEN];       /* "GET /path 200" */
    char interval[32];
    char timeout[32];
    int  retries;
} SkrtrHealth;

typedef struct {
    int min;
    int max;
    int cpu_above;   /* percent threshold to scale up */
    int cpu_below;   /* percent threshold to scale down */
} SkrtrScale;

typedef struct {
    char key[SKRMAKER_ENV_KEY];
    char val[SKRMAKER_ENV_VAL];
} SkrtrEnvVar;

/* -------------------------------------------------------------------------
 * LambProc — Canonical Workload Descriptor
 *
 * One LambProc per `app` block. Multiple apps in one .skr8tr file
 * are returned as a linked list via the `next` pointer.
 * ---------------------------------------------------------------------- */

typedef struct LambProc {
    /* identity */
    char               name[SKRMAKER_NAME_LEN];
    SkrtrWorkloadType  workload_type;

    /* process / binary */
    char               bin[SKRMAKER_PATH_LEN];   /* pre-built binary path */
    char               lang[32];                  /* "lambdac" | "wasm" | "" */
    char               src[SKRMAKER_PATH_LEN];    /* source file (lang != "") */

    /* resources */
    int                port;
    int                replicas;
    int64_t            ram_bytes;
    int                cpu_cores;
    int                nodes;       /* mesh nodes for distributed jobs */
    int                gpu_optional;

    /* sub-blocks */
    SkrtrBuild         build;
    SkrtrServe         serve;
    SkrtrHealth        health;
    SkrtrScale         scale;

    /* environment variables */
    SkrtrEnvVar        env[SKRMAKER_MAX_ENV];
    int                env_count;

    /* LambdaC job I/O */
    SkrtrIO            inputs[SKRMAKER_MAX_IO];
    int                input_count;
    SkrtrIO            outputs[SKRMAKER_MAX_IO];
    int                output_count;

    /* lifecycle hooks */
    char               on_complete_webhook[SKRMAKER_URL_LEN];

    /* linked list — multiple apps per file */
    struct LambProc*   next;
} LambProc;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * skrmaker_parse — Parse a .skr8tr manifest file.
 *
 * Returns the head of a LambProc linked list (one node per `app` block),
 * or NULL on error. On error, `err` is populated with a human-readable
 * message including file path and line number.
 *
 * Caller owns the returned list; free with skrmaker_free().
 */
LambProc* skrmaker_parse(const char* path, char* err, size_t err_len);

/*
 * skrmaker_free — Release all memory allocated by skrmaker_parse().
 */
void skrmaker_free(LambProc* proc);

/*
 * skrmaker_dump — Print a parsed LambProc list to stdout (debug).
 */
void skrmaker_dump(const LambProc* proc);
