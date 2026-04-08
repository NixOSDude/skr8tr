/*
 * skr8tr_conductor_mt.c — Enterprise Multi-Tenant Namespace Module
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Implements namespace isolation and resource quota enforcement for the
 * Skr8tr Conductor.  This module is stateful but fully thread-safe.
 *
 * Integration points in skr8tr_sched.c (all guarded with #ifdef ENTERPRISE):
 *
 *   1. On startup:
 *        mt_load_config(MT_NS_CONFIG_PATH);
 *
 *   2. On SUBMIT, after parsing the app name:
 *        char ns[MT_MAX_NS_NAME];
 *        if (mt_app_namespace(app_name, ns, sizeof(ns)) == 0) {
 *            MtStatus st = mt_quota_check(ns);
 *            if (st != MT_OK) {
 *                snprintf(resp, resp_len, "ERR|QUOTA: %s", mt_status_str(st));
 *                return;
 *            }
 *        }
 *        // ... launch replica ...
 *        if (launched_ok && ns[0])
 *            mt_replica_add(ns);
 *
 *   3. On EVICT or replica death:
 *        char ns[MT_MAX_NS_NAME];
 *        if (mt_app_namespace(app_name, ns, sizeof(ns)) == 0)
 *            mt_replica_remove(ns);
 *
 *   4. On NAMESPACE_LIST admin command:
 *        char buf[4096];
 *        mt_namespace_list(buf, sizeof(buf));
 *        snprintf(resp, resp_len, "OK|NAMESPACES|%s", buf);
 *
 * Build: compiled as part of skr8tr_sched.c when ENTERPRISE=1
 *
 * SSoA Level: ENTERPRISE
 */

#include "skr8tr_conductor_mt.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * In-memory namespace table — protected by g_ns_mu
 * ---------------------------------------------------------------------- */

static MtNamespace     g_ns[MT_MAX_NAMESPACES];
static int             g_ns_count = 0;
static pthread_mutex_t g_ns_mu    = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Find by name — caller must hold g_ns_mu */
static MtNamespace* ns_find_locked(const char* name) {
    for (int i = 0; i < g_ns_count; i++)
        if (g_ns[i].active && !strcmp(g_ns[i].name, name))
            return &g_ns[i];
    return NULL;
}

static void ns_set_name(MtNamespace* n, const char* name) {
    memset(n->name, 0, MT_MAX_NS_NAME);
    size_t len = strlen(name);
    if (len >= MT_MAX_NS_NAME) len = MT_MAX_NS_NAME - 1;
    memcpy(n->name, name, len);
}

/* -------------------------------------------------------------------------
 * mt_load_config — parse flat-file namespace registry
 * ---------------------------------------------------------------------- */

