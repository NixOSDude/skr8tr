/*
 * skr8tr_autoscale.c — Enterprise Custom Metric Autoscale
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Background autoscale thread that scrapes Prometheus /metrics from all
 * live nodes and drives horizontal scale decisions based on configured rules.
 *
 * Scale algorithm (per app, per scrape interval):
 *
 *   1. Find all live nodes hosting at least one replica of the app
 *   2. Scrape metric_name from each node's /metrics endpoint
 *   3. Compute max value across all scraped replicas
 *   4. If max > scale_up_above AND current_replicas < max_replicas
 *      AND cooldown elapsed → issue SUBMIT to add one replica
 *   5. If max < scale_dn_below AND current_replicas > min_replicas
 *      AND cooldown elapsed → issue EVICT of one replica (lowest-loaded node)
 *
 * Integration into skr8tr_sched.c (guarded with #ifdef ENTERPRISE):
 *
 *   On startup, after mt_load_config():
 *     as_load_config(NULL);
 *     pthread_t as_tid;
 *     pthread_create(&as_tid, NULL, as_thread, NULL);
 *
 * Build: compiled as part of skr8tr_sched when ENTERPRISE=1
 *
 * SSoA Level: ENTERPRISE
 */

#include "skr8tr_autoscale.h"
#include "../core/fabric.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Rule table — in-memory, mutex-protected
 * ---------------------------------------------------------------------- */

static AsRule          g_rules[AS_MAX_RULES];
static int             g_rule_count = 0;
static pthread_mutex_t g_rules_mu   = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Config loader
 * ---------------------------------------------------------------------- */

int as_load_config(const char* path) {
    if (!path) path = AS_CONFIG_PATH;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            printf("[autoscale] config not found — no rules registered: %s\n",
                   path);
            return 0;
        }
        fprintf(stderr, "[autoscale] cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    int count = 0;
    char line[512];

    pthread_mutex_lock(&g_rules_mu);

    while (fgets(line, sizeof(line), fp) && count < AS_MAX_RULES) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;

        AsRule r;
        memset(&r, 0, sizeof(r));

        /* Format: app_name metric_name up_above dn_below min_r max_r cooldown_s */
        char app[AS_MAX_APP_NAME]={0}, metric[AS_MAX_METRIC_NAME]={0};
        double up_above=0, dn_below=0;
        int min_r=1, max_r=8, cool=60;

        int parsed = sscanf(s,
            "%127s %127s %lf %lf %d %d %d",
            app, metric, &up_above, &dn_below, &min_r, &max_r, &cool);
        if (parsed < 4) continue;

        size_t al = strlen(app);    if (al >= AS_MAX_APP_NAME)    al = AS_MAX_APP_NAME-1;
        size_t ml = strlen(metric); if (ml >= AS_MAX_METRIC_NAME) ml = AS_MAX_METRIC_NAME-1;
        memcpy(r.app_name,    app,    al);
        memcpy(r.metric_name, metric, ml);
        r.scale_up_above = up_above;
        r.scale_dn_below = dn_below;
        r.min_replicas   = min_r > 0 ? min_r : 1;
        r.max_replicas   = max_r > 0 ? max_r : 8;
        r.cooldown_s     = cool > 0  ? cool  : 60;
        r.active         = 1;

        /* Preserve last_event_ts for existing rules */
        for (int i = 0; i < g_rule_count; i++) {
            if (g_rules[i].active &&
                !strcmp(g_rules[i].app_name, r.app_name) &&
                !strcmp(g_rules[i].metric_name, r.metric_name)) {
                r.last_event_ts = g_rules[i].last_event_ts;
                break;
            }
        }

        g_rules[count++] = r;
        printf("[autoscale] rule: %s  metric=%s  up>%.1f dn<%.1f  "
               "min=%d max=%d cool=%ds\n",
               r.app_name, r.metric_name,
               r.scale_up_above, r.scale_dn_below,
               r.min_replicas, r.max_replicas, r.cooldown_s);
    }
    g_rule_count = count;
    pthread_mutex_unlock(&g_rules_mu);

    fclose(fp);
    printf("[autoscale] %d rule(s) loaded from '%s'\n", count, path);
    return count;
}

/* -------------------------------------------------------------------------
 * as_rule_set
 * ---------------------------------------------------------------------- */

