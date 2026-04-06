/*
 * skr8tr_reg.c — The Tower — Service Registry
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every workload that needs to talk to another
 * service depends on this daemon.
 *
 * Responsibilities:
 *   - Workloads register themselves on startup: REGISTER|<name>|<ip>|<port>
 *   - Other workloads resolve names: LOOKUP|<name> → OK|LOOKUP|<name>|<ip>|<port>
 *   - Entries expire after TTL_S seconds (workloads must re-register to stay live)
 *   - Multiple endpoints per name supported (replicas all register the same name)
 *   - LOOKUP returns the least-recently-used endpoint (round-robin across replicas)
 *   - Dead endpoints auto-expire — no manual deregistration required
 *
 * Wire protocol — UDP port 7772:
 *   REGISTER|<name>|<ip>|<port>          → OK|REGISTERED|<name>
 *   LOOKUP|<name>                         → OK|LOOKUP|<name>|<ip>|<port>
 *                                           ERR|NOT_FOUND|<name>
 *   DEREGISTER|<name>|<ip>|<port>        → OK|DEREGISTERED|<name>
 *   LIST                                  → OK|LIST|<n>|<name:ip:port:ttl,...>
 *   PING                                  → OK|PONG|tower
 *
 * Design:
 *   - No persistent state — registry is rebuilt from live REGISTER heartbeats
 *   - Reaper thread expires stale entries every REAP_INTERVAL_S seconds
 *   - Multiple replicas of the same app register under the same name —
 *     LOOKUP load-balances across them round-robin
 *   - Hardware/cloud agnostic — just name → {ip, port}
 *
 * SSoA LEVEL 1 — Foundation Anchor
 */

#include "../core/fabric.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define TOWER_PORT        7772
#define TTL_S             30       /* seconds before an entry expires */
#define REAP_INTERVAL_S   10       /* how often the reaper runs */
#define MAX_ENTRIES       1024     /* total endpoint slots */
#define NAME_LEN          128
#define IP_LEN            INET_ADDRSTRLEN

/* -------------------------------------------------------------------------
 * Registry entry
 * ---------------------------------------------------------------------- */

typedef struct {
    char    name[NAME_LEN];
    char    ip[IP_LEN];
    int     port;
    time_t  last_seen;   /* updated on each REGISTER */
    int     active;
    int     rr_seq;      /* round-robin sequence counter */
} RegEntry;

static RegEntry        g_entries[MAX_ENTRIES];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Registry helpers
 * ---------------------------------------------------------------------- */

/* Find existing entry by name+ip+port, or allocate a free slot.
 * Returns NULL if table is full. Must be called with g_mu held. */
static RegEntry* entry_upsert(const char* name, const char* ip, int port) {
    RegEntry* free_slot = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        RegEntry* e = &g_entries[i];
        if (e->active &&
            !strcmp(e->name, name) &&
            !strcmp(e->ip,   ip)   &&
            e->port == port)
            return e;
        if (!e->active && !free_slot)
            free_slot = e;
    }
    if (!free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", name);
    snprintf(free_slot->ip,   sizeof(free_slot->ip),   "%s", ip);
    free_slot->port   = port;
    free_slot->active = 1;
    return free_slot;
}

/* Round-robin LOOKUP: return the next live endpoint for `name`.
 * Advances rr_seq on the returned entry. Returns NULL if none found. */
static RegEntry* entry_lookup(const char* name) {
    time_t now = time(NULL);

    /* Collect live candidates */
    RegEntry* candidates[MAX_ENTRIES];
    int       n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        RegEntry* e = &g_entries[i];
        if (e->active &&
            !strcmp(e->name, name) &&
            (now - e->last_seen) < TTL_S)
            candidates[n++] = e;
    }
    if (n == 0) return NULL;

    /* Pick the one with the lowest rr_seq (least recently served) */
    RegEntry* pick = candidates[0];
    for (int i = 1; i < n; i++)
        if (candidates[i]->rr_seq < pick->rr_seq)
            pick = candidates[i];

    pick->rr_seq++;
    return pick;
}

/* Expire entries that haven't re-registered within TTL_S */
static int entry_reap(void) {
    time_t now    = time(NULL);
    int    reaped = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        RegEntry* e = &g_entries[i];
        if (e->active && (now - e->last_seen) >= TTL_S) {
            printf("[tower] expired: %s  %s:%d\n", e->name, e->ip, e->port);
            e->active = 0;
            reaped++;
        }
    }
    return reaped;
}

/* Count live entries */
static int entry_live_count(void) {
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (g_entries[i].active &&
            (now - g_entries[i].last_seen) < TTL_S)
            n++;
    return n;
}

/* -------------------------------------------------------------------------
 * Reaper thread
 * ---------------------------------------------------------------------- */

