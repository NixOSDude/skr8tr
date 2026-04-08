/*
 * skr8tr_rbac.c — Enterprise RBAC Gateway
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * UDP gateway (port 7773) that enforces role-based access control
 * in front of the Conductor (port 7771).
 *
 * Flow:
 *   Client sends:  RBAC|<team>|<namespace>|<cmd>|<ts>|<sig_hex>
 *   Gateway:
 *     1. Parse fields
 *     2. Look up team → pubkey in registry
 *     3. Verify ML-DSA-65 sig over "<team>|<namespace>|<cmd>|<ts>"
 *     4. Check timestamp ±RBAC_NONCE_WINDOW_S (replay protection)
 *     5. Check team's namespace matches declared namespace
 *     6. Check team's permissions allow the command
 *     7. Prepend namespace to mutating commands
 *     8. Forward to Conductor, relay response back
 *
 * Admin commands require RBAC_PERM_ADMIN:
 *   RBAC_ADMIN|<admin_team>|TEAM_ADD|<name>|<ns>|<perms_hex>|<pubkey_hex>|<ts>|<sig>
 *   RBAC_ADMIN|<admin_team>|TEAM_REVOKE|<target>|<ts>|<sig>
 *   RBAC_ADMIN|<admin_team>|TEAM_LIST|<ts>|<sig>
 *
 * Build: make ENTERPRISE=1
 * Run:   bin/skr8tr_rbac [--registry <path>] [--conductor <host>] [--port <port>]
 */

#include "skr8tr_rbac.h"
#include "../core/fabric.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <oqs/oqs.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Team registry — in-memory, protected by mutex
 * ---------------------------------------------------------------------- */

static RbacTeam        g_teams[RBAC_MAX_TEAMS];
static int             g_team_count = 0;
static pthread_mutex_t g_reg_mu     = PTHREAD_MUTEX_INITIALIZER;
static char            g_registry_path[512] = RBAC_REGISTRY_PATH;

/* -------------------------------------------------------------------------
 * Hex encoding / decoding helpers
 * ---------------------------------------------------------------------- */

static void bytes_to_hex(const uint8_t* b, size_t len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = hex[b[i] >> 4];
        out[i*2+1] = hex[b[i] & 0xf];
    }
    out[len*2] = '\0';
}