int as_rule_set(const char* app_name, const char* metric_name,
                double up_above, double dn_below,
                int min_r, int max_r, int cooldown_s) {
    if (!app_name || !metric_name) return -1;

    pthread_mutex_lock(&g_rules_mu);

    /* Update existing */
    for (int i = 0; i < g_rule_count; i++) {
        if (g_rules[i].active &&
            !strcmp(g_rules[i].app_name,    app_name) &&
            !strcmp(g_rules[i].metric_name, metric_name)) {
            g_rules[i].scale_up_above = up_above;
            g_rules[i].scale_dn_below = dn_below;
            g_rules[i].min_replicas   = min_r;
            g_rules[i].max_replicas   = max_r;
            g_rules[i].cooldown_s     = cooldown_s;
            pthread_mutex_unlock(&g_rules_mu);
            return 0;
        }
    }

    if (g_rule_count >= AS_MAX_RULES) {
        pthread_mutex_unlock(&g_rules_mu);
        return -1;
    }

    AsRule* r = &g_rules[g_rule_count++];
    memset(r, 0, sizeof(*r));
    {
        size_t al = strlen(app_name);    if (al >= AS_MAX_APP_NAME)    al = AS_MAX_APP_NAME-1;
        size_t ml = strlen(metric_name); if (ml >= AS_MAX_METRIC_NAME) ml = AS_MAX_METRIC_NAME-1;
        memcpy(r->app_name,    app_name,    al);
        memcpy(r->metric_name, metric_name, ml);
    }
    r->scale_up_above = up_above;
    r->scale_dn_below = dn_below;
    r->min_replicas   = min_r;
    r->max_replicas   = max_r;
    r->cooldown_s     = cooldown_s;
    r->active         = 1;

    pthread_mutex_unlock(&g_rules_mu);
    return 0;
}

/* -------------------------------------------------------------------------
 * as_rule_remove
 * ---------------------------------------------------------------------- */

void as_rule_remove(const char* app_name) {
    if (!app_name) return;
    pthread_mutex_lock(&g_rules_mu);
    for (int i = 0; i < g_rule_count; i++)
        if (!strcmp(g_rules[i].app_name, app_name))
            g_rules[i].active = 0;
    pthread_mutex_unlock(&g_rules_mu);
}

/* -------------------------------------------------------------------------
 * as_scrape_metric — HTTP GET /metrics from node, parse Prometheus text
 * ---------------------------------------------------------------------- */