static void* reaper_thread(void* arg) {
    (void)arg;
    while (1) {
        sleep(REAP_INTERVAL_S);
        pthread_mutex_lock(&g_mu);
        int n = entry_reap();
        pthread_mutex_unlock(&g_mu);
        if (n > 0)
            printf("[tower] reaped %d expired entries\n", n);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

static void handle_register(const char* body, char* resp, size_t resp_len) {
    /* body: <name>|<ip>|<port> */
    char name[NAME_LEN] = {0};
    char ip[IP_LEN]     = {0};
    int  port           = 0;

    if (sscanf(body, "%127[^|]|%15[^|]|%d", name, ip, &port) != 3) {
        snprintf(resp, resp_len, "ERR|REGISTER syntax: name|ip|port");
        return;
    }

    pthread_mutex_lock(&g_mu);
    RegEntry* e = entry_upsert(name, ip, port);
    if (!e) {
        pthread_mutex_unlock(&g_mu);
        snprintf(resp, resp_len, "ERR|registry full");
        return;
    }
    e->last_seen = time(NULL);
    pthread_mutex_unlock(&g_mu);

    printf("[tower] registered: %s  %s:%d\n", name, ip, port);
    snprintf(resp, resp_len, "OK|REGISTERED|%.127s", name);
}

static void handle_lookup(const char* name, char* resp, size_t resp_len) {
    pthread_mutex_lock(&g_mu);
    RegEntry* e = entry_lookup(name);
    if (!e) {
        pthread_mutex_unlock(&g_mu);
        snprintf(resp, resp_len, "ERR|NOT_FOUND|%.127s", name);
        return;
    }
    char ip[IP_LEN];
    int  port;
    snprintf(ip, sizeof(ip), "%s", e->ip);
    port = e->port;
    pthread_mutex_unlock(&g_mu);

    snprintf(resp, resp_len, "OK|LOOKUP|%.127s|%s|%d", name, ip, port);
}

static void handle_deregister(const char* body,
                               char* resp, size_t resp_len) {
    char name[NAME_LEN] = {0};
    char ip[IP_LEN]     = {0};
    int  port           = 0;

    if (sscanf(body, "%127[^|]|%15[^|]|%d", name, ip, &port) != 3) {
        snprintf(resp, resp_len, "ERR|DEREGISTER syntax: name|ip|port");
        return;
    }

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        RegEntry* e = &g_entries[i];
        if (e->active &&
            !strcmp(e->name, name) &&
            !strcmp(e->ip,   ip)   &&
            e->port == port) {
            e->active = 0;
            pthread_mutex_unlock(&g_mu);
            printf("[tower] deregistered: %s  %s:%d\n", name, ip, port);
            snprintf(resp, resp_len, "OK|DEREGISTERED|%.127s", name);
            return;
        }
    }
    pthread_mutex_unlock(&g_mu);
    snprintf(resp, resp_len, "ERR|NOT_FOUND|%.127s", name);
}

static void handle_list(char* resp, size_t resp_len) {
    pthread_mutex_lock(&g_mu);
    time_t now  = time(NULL);
    int    live = entry_live_count();
    char   buf[FABRIC_MTU - 32];
    buf[0] = '\0';

    for (int i = 0; i < MAX_ENTRIES; i++) {
        RegEntry* e = &g_entries[i];
        if (!e->active || (now - e->last_seen) >= TTL_S) continue;
        long ttl_left = TTL_S - (long)(now - e->last_seen);
        char entry[200];
        snprintf(entry, sizeof(entry), "%.64s:%s:%d:%ld",
                 e->name, e->ip, e->port, ttl_left);
        if (buf[0]) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, entry, sizeof(buf) - strlen(buf) - 1);
    }
    pthread_mutex_unlock(&g_mu);
    snprintf(resp, resp_len, "OK|LIST|%d|%s", live, buf);
}

static void handle_command(const char* cmd, char* resp, size_t resp_len) {
    if (!strncmp(cmd, "REGISTER|", 9)) {
        handle_register(cmd + 9, resp, resp_len);
    } else if (!strncmp(cmd, "LOOKUP|", 7)) {
        handle_lookup(cmd + 7, resp, resp_len);
    } else if (!strncmp(cmd, "DEREGISTER|", 11)) {
        handle_deregister(cmd + 11, resp, resp_len);
    } else if (!strcmp(cmd, "LIST")) {
        handle_list(resp, resp_len);
    } else if (!strcmp(cmd, "PING")) {
        snprintf(resp, resp_len, "OK|PONG|tower");
    } else {
        snprintf(resp, resp_len, "ERR|unknown command");
    }
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    printf("[tower] Skr8tr Tower (service registry) starting...\n");
    printf("[tower] TTL: %ds  reap interval: %ds\n",
           TTL_S, REAP_INTERVAL_S);

    int sock = fabric_bind(TOWER_PORT);
    if (sock < 0) {
        fprintf(stderr, "[tower] FATAL: cannot bind port %d: %s\n",
                TOWER_PORT, strerror(errno));
        return 1;
    }
    printf("[tower] listening on UDP port %d\n", TOWER_PORT);

    /* Start reaper thread */
    pthread_t reap_tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&reap_tid, &attr, reaper_thread, NULL);
    pthread_attr_destroy(&attr);

    printf("[tower] ready\n");

    char      buf[FABRIC_MTU];
    char      resp[FABRIC_MTU];
    FabricAddr src;

    for (;;) {
        int n = fabric_recv(sock, buf, sizeof(buf) - 1, &src);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[tower] recv error: %s\n", strerror(errno));
            continue;
        }
        buf[n] = '\0';

        resp[0] = '\0';
        handle_command(buf, resp, sizeof(resp));

        if (resp[0])
            fabric_send(sock, src.ip, src.port, resp, strlen(resp));
    }

    return 0;
}
