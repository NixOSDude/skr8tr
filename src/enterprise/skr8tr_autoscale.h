/*
 * skr8tr_autoscale.h — Enterprise Custom Metric Autoscale — Public API
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Drives horizontal scale decisions based on arbitrary Prometheus metrics
 * scraped from the node /metrics endpoints (TCP 9100).
 *
 * The Conductor's built-in autoscaler reacts to CPU %.
 * This module extends that with:
 *
 *   - Per-app metric rules:  scale up/down based on any metric name
 *     (e.g., skr8tr_queue_depth, http_requests_per_second, gpu_util)
 *   - Cooldown enforcement: prevents flapping by enforcing a minimum
 *     interval between consecutive scale events per app
 *   - Integration: runs as a background thread inside skr8tr_sched;
 *     calls SUBMIT (to add replicas) or EVICT (to remove replicas)
 *     via the Conductor's own command handlers
 *
 * Rule config format (/etc/skr8tr/autoscale.conf):
 *   # app_name  metric_name              scale_up_above  scale_dn_below  min_r  max_r  cooldown_s
 *   myapp       skr8tr_queue_depth       100             10              1      8      60
 *   apiserver   http_requests_per_second  1000           100             2      16     30
 *
 * Wire:
 *   The autoscaler scrapes metrics from each live node's TCP 9100:
 *     GET /metrics HTTP/1.0\r\n\r\n
 *   Parses Prometheus text format to extract metric values.
 *   Computes max metric value across all replicas of the app.
 *   Issues scale commands to the local Conductor socket.
 *
 * SSoA Level: ENTERPRISE
 */

#pragma once

#include <stddef.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define AS_MAX_RULES         64
#define AS_MAX_APP_NAME      128
#define AS_MAX_METRIC_NAME   128
#define AS_CONFIG_PATH       "/etc/skr8tr/autoscale.conf"
#define AS_SCRAPE_INTERVAL_S 15     /* metrics scrape period */
#define AS_METRICS_PORT      9100   /* Prometheus /metrics port on nodes */
#define AS_CONDUCTOR_PORT    7771   /* local Conductor command port */

/* ── Scale rule ─────────────────────────────────────────────────────── */

typedef struct {
    char   app_name[AS_MAX_APP_NAME];
    char   metric_name[AS_MAX_METRIC_NAME];
    double scale_up_above;    /* add replica when metric > this */
    double scale_dn_below;    /* remove replica when metric < this */
    int    min_replicas;      /* never scale below this */
    int    max_replicas;      /* never scale above this */
    int    cooldown_s;        /* min seconds between scale events */
    long   last_event_ts;     /* unix timestamp of last scale action */
    int    active;
} AsRule;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/* Load (or reload) autoscale rules from path.
 * Returns count of rules loaded, -1 on I/O error. */
int  as_load_config(const char* path);

/* Add or update a rule at runtime.
 * Returns 0 on success, -1 if table is full. */
int  as_rule_set(const char* app_name, const char* metric_name,
                 double up_above, double dn_below,
                 int min_r, int max_r, int cooldown_s);

/* Remove rules for an app. */
void as_rule_remove(const char* app_name);

/* ── Background autoscale thread ────────────────────────────────────── */

/* Entry point for pthread_create.  Runs forever; send SIGTERM to stop.
 * arg: const char* conductor_host (or NULL for "127.0.0.1") */
void* as_thread(void* arg);

/* ── Prometheus metric scraper ──────────────────────────────────────── */

/* Scrape a single node's /metrics endpoint.
 * Finds metric_name in Prometheus text format and returns its value.
 * Returns 0 on success (value written to *out), -1 if not found or error. */
int  as_scrape_metric(const char* node_ip, int port,
                      const char* metric_name, double* out);

/* ── Rule introspection ─────────────────────────────────────────────── */

/* Serialize all rules as a human-readable string.
 * Returns bytes written. */
int  as_rule_list(char* out, size_t out_len);