int as_scrape_metric(const char* node_ip, int port,
                     const char* metric_name, double* out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec=3, .tv_usec=0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, node_ip, &sa.sin_addr) != 1) {
        close(fd); return -1;
    }

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }

    const char* req = "GET /metrics HTTP/1.0\r\nHost: localhost\r\n\r\n";
    ssize_t _w = write(fd, req, strlen(req)); (void)_w;

    /* Read response — up to 64KB */
    char buf[65536];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof(buf) - 1) break;
    }
    buf[total] = '\0';
    close(fd);

    /* Strip HTTP headers */
    const char* body = strstr(buf, "\r\n\r\n");
    if (body) body += 4; else body = buf;

    /* Scan for metric_name line:
     * Format: <metric_name>{...} <value>
     * or:     <metric_name> <value>
     * We scan line by line. */
    size_t mname_len = strlen(metric_name);
    const char* line = body;
    while (line && *line) {
        /* Skip comment lines */
        if (*line == '#') {
            line = strchr(line, '\n');
            if (line) line++;
            continue;
        }
        /* Check if line starts with metric_name */
        if (!strncmp(line, metric_name, mname_len)) {
            char next = line[mname_len];
            /* Must be followed by '{', ' ', '\t', or end */
            if (next == '{' || next == ' ' || next == '\t') {
                /* Find value after optional labels */
                const char* vp = line + mname_len;
                if (*vp == '{') {
                    /* Skip labels */
                    vp = strchr(vp, '}');
                    if (!vp) goto next_line;
                    vp++;
                }
                while (*vp == ' ' || *vp == '\t') vp++;
                char* end;
                double val = strtod(vp, &end);
                if (end != vp) {
                    *out = val;
                    return 0;
                }
            }
        }
next_line:
        line = strchr(line, '\n');
        if (line) line++;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Conductor command sender — ephemeral UDP round-trip
 * ---------------------------------------------------------------------- */

static void conductor_cmd(const char* host, int port,
                          const char* cmd,
                          char* resp, size_t resp_len) {
    int sock = fabric_bind(0);
    if (sock < 0) return;

    fabric_send(sock, host, port, cmd, strlen(cmd));

    FabricAddr src;
    struct timeval tv = { .tv_sec=5, .tv_usec=0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = fabric_recv(sock, resp, resp_len - 1, &src);
    close(sock);
    if (n > 0) resp[n] = '\0';
}

/* -------------------------------------------------------------------------
 * as_thread — background scrape and scale loop
 * ---------------------------------------------------------------------- */

void* as_thread(void* arg) {
    const char* conductor_host = arg ? (const char*)arg : "127.0.0.1";

    printf("[autoscale] thread started — conductor=%s:%d  "
           "scrape_interval=%ds\n",
           conductor_host, AS_CONDUCTOR_PORT, AS_SCRAPE_INTERVAL_S);

    for (;;) {
        sleep((unsigned)AS_SCRAPE_INTERVAL_S);

        pthread_mutex_lock(&g_rules_mu);
        /* Snapshot rules to avoid holding lock during network I/O */
        AsRule rules[AS_MAX_RULES];
        int    rule_count = g_rule_count;
        memcpy(rules, g_rules, (size_t)rule_count * sizeof(AsRule));
        pthread_mutex_unlock(&g_rules_mu);

        time_t now = time(NULL);

        for (int ri = 0; ri < rule_count; ri++) {
            AsRule* r = &rules[ri];
            if (!r->active) continue;

            /* Query Conductor for list of nodes hosting this app */
            char list_resp[4096];
            char list_cmd[256];
            snprintf(list_cmd, sizeof(list_cmd), "LIST");
            conductor_cmd(conductor_host, AS_CONDUCTOR_PORT,
                          list_cmd, list_resp, sizeof(list_resp));

            /* Parse LIST response: OK|LIST|N|app:node_id:pid,... */
            /* We need to know: (a) current replica count, (b) node IPs */
            const char* lp = strstr(list_resp, "|LIST|");
            if (!lp) continue;
            lp = strchr(lp + 6, '|');
            if (!lp) continue;
            lp++;   /* points to first entry */

            /* Collect node_ids of replicas for this app */
            char node_ids[16][33];
            int  ncount = 0;
            int  current_replicas = 0;

            char entry_buf[4096];
            size_t lp_len = strlen(lp);
            if (lp_len >= sizeof(entry_buf)) lp_len = sizeof(entry_buf)-1;
            memcpy(entry_buf, lp, lp_len); entry_buf[lp_len] = '\0';

            char* tok = entry_buf;
            while (tok && *tok) {
                char* comma = strchr(tok, ',');
                if (comma) *comma = '\0';

                /* entry format: app_name:node_id:pid */
                char* c1 = strchr(tok, ':');
                if (c1) {
                    size_t app_len = (size_t)(c1 - tok);
                    if (app_len == strlen(r->app_name) &&
                        !strncmp(tok, r->app_name, app_len)) {
                        current_replicas++;
                        char* c2 = strchr(c1+1, ':');
                        if (c2 && ncount < 16) {
                            size_t nl = (size_t)(c2 - (c1+1));
                            if (nl >= 33) nl = 32;
                            memcpy(node_ids[ncount], c1+1, nl);
                            node_ids[ncount][nl] = '\0';
                            ncount++;
                        }
                    }
                }
                tok = comma ? comma + 1 : NULL;
            }

            if (current_replicas == 0) continue;

            /* Query Conductor for node IPs: NODES */
            char nodes_resp[4096];
            conductor_cmd(conductor_host, AS_CONDUCTOR_PORT,
                          "NODES", nodes_resp, sizeof(nodes_resp));

            /* Build node_id → ip map */
            char nid_map[16][33];
            char nip_map[16][16];
            int  nmap_count = 0;

            const char* np = strstr(nodes_resp, "|NODES|");
            if (np) {
                np = strchr(np + 7, '|');
                if (np) {
                    np++;
                    char nbuf[4096];
                    size_t np_len = strlen(np);
                    if (np_len >= sizeof(nbuf)) np_len = sizeof(nbuf)-1;
                    memcpy(nbuf, np, np_len); nbuf[np_len] = '\0';

                    char* ntok = nbuf;
                    while (ntok && *ntok && nmap_count < 16) {
                        char* nc = strchr(ntok, ',');
                        if (nc) *nc = '\0';
                        /* format: node_id:ip:cpu:ram */
                        char* c1n = strchr(ntok, ':');
                        if (c1n) {
                            size_t idl = (size_t)(c1n - ntok);
                            if (idl >= 33) idl = 32;
                            memcpy(nid_map[nmap_count], ntok, idl);
                            nid_map[nmap_count][idl] = '\0';
                            char* c2n = strchr(c1n+1, ':');
                            size_t ipl = c2n ? (size_t)(c2n - (c1n+1)) : strlen(c1n+1);
                            if (ipl >= 16) ipl = 15;
                            memcpy(nip_map[nmap_count], c1n+1, ipl);
                            nip_map[nmap_count][ipl] = '\0';
                            nmap_count++;
                        }
                        ntok = nc ? nc + 1 : NULL;
                    }
                }
            }

            /* Scrape metric from each replica's node — take max */
            double max_val = -1.0;
            for (int ni = 0; ni < ncount; ni++) {
                /* Find IP for this node_id */
                char ip[16] = {0};
                for (int mi = 0; mi < nmap_count; mi++) {
                    if (!strcmp(nid_map[mi], node_ids[ni])) {
                        size_t il = strlen(nip_map[mi]);
                        if (il >= sizeof(ip)) il = sizeof(ip)-1;
                        memcpy(ip, nip_map[mi], il);
                        break;
                    }
                }
                if (!ip[0]) continue;

                double val;
                if (as_scrape_metric(ip, AS_METRICS_PORT,
                                     r->metric_name, &val) == 0) {
                    if (val > max_val) max_val = val;
                }
            }

            if (max_val < 0) continue;   /* metric not found on any node */

            /* Check cooldown */
            if (now - r->last_event_ts < r->cooldown_s) continue;

            char resp[512]; resp[0] = '\0';

            if (max_val > r->scale_up_above &&
                current_replicas < r->max_replicas) {
                /* Scale up: submit one additional replica */
                printf("[autoscale] SCALE UP  %s  metric=%.1f > %.1f  "
                       "replicas: %d → %d\n",
                       r->app_name, max_val, r->scale_up_above,
                       current_replicas, current_replicas + 1);

                /* We need the manifest path — ask Conductor for workload info.
                 * Workaround: we re-submit the same manifest.
                 * The Conductor is idempotent: submitting an existing workload
                 * adds a replica if below desired count. We send SUBMIT with
                 * the app name prefixed manifest; the Conductor resolves by
                 * cached manifest path. */
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "SUBMIT|%s", r->app_name);
                conductor_cmd(conductor_host, AS_CONDUCTOR_PORT,
                              cmd, resp, sizeof(resp));
                printf("[autoscale] SUBMIT response: %s\n", resp);

                /* Update last_event_ts */
                pthread_mutex_lock(&g_rules_mu);
                for (int i = 0; i < g_rule_count; i++) {
                    if (g_rules[i].active &&
                        !strcmp(g_rules[i].app_name,    r->app_name) &&
                        !strcmp(g_rules[i].metric_name, r->metric_name))
                        g_rules[i].last_event_ts = now;
                }
                pthread_mutex_unlock(&g_rules_mu);

            } else if (max_val < r->scale_dn_below &&
                       current_replicas > r->min_replicas) {
                /* Scale down: evict one replica
                 * We send EVICT|app_name — Conductor removes one replica */
                printf("[autoscale] SCALE DOWN %s  metric=%.1f < %.1f  "
                       "replicas: %d → %d\n",
                       r->app_name, max_val, r->scale_dn_below,
                       current_replicas, current_replicas - 1);

                char cmd[256];
                /* EVICT_ONE is a scale-down-by-one signal vs full EVICT */
                snprintf(cmd, sizeof(cmd), "EVICT_ONE|%s", r->app_name);
                conductor_cmd(conductor_host, AS_CONDUCTOR_PORT,
                              cmd, resp, sizeof(resp));
                printf("[autoscale] EVICT_ONE response: %s\n", resp);

                pthread_mutex_lock(&g_rules_mu);
                for (int i = 0; i < g_rule_count; i++) {
                    if (g_rules[i].active &&
                        !strcmp(g_rules[i].app_name,    r->app_name) &&
                        !strcmp(g_rules[i].metric_name, r->metric_name))
                        g_rules[i].last_event_ts = now;
                }
                pthread_mutex_unlock(&g_rules_mu);
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * as_rule_list
 * ---------------------------------------------------------------------- */

int as_rule_list(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';
    size_t used = 0;

    pthread_mutex_lock(&g_rules_mu);
    for (int i = 0; i < g_rule_count; i++) {
        AsRule* r = &g_rules[i];
        if (!r->active) continue;

        char _app[AS_MAX_APP_NAME], _met[AS_MAX_METRIC_NAME];
        memcpy(_app, r->app_name,    AS_MAX_APP_NAME); _app[AS_MAX_APP_NAME-1]    = '\0';
        memcpy(_met, r->metric_name, AS_MAX_METRIC_NAME); _met[AS_MAX_METRIC_NAME-1] = '\0';

        char entry[320];
        int elen = snprintf(entry, sizeof(entry),
                            "%s:metric=%s:up>%.1f:dn<%.1f:min=%d:max=%d:cool=%ds",
                            _app, _met,
                            r->scale_up_above, r->scale_dn_below,
                            r->min_replicas, r->max_replicas,
                            r->cooldown_s);
        if (elen <= 0) continue;

        if (used > 0 && used + 1 < out_len) {
            out[used++] = ','; out[used] = '\0';
        }
        size_t space = out_len - used - 1;
        size_t copy  = (size_t)elen < space ? (size_t)elen : space;
        memcpy(out + used, entry, copy);
        used += copy;
        out[used] = '\0';
    }
    pthread_mutex_unlock(&g_rules_mu);
    return (int)used;
}