static int hex_to_bytes(const char* hex, uint8_t* out, size_t expect_len) {
    size_t hlen = strlen(hex);
    if (hlen != expect_len * 2) return -1;
    for (size_t i = 0; i < expect_len; i++) {
        unsigned int hi, lo;
        char h = hex[i*2], l = hex[i*2+1];
        hi = isdigit((unsigned char)h) ? (unsigned)(h-'0') : (unsigned)(tolower((unsigned char)h)-'a'+10);
        lo = isdigit((unsigned char)l) ? (unsigned)(l-'0') : (unsigned)(tolower((unsigned char)l)-'a'+10);
        if (hi > 15 || lo > 15) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Registry I/O — load from flat file
 * ---------------------------------------------------------------------- */

static int registry_load_locked(void) {
    FILE* fp = fopen(g_registry_path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            printf("[rbac] registry not found — starting empty: %s\n",
                   g_registry_path);
            g_team_count = 0;
            return 0;
        }
        fprintf(stderr, "[rbac] cannot open registry '%s': %s\n",
                g_registry_path, strerror(errno));
        return -1;
    }

    int count = 0;
    char line[RBAC_HEXKEY_LEN + 256];

    while (fgets(line, sizeof(line), fp) && count < RBAC_MAX_TEAMS) {
        /* strip newline */
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        /* skip comments and blank lines */
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;

        /* Format: name:namespace:perms_hex:pubkey_hex */
        char name[RBAC_MAX_NAME]={0}, ns[RBAC_MAX_NAME]={0};
        char perms_s[16]={0};
        char key_hex[RBAC_HEXKEY_LEN + 4]={0};

        /* Field 1: name */
        char* col = strchr(s, ':');
        if (!col) continue;
        size_t fl = (size_t)(col - s);
        if (fl >= RBAC_MAX_NAME) continue;
        memcpy(name, s, fl); name[fl] = '\0';
        s = col + 1;

        /* Field 2: namespace */
        col = strchr(s, ':');
        if (!col) continue;
        fl = (size_t)(col - s);
        if (fl >= RBAC_MAX_NAME) continue;
        memcpy(ns, s, fl); ns[fl] = '\0';
        s = col + 1;

        /* Field 3: perms hex (e.g. "ff") */
        col = strchr(s, ':');
        if (!col) continue;
        fl = (size_t)(col - s);
        if (fl >= sizeof(perms_s)) continue;
        memcpy(perms_s, s, fl); perms_s[fl] = '\0';
        s = col + 1;

        /* Field 4: pubkey hex */
        strncpy(key_hex, s, sizeof(key_hex) - 1);

        RbacTeam* t = &g_teams[count];
        memset(t, 0, sizeof(*t));
        /* name[] and ns[] are already null-terminated from the parse above;
         * use memcpy to avoid -Wstringop-truncation from strncpy */
        memset(t->name,       0, RBAC_MAX_NAME);
        memset(t->namespace_, 0, RBAC_MAX_NAME);
        {
            size_t nl = strlen(name); if (nl >= RBAC_MAX_NAME) nl = RBAC_MAX_NAME - 1;
            size_t sl = strlen(ns);   if (sl >= RBAC_MAX_NAME) sl = RBAC_MAX_NAME - 1;
            memcpy(t->name,       name, nl);
            memcpy(t->namespace_, ns,   sl);
        }
        t->permissions = (uint8_t)strtol(perms_s, NULL, 16);
        t->active = 1;

        if (hex_to_bytes(key_hex, t->pubkey, RBAC_PK_LEN) < 0) {
            fprintf(stderr, "[rbac] bad pubkey hex for team '%s' — skipping\n", name);
            continue;
        }

        count++;
        printf("[rbac] loaded team: %s  ns=%s  perms=0x%02x\n",
               name, ns, t->permissions);
    }

    fclose(fp);
    g_team_count = count;
    printf("[rbac] registry loaded: %d team(s)\n", count);
    return 0;
}

static int registry_save_locked(void) {
    /* Write atomically via temp file */
    char tmp[520];   /* 512 (registry path max) + 8 for ".tmp\0" */
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_registry_path);

    /* Ensure directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", g_registry_path);
    char* slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0750); }

    FILE* fp = fopen(tmp, "w");
    if (!fp) {
        fprintf(stderr, "[rbac] cannot write registry '%s': %s\n",
                tmp, strerror(errno));
        return -1;
    }

    fprintf(fp, "# Skr8tr RBAC Team Registry\n");
    fprintf(fp, "# Format: name:namespace:perms_hex:pubkey_hex\n");

    char key_hex[RBAC_HEXKEY_LEN + 4];
    for (int i = 0; i < g_team_count; i++) {
        RbacTeam* t = &g_teams[i];
        if (!t->active) continue;
        bytes_to_hex(t->pubkey, RBAC_PK_LEN, key_hex);
        fprintf(fp, "%s:%s:%02x:%s\n",
                t->name, t->namespace_, t->permissions, key_hex);
    }
    fclose(fp);
    rename(tmp, g_registry_path);
    return 0;
}

static RbacTeam* team_find_locked(const char* name) {
    for (int i = 0; i < g_team_count; i++)
        if (g_teams[i].active && !strcmp(g_teams[i].name, name))
            return &g_teams[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * ML-DSA-65 signature verification — against raw pubkey bytes
 * ---------------------------------------------------------------------- */

/* Verifies sig_hex over payload "<team>|<namespace>|<cmd>|<ts>"
 * using raw pubkey bytes from the registry.
 * Returns 0 on success, -1 on failure. */
static int rbac_verify_sig(const uint8_t* pubkey,
                            const char*   payload,
                            const char*   sig_hex) {
    if (strlen(sig_hex) != RBAC_HEXSIG_LEN) return -1;

    uint8_t sig[RBAC_SIG_LEN];
    if (hex_to_bytes(sig_hex, sig, RBAC_SIG_LEN) < 0) return -1;

    OQS_SIG* oqs = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!oqs) return -1;

    OQS_STATUS rc = OQS_SIG_verify(
        oqs,
        (const uint8_t*)payload, strlen(payload),
        sig, RBAC_SIG_LEN,
        pubkey
    );

    OQS_SIG_free(oqs);
    return (rc == OQS_SUCCESS) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Command → permission mapping
 * ---------------------------------------------------------------------- */

static uint8_t cmd_required_perm(const char* cmd) {
    if (!strncmp(cmd, "SUBMIT",  6)) return RBAC_PERM_SUBMIT;
    if (!strncmp(cmd, "EVICT",   5)) return RBAC_PERM_EVICT;
    if (!strncmp(cmd, "ROLLOUT", 7)) return RBAC_PERM_ROLLOUT;
    if (!strncmp(cmd, "EXEC",    4)) return RBAC_PERM_EXEC;
    /* READ-only commands */
    if (!strncmp(cmd, "NODES",   5)) return RBAC_PERM_READ;
    if (!strncmp(cmd, "LIST",    4)) return RBAC_PERM_READ;
    if (!strncmp(cmd, "PING",    4)) return RBAC_PERM_READ;
    if (!strncmp(cmd, "LOOKUP",  6)) return RBAC_PERM_READ;
    if (!strncmp(cmd, "LOGS",    4)) return RBAC_PERM_READ;
    if (!strncmp(cmd, "AUDIT",   5)) return RBAC_PERM_READ;
    /* Namespace-scoped read */
    if (!strncmp(cmd, "STATUS",  6)) return RBAC_PERM_READ;
    return RBAC_PERM_ADMIN;   /* unknown → require admin */
}

/* Mutating commands that get namespace-prefixed before forwarding */
static int cmd_needs_namespace(const char* cmd) {
    return (!strncmp(cmd, "SUBMIT",  6) ||
            !strncmp(cmd, "EVICT",   5) ||
            !strncmp(cmd, "ROLLOUT", 7) ||
            !strncmp(cmd, "EXEC",    4) ||
            !strncmp(cmd, "LOGS",    4));
}

/* -------------------------------------------------------------------------
 * Conductor round-trip — forward cmd, return response
 * ---------------------------------------------------------------------- */

static int conductor_send(const char* conductor_host, int conductor_port,
                           const char* cmd, char* resp, size_t resp_len) {
    int sock = fabric_bind(0);   /* ephemeral port */
    if (sock < 0) return -1;

    fabric_send(sock, conductor_host, conductor_port, cmd, strlen(cmd));

    FabricAddr src;
    struct timeval tv = { .tv_sec=5, .tv_usec=0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = fabric_recv(sock, resp, resp_len - 1, &src);
    close(sock);
    if (n <= 0) return -1;
    resp[n] = '\0';
    return n;
}

/* -------------------------------------------------------------------------
 * Handle RBAC_ADMIN commands
 * ---------------------------------------------------------------------- */

static void handle_admin(const char* msg, char* resp, size_t resp_len) {
    /* Format: RBAC_ADMIN|<admin_team>|<admin_cmd>|<args...>|<ts>|<sig> */
    /* Parse admin_team */
    const char* p = msg + strlen("RBAC_ADMIN|");
    const char* sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|RBAC_ADMIN: malformed"); return; }

    char admin_team[RBAC_MAX_NAME] = {0};
    size_t tl = (size_t)(sep - p);
    if (tl >= RBAC_MAX_NAME) { snprintf(resp, resp_len, "ERR|team name too long"); return; }
    memcpy(admin_team, p, tl);
    p = sep + 1;

    /* Parse admin_cmd */
    sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|RBAC_ADMIN: missing cmd"); return; }
    char admin_cmd[64] = {0};
    size_t cl = (size_t)(sep - p);
    if (cl >= sizeof(admin_cmd)) { snprintf(resp, resp_len, "ERR|cmd too long"); return; }
    memcpy(admin_cmd, p, cl);
    p = sep + 1;

    /* The last two fields are always <ts>|<sig> — find them from the end */
    /* signature is exactly RBAC_HEXSIG_LEN chars, preceded by | */
    size_t mlen = strlen(p);
    if (mlen < RBAC_HEXSIG_LEN + 2) {
        snprintf(resp, resp_len, "ERR|RBAC_ADMIN: signature missing");
        return;
    }
    const char* sig_hex = p + mlen - RBAC_HEXSIG_LEN;
    if (*(sig_hex - 1) != '|') {
        snprintf(resp, resp_len, "ERR|RBAC_ADMIN: malformed signature boundary");
        return;
    }

    /* Everything before |ts|sig is the args section; ts is before sig */
    /* p now points to: args...|ts|sig_hex */
    /* We need the full payload for verification: admin_team|admin_cmd|args...|ts */
    char payload[2048];
    size_t payload_len = (size_t)(sig_hex - 1 - (msg)); /* up to the | before sig */
    /* The signed payload is: admin_team|admin_cmd|args...|ts
     * which is the original msg from "RBAC_ADMIN|" stripped to just: admin_team|... */
    const char* payload_start = msg + strlen("RBAC_ADMIN|");
    size_t pl = (size_t)((sig_hex - 1) - payload_start);
    if (pl >= sizeof(payload)) pl = sizeof(payload) - 1;
    memcpy(payload, payload_start, pl);
    payload[pl] = '\0';

    /* Look up admin team and verify signature */
    pthread_mutex_lock(&g_reg_mu);
    RbacTeam* admin = team_find_locked(admin_team);
    if (!admin) {
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "ERR|RBAC_ADMIN: unknown team: %.63s", admin_team);
        return;
    }
    if (!(admin->permissions & RBAC_PERM_ADMIN)) {
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "ERR|RBAC_ADMIN: team %.63s lacks ADMIN permission", admin_team);
        return;
    }
    uint8_t admin_pk[RBAC_PK_LEN];
    memcpy(admin_pk, admin->pubkey, RBAC_PK_LEN);
    pthread_mutex_unlock(&g_reg_mu);

    if (rbac_verify_sig(admin_pk, payload, sig_hex) < 0) {
        snprintf(resp, resp_len, "ERR|RBAC_ADMIN: invalid signature");
        return;
    }

    /* Dispatch admin_cmd */
    if (!strcmp(admin_cmd, "TEAM_LIST")) {
        pthread_mutex_lock(&g_reg_mu);
        char list[4096] = {0};
        for (int i = 0; i < g_team_count; i++) {
            if (!g_teams[i].active) continue;
            /* Copy to local bounded arrays to prevent GCC inliner from
             * confusing field ranges with adjacent pubkey[] bytes */
            char _n[RBAC_MAX_NAME], _ns[RBAC_MAX_NAME];
            memcpy(_n,  g_teams[i].name,       RBAC_MAX_NAME); _n[RBAC_MAX_NAME-1]  = '\0';
            memcpy(_ns, g_teams[i].namespace_, RBAC_MAX_NAME); _ns[RBAC_MAX_NAME-1] = '\0';
            char entry[160];   /* 64 + 64 + "ns=:perms=0xff" + NUL = safe */
            snprintf(entry, sizeof(entry), "%s:ns=%s:perms=0x%02x",
                     _n, _ns, g_teams[i].permissions);
            if (list[0]) strncat(list, ",", sizeof(list)-strlen(list)-1);
            strncat(list, entry, sizeof(list)-strlen(list)-1);
        }
        int cnt = g_team_count;
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "OK|TEAM_LIST|%d|%s", cnt, list);
        return;
    }

    if (!strcmp(admin_cmd, "TEAM_REVOKE")) {
        /* args: target_team (already consumed in p before ts) */
        char target[RBAC_MAX_NAME] = {0};
        /* p points to: target_team|ts|sig — extract target up to first | */
        const char* tsep = strchr(p, '|');
        size_t tlen = tsep ? (size_t)(tsep - p)
                           : strlen(p) - RBAC_HEXSIG_LEN - 1;
        if (tlen >= RBAC_MAX_NAME) tlen = RBAC_MAX_NAME - 1;
        memcpy(target, p, tlen);

        pthread_mutex_lock(&g_reg_mu);
        RbacTeam* t = team_find_locked(target);
        if (!t) {
            pthread_mutex_unlock(&g_reg_mu);
            snprintf(resp, resp_len, "ERR|TEAM_REVOKE: not found: %.63s", target);
            return;
        }
        t->active = 0;
        registry_save_locked();
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "OK|TEAM_REVOKED|%.63s", target);
        return;
    }

    if (!strcmp(admin_cmd, "TEAM_ADD")) {
        /* args: name|namespace|perms_hex|pubkey_hex|ts|sig */
        char new_name[RBAC_MAX_NAME]={0}, new_ns[RBAC_MAX_NAME]={0};
        char new_perms_s[16]={0}, new_key_hex[RBAC_HEXKEY_LEN+4]={0};

        /* Parse 4 pipe-delimited fields from p */
        const char* fp2 = p;

        #define NEXT_FIELD(dst, dlen) do { \
            const char* _sep2 = strchr(fp2, '|'); \
            if (!_sep2) goto bad_team_add; \
            size_t _fl = (size_t)(_sep2 - fp2); \
            if (_fl >= (dlen)) _fl = (dlen)-1; \
            memcpy((dst), fp2, _fl); (dst)[_fl] = '\0'; \
            fp2 = _sep2 + 1; \
        } while(0)

        NEXT_FIELD(new_name,    RBAC_MAX_NAME);
        NEXT_FIELD(new_ns,      RBAC_MAX_NAME);
        NEXT_FIELD(new_perms_s, sizeof(new_perms_s));
        /* pubkey_hex: everything up to |ts|sig */
        {
            /* fp2 now points to pubkey_hex|ts|sig_hex
             * pubkey_hex length = RBAC_HEXKEY_LEN, then |ts|sig_hex */
            if (strlen(fp2) < RBAC_HEXKEY_LEN + 2) goto bad_team_add;
            memcpy(new_key_hex, fp2, RBAC_HEXKEY_LEN);
            new_key_hex[RBAC_HEXKEY_LEN] = '\0';
        }
        #undef NEXT_FIELD

        RbacTeam new_t;
        memset(&new_t, 0, sizeof(new_t));
        memset(new_t.name,       0, RBAC_MAX_NAME);
        memset(new_t.namespace_, 0, RBAC_MAX_NAME);
        {
            size_t nl = strlen(new_name); if (nl >= RBAC_MAX_NAME) nl = RBAC_MAX_NAME - 1;
            size_t sl = strlen(new_ns);   if (sl >= RBAC_MAX_NAME) sl = RBAC_MAX_NAME - 1;
            memcpy(new_t.name,       new_name, nl);
            memcpy(new_t.namespace_, new_ns,   sl);
        }
        new_t.permissions = (uint8_t)strtol(new_perms_s, NULL, 16);
        new_t.active = 1;
        if (hex_to_bytes(new_key_hex, new_t.pubkey, RBAC_PK_LEN) < 0) {
            snprintf(resp, resp_len, "ERR|TEAM_ADD: invalid pubkey hex");
            return;
        }

        pthread_mutex_lock(&g_reg_mu);
        if (g_team_count >= RBAC_MAX_TEAMS) {
            pthread_mutex_unlock(&g_reg_mu);
            snprintf(resp, resp_len, "ERR|TEAM_ADD: registry full");
            return;
        }
        /* Replace existing (revoked) or append */
        RbacTeam* existing = team_find_locked(new_name);
        if (existing) {
            *existing = new_t;
        } else {
            g_teams[g_team_count++] = new_t;
        }
        registry_save_locked();
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "OK|TEAM_ADDED|%.63s|ns=%.63s|perms=0x%02x",
                 new_name, new_ns, new_t.permissions);
        return;

