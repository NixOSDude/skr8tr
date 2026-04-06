/*
 * skrmaker.h — SkrtrMaker Manifest Parser — Public API
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 0 — Sovereign Anchor (struct definitions)
 * These structs are the canonical SkrProc descriptor. All daemons
 * read from this header. Do not modify without full downstream audit.
 *
 * Skr8tr is process-agnostic. It launches binaries and VMs — compiled from
 * any language, built by any toolchain. Source language, data formats, and
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
    SKRTR_TYPE_VM,            /* full VM — QEMU or Firecracker microVM */
} SkrtrWorkloadType;

/* -------------------------------------------------------------------------
 * Sub-structs
 * ---------------------------------------------------------------------- */

typedef struct {
    char run[SKRMAKER_MAX_RUNS][SKRMAKER_CMD_LEN];
    int  run_count;
    char out[SKRMAKER_PATH_LEN];
} SkrtrBuild;

typedef struct {
    int  is_static;
    char static_dir[SKRMAKER_PATH_LEN];
    int  port;
    char proxy_target[SKRMAKER_URL_LEN];
} SkrtrServe;

typedef struct {
    char check[SKRMAKER_CMD_LEN];   /* "GET /path 200" */
    char interval[32];
    char timeout[32];
    int  retries;
} SkrtrHealth;

typedef struct {
    int min;
    int max;
    int cpu_above;
    int cpu_below;
} SkrtrScale;

typedef struct {
    char key[SKRMAKER_ENV_KEY];
    char val[SKRMAKER_ENV_VAL];
} SkrtrEnvVar;

/* VM configuration — only populated when workload_type == SKRTR_TYPE_VM */
typedef struct {
    char hypervisor[SKRMAKER_PATH_LEN]; /* path: qemu-system-x86_64 or firecracker */
    char kernel[SKRMAKER_PATH_LEN];     /* kernel image or Firecracker kernel */
    char rootfs[SKRMAKER_PATH_LEN];     /* root disk image */
    int  vcpus;                         /* virtual CPUs */
    int  memory_mb;                     /* RAM in MB */
    char net[64];                       /* network: "user" | "tap:<iface>" */
    char extra_args[SKRMAKER_CMD_LEN];  /* additional hypervisor arguments */
} SkrtrVM;

/* -------------------------------------------------------------------------
 * SkrProc — Canonical Workload Descriptor
 *
 * One SkrProc per `app` block. Multiple apps in one .skr8tr file
 * are returned as a linked list via the `next` pointer.
 * ---------------------------------------------------------------------- */

typedef struct SkrProc {
    /* identity */
    char               name[SKRMAKER_NAME_LEN];
    SkrtrWorkloadType  workload_type;

    /* process */
    char               bin[SKRMAKER_PATH_LEN];

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
    SkrtrVM            vm;           /* populated for SKRTR_TYPE_VM */

    /* environment variables */
    SkrtrEnvVar        env[SKRMAKER_MAX_ENV];
    int                env_count;

    /* linked list */
    struct SkrProc*    next;
} SkrProc;

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

SkrProc* skrmaker_parse(const char* path, char* err, size_t err_len);
void     skrmaker_free(SkrProc* proc);
void     skrmaker_dump(const SkrProc* proc);