int mt_load_config(const char* path) {
    if (!path) path = MT_NS_CONFIG_PATH;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            printf("[mt] namespace config not found — no namespaces registered: %s\n",
                   path);
            return 0;
        }
        fprintf(stderr, "[mt] cannot open namespace config '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    MtNamespace tmp[MT_MAX_NAMESPACES];
    int count = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp) && count < MT_MAX_NAMESPACES) {
        /* Strip newline */
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        /* Skip comments and blanks */
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;

        /* Format: name:max_replicas:cpu_quota_pct */
        char name[MT_MAX_NS_NAME] = {0};
        char max_r_s[16] = {0}, cpu_s[16] = {0};

        char* col = strchr(s, ':');
        if (!col) continue;
        size_t nl2 = (size_t)(col - s);
        if (nl2 >= MT_MAX_NS_NAME) continue;
        memcpy(name, s, nl2);
        s = col + 1;

        col = strchr(s, ':');
        if (!col) continue;
        size_t ml = (size_t)(col - s);
        if (ml >= sizeof(max_r_s)) ml = sizeof(max_r_s) - 1;
        memcpy(max_r_s, s, ml);
        s = col + 1;

        /* cpu_quota_pct: rest of line (strip trailing whitespace) */
        size_t cl = strlen(s);
        while (cl > 0 && (s[cl-1] == ' ' || s[cl-1] == '\t' || s[cl-1] == '\r'))
            cl--;
        if (cl >= sizeof(cpu_s)) cl = sizeof(cpu_s) - 1;
        memcpy(cpu_s, s, cl);

        MtNamespace* n = &tmp[count];
        memset(n, 0, sizeof(*n));
        ns_set_name(n, name);
        n->max_replicas  = (int)strtol(max_r_s, NULL, 10);
        n->cpu_quota_pct = (int)strtol(cpu_s,   NULL, 10);
        n->active        = 1;
        /* current_replicas starts at 0 — will be rebuilt from live state */

        count++;
        printf("[mt] namespace '%s'  max_replicas=%d  cpu_quota=%d%%\n",
               name, n->max_replicas, n->cpu_quota_pct);
    }
    fclose(fp);

    pthread_mutex_lock(&g_ns_mu);
    /* Preserve live replica counts for namespaces that survived the reload */
    for (int i = 0; i < count; i++) {
        MtNamespace* existing = ns_find_locked(tmp[i].name);
        if (existing) tmp[i].current_replicas = existing->current_replicas;
    }
    memcpy(g_ns, tmp, (size_t)count * sizeof(MtNamespace));
    g_ns_count = count;
    pthread_mutex_unlock(&g_ns_mu);

    printf("[mt] %d namespace(s) loaded from '%s'\n", count, path);
    return 0;
}

/* -------------------------------------------------------------------------
 * mt_namespace_add — add or update at runtime
 * ---------------------------------------------------------------------- */

int mt_namespace_add(const char* name, int max_replicas, int cpu_quota_pct) {
    if (!name || !*name) return -1;

    pthread_mutex_lock(&g_ns_mu);

    /* Update existing */
    MtNamespace* existing = ns_find_locked(name);
    if (existing) {
        existing->max_replicas  = max_replicas;
        existing->cpu_quota_pct = cpu_quota_pct;
        existing->active        = 1;
        pthread_mutex_unlock(&g_ns_mu);
        printf("[mt] namespace '%s' updated  max_r=%d cpu=%d%%\n",
               name, max_replicas, cpu_quota_pct);
        return 0;
    }

    if (g_ns_count >= MT_MAX_NAMESPACES) {
        pthread_mutex_unlock(&g_ns_mu);
        fprintf(stderr, "[mt] namespace table full — cannot add '%s'\n", name);
        return -1;
    }

    MtNamespace* n = &g_ns[g_ns_count++];
    memset(n, 0, sizeof(*n));
    ns_set_name(n, name);
    n->max_replicas  = max_replicas;
    n->cpu_quota_pct = cpu_quota_pct;
    n->active        = 1;
    pthread_mutex_unlock(&g_ns_mu);

    printf("[mt] namespace '%s' registered  max_r=%d cpu=%d%%\n",
           name, max_replicas, cpu_quota_pct);
    return 0;
}

/* -------------------------------------------------------------------------
 * mt_namespace_revoke
 * ---------------------------------------------------------------------- */

int mt_namespace_revoke(const char* name) {
    pthread_mutex_lock(&g_ns_mu);
    MtNamespace* n = ns_find_locked(name);
    if (!n) {
        pthread_mutex_unlock(&g_ns_mu);
        return -1;
    }
    n->active = 0;
    pthread_mutex_unlock(&g_ns_mu);
    printf("[mt] namespace '%s' revoked\n", name);
    return 0;
}

/* -------------------------------------------------------------------------
 * mt_app_namespace — extract "ns" from "ns.appname"
 * ---------------------------------------------------------------------- */