bad_team_add:
        snprintf(resp, resp_len, "ERR|TEAM_ADD: malformed arguments");
        return;
    }

    snprintf(resp, resp_len, "ERR|RBAC_ADMIN: unknown admin cmd: %.63s", admin_cmd);
    (void)payload_len;
}

/* -------------------------------------------------------------------------
 * Handle standard RBAC command
 * ---------------------------------------------------------------------- */

static void handle_rbac(const char* msg, char* resp, size_t resp_len,
                         const char* conductor_host, int conductor_port) {
    /* Format: RBAC|<team>|<namespace>|<cmd>|<ts>|<sig_hex> */
    const char* p = msg + strlen("RBAC|");

    /* Parse team */
    const char* sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|malformed: missing team"); return; }
    char team[RBAC_MAX_NAME] = {0};
    size_t fl = (size_t)(sep - p);
    if (fl >= RBAC_MAX_NAME) { snprintf(resp, resp_len, "ERR|team name too long"); return; }
    memcpy(team, p, fl); p = sep + 1;

    /* Parse namespace */
    sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|malformed: missing namespace"); return; }
    char ns[RBAC_MAX_NAME] = {0};
    fl = (size_t)(sep - p);
    if (fl >= RBAC_MAX_NAME) { snprintf(resp, resp_len, "ERR|namespace too long"); return; }
    memcpy(ns, p, fl); p = sep + 1;

    /* Parse command */
    sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|malformed: missing cmd"); return; }
    char cmd[512] = {0};
    fl = (size_t)(sep - p);
    if (fl >= sizeof(cmd)) fl = sizeof(cmd) - 1;
    memcpy(cmd, p, fl); p = sep + 1;

    /* Parse timestamp */
    sep = strchr(p, '|');
    if (!sep) { snprintf(resp, resp_len, "ERR|malformed: missing ts"); return; }
    char ts_s[32] = {0};
    fl = (size_t)(sep - p);
    if (fl >= sizeof(ts_s)) fl = sizeof(ts_s) - 1;
    memcpy(ts_s, p, fl); p = sep + 1;

    /* Remainder is the signature */
    const char* sig_hex = p;
    if (strlen(sig_hex) != RBAC_HEXSIG_LEN) {
        snprintf(resp, resp_len, "ERR|malformed: bad signature length (%zu != %d)",
                 strlen(sig_hex), RBAC_HEXSIG_LEN);
        return;
    }

    /* Replay / timestamp check */
    time_t ts  = (time_t)strtoll(ts_s, NULL, 10);
    time_t now = time(NULL);
    if (ts < now - RBAC_NONCE_WINDOW_S || ts > now + RBAC_NONCE_WINDOW_S) {
        snprintf(resp, resp_len, "ERR|REPLAY: timestamp outside ±%ds window",
                 RBAC_NONCE_WINDOW_S);
        return;
    }

    /* Look up team */
    pthread_mutex_lock(&g_reg_mu);
    RbacTeam* t = team_find_locked(team);
    if (!t) {
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "ERR|UNAUTHORIZED: unknown team: %.63s", team);
        return;
    }
    /* Namespace check */
    if (strcmp(t->namespace_, ns) && strcmp(t->namespace_, "*")) {
        pthread_mutex_unlock(&g_reg_mu);
        snprintf(resp, resp_len, "ERR|UNAUTHORIZED: team %.63s not in namespace %.63s",
                 team, ns);
        return;
    }
    uint8_t perm = t->permissions;
    uint8_t pk[RBAC_PK_LEN];
    memcpy(pk, t->pubkey, RBAC_PK_LEN);
    pthread_mutex_unlock(&g_reg_mu);

    /* Permission check */
    uint8_t required = cmd_required_perm(cmd);
    if (!(perm & required)) {
        snprintf(resp, resp_len,
                 "ERR|UNAUTHORIZED: team %.63s lacks permission 0x%02x for %.63s",
                 team, required, cmd);
        return;
    }

    /* Signature verification */
    char payload[1024];
    snprintf(payload, sizeof(payload), "%s|%s|%s|%s", team, ns, cmd, ts_s);
    if (rbac_verify_sig(pk, payload, sig_hex) < 0) {
        snprintf(resp, resp_len, "ERR|UNAUTHORIZED: invalid ML-DSA-65 signature");
        return;
    }

    /* Build forwarded command — namespace-prefix mutating commands */
    char fwd_cmd[512];
    if (cmd_needs_namespace(cmd)) {
        /* Prefix app name with namespace: SUBMIT|app → SUBMIT|ns.app */
        const char* cmd_sep = strchr(cmd, '|');
        if (cmd_sep) {
            char verb[64] = {0};
            size_t vl = (size_t)(cmd_sep - cmd);
            if (vl >= sizeof(verb)) vl = sizeof(verb) - 1;
            memcpy(verb, cmd, vl);
            snprintf(fwd_cmd, sizeof(fwd_cmd), "%s|%s.%s",
                     verb, ns, cmd_sep + 1);
        } else {
            snprintf(fwd_cmd, sizeof(fwd_cmd), "%s", cmd);
        }
    } else {
        snprintf(fwd_cmd, sizeof(fwd_cmd), "%s", cmd);
    }

    /* Forward to Conductor */
    char cond_resp[FABRIC_MTU];
    if (conductor_send(conductor_host, conductor_port,
                       fwd_cmd, cond_resp, sizeof(cond_resp)) < 0) {
        snprintf(resp, resp_len, "ERR|CONDUCTOR: no response");
        return;
    }

    /* Relay response */
    snprintf(resp, resp_len, "%s", cond_resp);
}

