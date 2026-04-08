/*
 * skr8tr_conductor_mt.h — Enterprise Multi-Tenant Namespace Module — Public API
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Provides namespace isolation and per-namespace resource quotas for the
 * Skr8tr Conductor (skr8tr_sched).
 *
 * Design goals:
 *   - Hard namespace boundaries: team A cannot submit, evict, or inspect
 *     workloads belonging to team B
 *   - Per-namespace replica quotas and CPU allocation caps
 *   - Zero overhead when no namespace is active (plain SUBMIT bypasses checks)
 *   - Thread-safe: all operations protected by internal mutex
 *
 * Integration:
 *   The RBAC gateway prefixes mutating app names with "<namespace>." before
 *   forwarding to the Conductor.  The Conductor calls mt_app_namespace() to
 *   extract the namespace component, then calls mt_quota_check() before
 *   allowing placement.
 *
 * Config file format (one namespace per line):
 *   # comment
 *   <name>:<max_replicas>:<cpu_quota_pct>
 *
 *   Example:
 *     # name           max_replicas  cpu%
 *     ml-team          20            40
 *     data-eng         10            25
 *     infra            50            80
 *
 * Default config path: /etc/skr8tr/namespaces.conf
 *
 * SSoA Level: ENTERPRISE — loaded only when compiled with -DENTERPRISE
 */

#pragma once

#include <stddef.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define MT_MAX_NAMESPACES       64
#define MT_MAX_NS_NAME          64
#define MT_NS_CONFIG_PATH       "/etc/skr8tr/namespaces.conf"
#define MT_SEPARATOR            '.'   /* namespace.appname delimiter */

/* ── Namespace record ───────────────────────────────────────────────── */

typedef struct {
    char name[MT_MAX_NS_NAME];
    int  max_replicas;      /* hard cap: 0 = unlimited */
    int  cpu_quota_pct;     /* soft cap: 0 = unlimited (informational) */
    int  current_replicas;  /* live counter maintained by mt_replica_* */
    int  active;
} MtNamespace;

/* ── Quota check result ─────────────────────────────────────────────── */

typedef enum {
    MT_OK               = 0,   /* quota OK — proceed */
    MT_ERR_UNKNOWN_NS   = 1,   /* namespace not in registry */
    MT_ERR_QUOTA_FULL   = 2,   /* max_replicas reached */
    MT_ERR_NS_INACTIVE  = 3,   /* namespace has been deactivated */
} MtStatus;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/* Load (or reload) namespace config from path.
 * Thread-safe; replaces existing table atomically.
 * Returns 0 on success, -1 on I/O error. */
int  mt_load_config(const char* path);

/* Add or update a namespace at runtime (e.g., from an admin command).
 * max_replicas == 0 → unlimited.  cpu_quota_pct == 0 → no cap.
 * Returns 0 on success, -1 if table is full. */
int  mt_namespace_add(const char* name, int max_replicas, int cpu_quota_pct);

/* Deactivate a namespace — future quota checks will return MT_ERR_NS_INACTIVE. */
int  mt_namespace_revoke(const char* name);

/* ── Per-submission quota enforcement ──────────────────────────────── */

/* Extract the namespace component from a namespaced app name.
 * "ml-team.trainer" → writes "ml-team" into ns_out (max ns_out_len bytes).
 * Returns 0 on success, -1 if no separator found (plain name → no namespace). */
int  mt_app_namespace(const char* app_name, char* ns_out, size_t ns_out_len);

/* Check whether adding one more replica to the given namespace is within quota.
 * ns_name: the namespace portion of the app name (from mt_app_namespace).
 * Returns MT_OK, MT_ERR_UNKNOWN_NS, MT_ERR_QUOTA_FULL, or MT_ERR_NS_INACTIVE. */
MtStatus mt_quota_check(const char* ns_name);

/* ── Live replica accounting ────────────────────────────────────────── */

/* Call after a replica is successfully launched. */
void mt_replica_add(const char* ns_name);

/* Call when a replica is evicted, dies, or fails to launch. */
void mt_replica_remove(const char* ns_name);

/* ── Introspection ──────────────────────────────────────────────────── */

/* Serialize the namespace table as a human-readable string.
 * Format: "ns1:max=N:used=M:cpu=P%,ns2:..." (comma-separated).
 * Writes at most out_len-1 bytes. Returns number of bytes written. */
int  mt_namespace_list(char* out, size_t out_len);

/* Look up a namespace record (read-only snapshot).
 * Returns 1 if found (copies into *dst), 0 if not found. */
int  mt_namespace_get(const char* name, MtNamespace* dst);

/* ── Human-readable error string ────────────────────────────────────── */

const char* mt_status_str(MtStatus s);