int mt_app_namespace(const char* app_name, char* ns_out, size_t ns_out_len) {
    if (!app_name || !ns_out || ns_out_len == 0) return -1;
    const char* dot = strchr(app_name, MT_SEPARATOR);
    if (!dot) return -1;   /* plain name — no namespace */
    size_t len = (size_t)(dot - app_name);
    if (len >= ns_out_len) len = ns_out_len - 1;
    memcpy(ns_out, app_name, len);
    ns_out[len] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------
 * mt_quota_check
 * ---------------------------------------------------------------------- */

MtStatus mt_quota_check(const char* ns_name) {
    if (!ns_name || !*ns_name) return MT_OK;   /* no namespace → no restriction */

    pthread_mutex_lock(&g_ns_mu);
    MtNamespace* n = ns_find_locked(ns_name);
    if (!n) {
        pthread_mutex_unlock(&g_ns_mu);
        return MT_ERR_UNKNOWN_NS;
    }
    if (!n->active) {
        pthread_mutex_unlock(&g_ns_mu);
        return MT_ERR_NS_INACTIVE;
    }
    if (n->max_replicas > 0 && n->current_replicas >= n->max_replicas) {
        pthread_mutex_unlock(&g_ns_mu);
        return MT_ERR_QUOTA_FULL;
    }
    pthread_mutex_unlock(&g_ns_mu);
    return MT_OK;
}

/* -------------------------------------------------------------------------
 * mt_replica_add / mt_replica_remove
 * ---------------------------------------------------------------------- */

void mt_replica_add(const char* ns_name) {
    if (!ns_name || !*ns_name) return;
    pthread_mutex_lock(&g_ns_mu);
    MtNamespace* n = ns_find_locked(ns_name);
    if (n) n->current_replicas++;
    pthread_mutex_unlock(&g_ns_mu);
}

void mt_replica_remove(const char* ns_name) {
    if (!ns_name || !*ns_name) return;
    pthread_mutex_lock(&g_ns_mu);
    MtNamespace* n = ns_find_locked(ns_name);
    if (n && n->current_replicas > 0) n->current_replicas--;
    pthread_mutex_unlock(&g_ns_mu);
}

/* -------------------------------------------------------------------------
 * mt_namespace_list
 * ---------------------------------------------------------------------- */

int mt_namespace_list(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;

    pthread_mutex_lock(&g_ns_mu);
    for (int i = 0; i < g_ns_count; i++) {
        MtNamespace* n = &g_ns[i];
        if (!n->active) continue;

        /* Copy to local buffers to avoid GCC inliner range confusion */
        char _name[MT_MAX_NS_NAME];
        memcpy(_name, n->name, MT_MAX_NS_NAME); _name[MT_MAX_NS_NAME-1] = '\0';

        char entry[160];
        int elen = snprintf(entry, sizeof(entry),
                            "%s:max=%d:used=%d:cpu=%d%%",
                            _name,
                            n->max_replicas,
                            n->current_replicas,
                            n->cpu_quota_pct);
        if (elen <= 0) continue;

        if (used > 0 && used + 1 < out_len) {
            out[used++] = ',';
            out[used]   = '\0';
        }
        size_t space = out_len - used - 1;
        size_t copy  = (size_t)elen < space ? (size_t)elen : space;
        memcpy(out + used, entry, copy);
        used += copy;
        out[used] = '\0';
    }
    pthread_mutex_unlock(&g_ns_mu);

    return (int)used;
}

/* -------------------------------------------------------------------------
 * mt_namespace_get
 * ---------------------------------------------------------------------- */

int mt_namespace_get(const char* name, MtNamespace* dst) {
    if (!name || !dst) return 0;
    pthread_mutex_lock(&g_ns_mu);
    MtNamespace* n = ns_find_locked(name);
    if (n) { memcpy(dst, n, sizeof(*dst)); }
    pthread_mutex_unlock(&g_ns_mu);
    return n ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * mt_status_str
 * ---------------------------------------------------------------------- */

const char* mt_status_str(MtStatus s) {
    switch (s) {
        case MT_OK:             return "OK";
        case MT_ERR_UNKNOWN_NS: return "namespace not registered";
        case MT_ERR_QUOTA_FULL: return "replica quota exhausted for namespace";
        case MT_ERR_NS_INACTIVE:return "namespace is inactive/revoked";
        default:                return "unknown error";
    }
}
