/*
 * skrmaker.h — SkrtrMaker Manifest Parser — Public API
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 0 — Sovereign Anchor (struct definitions)
 * These structs are the canonical SkrProc descriptor. All daemons
 * read from this header. Do not modify without full downstream audit.
 *
 * Skr8tr is process-agnostic. It launches binaries — compiled from any
 * language, built by any toolchain. Source language, data formats, and
 * analytics frameworks are outside Skr8tr's scope.
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

/* -------------------------------------------------------------------------
 * Workload type
 * ---------------------------------------------------------------------- */

typedef enum {
    SKRTR_TYPE_SERVICE = 0,   /* long-running process (default) */
    SKRTR_TYPE_JOB,           /* run-to-completion binary */
    SKRTR_TYPE_WASM,          /* WASM module via wasmtime */
} SkrtrWorkloadType;

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
 * SkrProc — Canonical Workload Descriptor
 *
 * One SkrProc per `app` block. Multiple apps in one .skr8tr file
 * are returned as a linked list via the `next` pointer.
 *
 * Skr8tr launches bare processes. The binary can be compiled from any
 * language — C, Rust, Go, LambdaC, WASM, anything. Skr8tr does not care.
 * ---------------------------------------------------------------------- */

typedef struct SkrProc {
    /* identity */
    char               name[SKRMAKER_NAME_LEN];
    SkrtrWorkloadType  workload_type;

    /* process */
    char               bin[SKRMAKER_PATH_LEN];   /* path to binary */

    /* resources */
    int                port;
    int                replicas;
    int64_t            ram_bytes;
    int                cpu_cores;

    /* sub-blocks */
    SkrtrBuild         build;
    SkrtrServe         serve;
    SkrtrHealth        health;
    SkrtrScale         scale;

    /* environment variables injected into each instance */
    SkrtrEnvVar        env[SKRMAKER_MAX_ENV];
    int                env_count;

    /* linked list — multiple apps per file */
    struct SkrProc*    next;
} SkrProc;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * skrmaker_parse — Parse a .skr8tr manifest file.
 *
 * Returns the head of a SkrProc linked list (one node per `app` block),
 * or NULL on error. On error, `err` is populated with a human-readable
 * message including file path and line number.
 *
 * Caller owns the returned list; free with skrmaker_free().
 */
SkrProc* skrmaker_parse(const char* path, char* err, size_t err_len);

/*
 * skrmaker_free — Release all memory allocated by skrmaker_parse().
 */
void skrmaker_free(SkrProc* proc);

/*
 * skrmaker_dump — Print a parsed SkrProc list to stdout (debug).
 */
void skrmaker_dump(const SkrProc* proc);