/* -------------------------------------------------------------------------
 * SIGHUP handler — reload registry
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_reload = 0;
static void sighup_handler(int s) { (void)s; g_reload = 1; }

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  sighup_handler);

    RbacConfig cfg = {
        .conductor_host = "127.0.0.1",
        .conductor_port = 7771,
        .gateway_port   = RBAC_GATEWAY_PORT,
    };
    {
        size_t rpl = strlen(RBAC_REGISTRY_PATH);
        if (rpl >= sizeof(cfg.registry_path)) rpl = sizeof(cfg.registry_path) - 1;
        memcpy(cfg.registry_path, RBAC_REGISTRY_PATH, rpl);
        cfg.registry_path[rpl] = '\0';
    }

    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--registry"))
            strncpy(g_registry_path, argv[i+1], sizeof(g_registry_path)-1);
        if (!strcmp(argv[i], "--conductor"))
            strncpy(cfg.conductor_host, argv[i+1], sizeof(cfg.conductor_host)-1);
        if (!strcmp(argv[i], "--port"))
            cfg.gateway_port = (int)strtol(argv[i+1], NULL, 10);
    }
    {
        size_t rpl = strlen(g_registry_path);
        if (rpl >= sizeof(cfg.registry_path)) rpl = sizeof(cfg.registry_path) - 1;
        memcpy(cfg.registry_path, g_registry_path, rpl);
        cfg.registry_path[rpl] = '\0';
    }

    printf("[rbac] Skr8tr RBAC Gateway starting...\n");
    printf("[rbac] registry=%s  conductor=%s:%d  gateway_port=%d\n",
           g_registry_path, cfg.conductor_host, cfg.conductor_port, cfg.gateway_port);

    pthread_mutex_lock(&g_reg_mu);
    registry_load_locked();
    pthread_mutex_unlock(&g_reg_mu);

    int sock = fabric_bind(cfg.gateway_port);
    if (sock < 0) {
        fprintf(stderr, "[rbac] FATAL: cannot bind port %d: %s\n",
                cfg.gateway_port, strerror(errno));
        return 1;
    }
    printf("[rbac] listening on UDP %d\n", cfg.gateway_port);

    char buf[FABRIC_MTU], resp[FABRIC_MTU];
    FabricAddr src;

    for (;;) {
        if (g_reload) {
            g_reload = 0;
            printf("[rbac] SIGHUP — reloading registry\n");
            pthread_mutex_lock(&g_reg_mu);
            registry_load_locked();
            pthread_mutex_unlock(&g_reg_mu);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = { .tv_sec=1, .tv_usec=0 };
        int ready = select(sock+1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        int n = fabric_recv(sock, buf, sizeof(buf)-1, &src);
        if (n <= 0) continue;
        buf[n] = '\0';
        resp[0] = '\0';

        if (!strncmp(buf, "RBAC_ADMIN|", 11)) {
            handle_admin(buf, resp, sizeof(resp));
        } else if (!strncmp(buf, "RBAC|", 5)) {
            handle_rbac(buf, resp, sizeof(resp),
                        cfg.conductor_host, cfg.conductor_port);
        } else if (!strncmp(buf, "PING", 4)) {
            snprintf(resp, sizeof(resp), "OK|PONG|rbac-gateway");
        } else {
            snprintf(resp, sizeof(resp), "ERR|use RBAC|team|ns|cmd|ts|sig format");
        }

        if (resp[0])
            fabric_send(sock, src.ip, src.port, resp, strlen(resp));
    }
    return 0;
}
