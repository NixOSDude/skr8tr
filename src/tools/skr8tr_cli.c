/*
 * skr8tr_cli.c — Skr8tr Unified Operator CLI
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 3 — Manifest Shard (operator tool)
 *
 * Single binary that speaks to the Conductor (UDP 7771) and the RBAC
 * gateway (UDP 7773).  Handles ML-DSA-65 signing automatically.
 *
 * Usage:
 *   skr8tr [flags] <command> [args...]
 *
 * Global flags:
 *   --host  <ip>       Conductor host     (default: 127.0.0.1)
 *   --port  <port>     Conductor port     (default: 7771)
 *   --key   <path>     ML-DSA-65 secret key for conductor auth
 *                      (default: ~/.skr8tr/signing.sec if present)
 *   --team  <name>     RBAC team name (enables RBAC gateway mode)
 *   --ns    <ns>       RBAC namespace  (required with --team)
 *   --rkey  <path>     Team secret key for RBAC signing (required with --team)
 *   --rbac-port <p>    RBAC gateway port (default: 7773)
 *   --timeout <s>      Response timeout in seconds (default: 5)
 *
 * Workload commands:
 *   up      <manifest.skr8tr>   Submit workload
 *   down    <app>               Evict workload (all replicas)
 *   rollout <manifest.skr8tr>   Rolling zero-downtime update
 *   exec    <app> <cmd>         Run shell command inside app replica
 *   logs    <app>               Tail app output log
 *   lookup  <app>               Show which nodes run the app
 *
 * Cluster commands:
 *   nodes                       List live mesh nodes + metrics
 *   list                        List all running workloads
 *   ping                        Ping Conductor (health check)
 *
 * Enterprise namespace commands:
 *   ns list                     List namespaces + quota usage
 *   ns add <name> <max_r> <cpu> Add/update namespace quota
 *   ns revoke <name>            Deactivate namespace
 *   autoscale rules             List autoscale rules
 *
 * Enterprise RBAC admin commands (require --team --ns --rkey):
 *   rbac team list              List all registered teams
 *   rbac team add <name> <ns> <perms_hex> <pubkey_hex>
 *   rbac team revoke <name>
 *
 * Examples:
 *   skr8tr ping
 *   skr8tr nodes
 *   skr8tr --key ~/.skr8tr/signing.sec up apps/api.skr8tr
 *   skr8tr --key ~/.skr8tr/signing.sec down api
 *   skr8tr --team ml-team --ns ml-prod --rkey ~/.skr8tr/team.sec \
 *            up ml-prod/trainer.skr8tr
 *   skr8tr exec api "ps aux"
 *   skr8tr ns list
 */

#include "../core/fabric.h"
#include "../core/skrauth.h"

#include <arpa/inet.h>
#include <errno.h>
#include <oqs/oqs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define CLI_DEFAULT_HOST        "127.0.0.1"
#define CLI_DEFAULT_PORT        7771
#define CLI_DEFAULT_RBAC_PORT   7773
#define CLI_DEFAULT_TIMEOUT_S   5
#define CLI_RESP_BUF            65536

/* ML-DSA-65 key sizes (RBAC signing uses same algorithm as conductor) */
#define CLI_SK_LEN     4032
#define CLI_SIG_LEN    3309
#define CLI_HEXSIG_LEN 6618
#define CLI_PK_LEN     1952

/* -------------------------------------------------------------------------
 * Hex helpers — for RBAC wire-format signing
 * ---------------------------------------------------------------------- */

static void bytes_to_hex(const uint8_t* b, size_t len, char* out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = h[b[i] >> 4];
        out[i*2+1] = h[b[i] & 0xf];
    }
    out[len*2] = '\0';
}

/* -------------------------------------------------------------------------
 * UDP send + receive with timeout
 * Returns response length, -1 on timeout/error.
 * ---------------------------------------------------------------------- */

static int udp_send_recv(const char* host, int port, int timeout_s,
                         const char* msg,
                         char* resp, size_t resp_len) {
    int sock = fabric_bind(0);   /* ephemeral port */
    if (sock < 0) {
        fprintf(stderr, "error: cannot create UDP socket: %s\n",
                strerror(errno));
        return -1;
    }

    fabric_send(sock, host, port, msg, strlen(msg));

    struct timeval tv = { .tv_sec=timeout_s, .tv_usec=0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    FabricAddr src;
    int n = fabric_recv(sock, resp, resp_len - 1, &src);
    close(sock);

    if (n <= 0) {
        fprintf(stderr, "error: no response from %s:%d within %ds\n",
                host, port, timeout_s);
        return -1;
    }
    resp[n] = '\0';
    return n;
}

/* -------------------------------------------------------------------------
 * Print response — format OK/ERR and unwrap fields for readability
 * ---------------------------------------------------------------------- */

static void print_response(const char* resp) {
    if (!strncmp(resp, "ERR|", 4)) {
        fprintf(stderr, "ERR: %s\n", resp + 4);
        return;
    }
    if (!strncmp(resp, "OK|", 3)) {
        /* Strip the OK| prefix and first word (command echo) */
        const char* p = resp + 3;
        const char* sep = strchr(p, '|');
        if (sep) {
            /* Print command tag then formatted body */
            char tag[64] = {0};
            size_t tl = (size_t)(sep - p);
            if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
            memcpy(tag, p, tl);
            printf("[%s]\n", tag);
            p = sep + 1;
            /* For list responses: comma-separated → one per line */
            if (strchr(p, ',')) {
                char buf[CLI_RESP_BUF];
                size_t bl = strlen(p);
                if (bl >= sizeof(buf)) bl = sizeof(buf) - 1;
                memcpy(buf, p, bl); buf[bl] = '\0';
                char* tok = buf;
                while (tok) {
                    char* comma = strchr(tok, ',');
                    if (comma) *comma = '\0';
                    printf("  %s\n", tok);
                    tok = comma ? comma + 1 : NULL;
                }
            } else {
                /* Skip count field if numeric */
                char* end;
                strtol(p, &end, 10);
                if (*end == '|') p = end + 1;
                printf("  %s\n", p);
            }
        } else {
            printf("%s\n", resp + 3);
        }
        return;
    }
    /* Raw / unexpected format */
    printf("%s\n", resp);
}

/* -------------------------------------------------------------------------
 * Conductor command — optionally ML-DSA-65 signed
 * ---------------------------------------------------------------------- */

static int conductor_cmd(const char* host, int port, int timeout_s,
                         const char* cmd, const char* seckey_path,
                         char* resp, size_t resp_len) {
    char wire[16384 + SKRAUTH_HEXSIG_LEN + 64];

    if (seckey_path && *seckey_path) {
        char err[256];
        if (skrauth_sign(cmd, seckey_path, wire, sizeof(wire),
                         err, sizeof(err)) < 0) {
            fprintf(stderr, "sign error: %s\n", err);
            return -1;
        }
    } else {
        size_t cl = strlen(cmd);
        if (cl >= sizeof(wire)) cl = sizeof(wire) - 1;
        memcpy(wire, cmd, cl); wire[cl] = '\0';
    }

    return udp_send_recv(host, port, timeout_s, wire, resp, resp_len);
}

/* -------------------------------------------------------------------------
 * RBAC command — ML-DSA-65 signed with team secret key
 * Wire: RBAC|<team>|<ns>|<bare_cmd>|<ts>|<sig_hex>
 * ---------------------------------------------------------------------- */

static int rbac_cmd(const char* host, int rbac_port, int timeout_s,
                    const char* team, const char* ns,
                    const char* bare_cmd, const char* team_seckey,
                    char* resp, size_t resp_len) {
    /* Load team secret key */
    uint8_t sk[CLI_SK_LEN];
    FILE* fp = fopen(team_seckey, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open team key '%s': %s\n",
                team_seckey, strerror(errno));
        return -1;
    }
    size_t sk_len = fread(sk, 1, sizeof(sk), fp);
    fclose(fp);
    if (sk_len != CLI_SK_LEN) {
        fprintf(stderr, "error: team key wrong size (%zu, expected %d)\n",
                sk_len, CLI_SK_LEN);
        return -1;
    }

    /* Build payload: "<team>|<ns>|<bare_cmd>|<ts>" */
    char ts_s[32];
    snprintf(ts_s, sizeof(ts_s), "%ld", (long)time(NULL));

    char payload[4096];
    snprintf(payload, sizeof(payload), "%s|%s|%s|%s",
             team, ns, bare_cmd, ts_s);

    /* Sign payload */
    OQS_SIG* oqs = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!oqs) {
        fprintf(stderr, "error: OQS_SIG_new failed\n");
        return -1;
    }

    uint8_t sig[CLI_SIG_LEN];
    size_t  sig_len = CLI_SIG_LEN;
    OQS_STATUS st = OQS_SIG_sign(
        oqs, sig, &sig_len,
        (const uint8_t*)payload, strlen(payload),
        sk
    );
    OQS_SIG_free(oqs);
    memset(sk, 0, sizeof(sk));

    if (st != OQS_SUCCESS) {
        fprintf(stderr, "error: ML-DSA-65 signing failed\n");
        return -1;
    }

    /* Hex-encode signature */
    char sig_hex[CLI_HEXSIG_LEN + 4];
    bytes_to_hex(sig, CLI_SIG_LEN, sig_hex);

    /* Build wire message */
    char wire[4096 + CLI_HEXSIG_LEN + 64];
    snprintf(wire, sizeof(wire), "RBAC|%s|%s|%s|%s|%s",
             team, ns, bare_cmd, ts_s, sig_hex);

    return udp_send_recv(host, rbac_port, timeout_s, wire, resp, resp_len);
}

/* -------------------------------------------------------------------------
 * RBAC admin command — signed admin wire format
 * Wire: RBAC_ADMIN|<team>|<admin_cmd>|<args...>|<ts>|<sig>
 * Signed: "<team>|<admin_cmd>|<args...>|<ts>"
 * ---------------------------------------------------------------------- */

static int rbac_admin_cmd(const char* host, int rbac_port, int timeout_s,
                          const char* team, const char* admin_cmd_str,
                          const char* team_seckey,
                          char* resp, size_t resp_len) {
    uint8_t sk[CLI_SK_LEN];
    FILE* fp = fopen(team_seckey, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open team key '%s': %s\n",
                team_seckey, strerror(errno));
        return -1;
    }
    size_t sk_len = fread(sk, 1, sizeof(sk), fp);
    fclose(fp);
    if (sk_len != CLI_SK_LEN) {
        fprintf(stderr, "error: team key wrong size\n");
        return -1;
    }

    char ts_s[32];
    snprintf(ts_s, sizeof(ts_s), "%ld", (long)time(NULL));

    /* Signed payload: "<team>|<admin_cmd_str>|<ts>"
     * admin_cmd_str includes the sub-command + args already
     * e.g. "TEAM_LIST" or "TEAM_REVOKE|target" or "TEAM_ADD|name|ns|..." */
    char payload[8192 + CLI_HEXSIG_LEN + 64];
    snprintf(payload, sizeof(payload), "%s|%s|%s",
             team, admin_cmd_str, ts_s);

    OQS_SIG* oqs = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!oqs) { memset(sk, 0, sizeof(sk)); return -1; }

    uint8_t sig[CLI_SIG_LEN];
    size_t  sig_len = CLI_SIG_LEN;
    OQS_STATUS st = OQS_SIG_sign(
        oqs, sig, &sig_len,
        (const uint8_t*)payload, strlen(payload),
        sk
    );
    OQS_SIG_free(oqs);
    memset(sk, 0, sizeof(sk));

    if (st != OQS_SUCCESS) {
        fprintf(stderr, "error: ML-DSA-65 admin signing failed\n");
        return -1;
    }

    char sig_hex[CLI_HEXSIG_LEN + 4];
    bytes_to_hex(sig, CLI_SIG_LEN, sig_hex);

    char wire[8192 + CLI_HEXSIG_LEN + 128];
    snprintf(wire, sizeof(wire), "RBAC_ADMIN|%s|%s|%s|%s",
             team, admin_cmd_str, ts_s, sig_hex);

    return udp_send_recv(host, rbac_port, timeout_s, wire, resp, resp_len);
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: skr8tr [flags] <command> [args...]\n"
        "\n"
        "flags:\n"
        "  --host  <ip>       Conductor host         (default: 127.0.0.1)\n"
        "  --port  <port>     Conductor port         (default: 7771)\n"
        "  --key   <path>     Secret key for conductor auth\n"
        "                     (default: ~/.skr8tr/signing.sec)\n"
        "  --team  <name>     RBAC team (enables gateway mode, port 7773)\n"
        "  --ns    <ns>       RBAC namespace (required with --team)\n"
        "  --rkey  <path>     Team secret key for RBAC signing\n"
        "  --rbac-port <p>    RBAC gateway port      (default: 7773)\n"
        "  --timeout <s>      Response timeout       (default: 5)\n"
        "\n"
        "workload commands:\n"
        "  up      <manifest>     Submit workload\n"
        "  down    <app>          Evict workload (all replicas)\n"
        "  rollout <manifest>     Rolling zero-downtime update\n"
        "  exec    <app> <cmd>    Run command in app replica\n"
        "  logs    <app>          Retrieve app output log\n"
        "  lookup  <app>          Show node placement for app\n"
        "\n"
        "cluster commands:\n"
        "  nodes                  List live nodes + metrics\n"
        "  list                   List running workloads\n"
        "  ping                   Ping Conductor\n"
        "\n"
        "enterprise commands:\n"
        "  ns list                List namespaces + quota usage\n"
        "  ns add <n> <max> <cpu> Add or update namespace quota\n"
        "  ns revoke <name>       Deactivate namespace\n"
        "  autoscale rules        List custom metric autoscale rules\n"
        "\n"
        "RBAC admin (requires --team --rkey, team must have ADMIN permission):\n"
        "  rbac team list\n"
        "  rbac team add <name> <ns> <perms_hex> <pubkey_hex>\n"
        "  rbac team revoke <name>\n"
        "\n"
        "examples:\n"
        "  skr8tr ping\n"
        "  skr8tr nodes\n"
        "  skr8tr --key ~/.skr8tr/signing.sec up apps/api.skr8tr\n"
        "  skr8tr --key ~/.skr8tr/signing.sec down api\n"
        "  skr8tr exec api 'ps aux'\n"
        "  skr8tr --team ml-team --ns prod --rkey ~/.skr8tr/team.sec up prod/train.skr8tr\n"
        "  skr8tr --team admin --rkey ~/.skr8tr/team.sec rbac team list\n"
    );
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    /* ── Parse global flags ── */
    const char* host      = CLI_DEFAULT_HOST;
    int         port      = CLI_DEFAULT_PORT;
    int         rbac_port = CLI_DEFAULT_RBAC_PORT;
    int         timeout_s = CLI_DEFAULT_TIMEOUT_S;
    const char* team      = NULL;
    const char* ns        = NULL;
    const char* rkey      = NULL;

    /* Auto-detect conductor signing key */
    char default_seckey[576] = {0};
    const char* home = getenv("HOME");
    if (home) {
        snprintf(default_seckey, sizeof(default_seckey),
                 "%s/.skr8tr/signing.sec", home);
        struct stat st;
        if (stat(default_seckey, &st) != 0) default_seckey[0] = '\0';
    }
    const char* seckey = default_seckey[0] ? default_seckey : NULL;

    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host")      && i+1 < argc) { host      = argv[++i]; continue; }
        if (!strcmp(argv[i], "--port")      && i+1 < argc) { port      = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--key")       && i+1 < argc) { seckey    = argv[++i]; continue; }
        if (!strcmp(argv[i], "--team")      && i+1 < argc) { team      = argv[++i]; continue; }
        if (!strcmp(argv[i], "--ns")        && i+1 < argc) { ns        = argv[++i]; continue; }
        if (!strcmp(argv[i], "--rkey")      && i+1 < argc) { rkey      = argv[++i]; continue; }
        if (!strcmp(argv[i], "--rbac-port") && i+1 < argc) { rbac_port = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--timeout")   && i+1 < argc) { timeout_s = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        break;   /* first non-flag argument = command */
    }

    if (i >= argc) { usage(); return 1; }
    const char* cmd = argv[i++];

    char resp[CLI_RESP_BUF];
    int rc;

    /* ── RBAC mode helper ── */
    int rbac_mode = (team != NULL);
    if (rbac_mode && !rkey) {
        /* Try default team key location */
        static char default_rkey[576];
        if (home)
            snprintf(default_rkey, sizeof(default_rkey),
                     "%s/.skr8tr/team.sec", home);
        struct stat st;
        if (home && stat(default_rkey, &st) == 0) rkey = default_rkey;
    }

#define COND_CMD(cmd_str) \
    do { \
        if (rbac_mode) { \
            if (!ns || !rkey) { \
                fprintf(stderr, "error: --team requires --ns and --rkey\n"); \
                return 1; \
            } \
            rc = rbac_cmd(host, rbac_port, timeout_s, team, ns, \
                          (cmd_str), rkey, resp, sizeof(resp)); \
        } else { \
            rc = conductor_cmd(host, port, timeout_s, \
                               (cmd_str), seckey, resp, sizeof(resp)); \
        } \
    } while(0)

    /* ── Workload commands ── */

    if (!strcmp(cmd, "up") || !strcmp(cmd, "submit")) {
        if (i >= argc) { fprintf(stderr, "error: up requires <manifest>\n"); return 1; }
        char wire[512];
        snprintf(wire, sizeof(wire), "SUBMIT|%s", argv[i]);
        COND_CMD(wire);
        goto done;
    }

    if (!strcmp(cmd, "down") || !strcmp(cmd, "evict")) {
        if (i >= argc) { fprintf(stderr, "error: down requires <app>\n"); return 1; }
        char wire[256];
        snprintf(wire, sizeof(wire), "EVICT|%s", argv[i]);
        COND_CMD(wire);
        goto done;
    }

    if (!strcmp(cmd, "rollout")) {
        if (i >= argc) { fprintf(stderr, "error: rollout requires <manifest>\n"); return 1; }
        char wire[512];
        snprintf(wire, sizeof(wire), "ROLLOUT|%s", argv[i]);
        COND_CMD(wire);
        goto done;
    }

    if (!strcmp(cmd, "exec")) {
        if (i + 1 >= argc) {
            fprintf(stderr, "error: exec requires <app> <cmd>\n"); return 1;
        }
        char wire[1024];
        snprintf(wire, sizeof(wire), "EXEC|%s|%s", argv[i], argv[i+1]);
        COND_CMD(wire);
        goto done;
    }

    if (!strcmp(cmd, "logs")) {
        if (i >= argc) { fprintf(stderr, "error: logs requires <app>\n"); return 1; }
        char wire[256];
        snprintf(wire, sizeof(wire), "LOGS|%s", argv[i]);
        COND_CMD(wire);
        goto done;
    }

    if (!strcmp(cmd, "lookup")) {
        if (i >= argc) { fprintf(stderr, "error: lookup requires <app>\n"); return 1; }
        char wire[256];
        snprintf(wire, sizeof(wire), "LOOKUP|%s", argv[i]);
        COND_CMD(wire);
        goto done;
    }

    /* ── Cluster commands ── */

    if (!strcmp(cmd, "nodes")) {
        COND_CMD("NODES");
        goto done;
    }

    if (!strcmp(cmd, "list") || !strcmp(cmd, "ps")) {
        COND_CMD("LIST");
        goto done;
    }

    if (!strcmp(cmd, "ping")) {
        COND_CMD("PING");
        goto done;
    }

    /* ── Enterprise namespace commands ── */

    if (!strcmp(cmd, "ns")) {
        if (i >= argc) {
            fprintf(stderr, "error: ns requires: list | add | revoke\n"); return 1;
        }
        const char* sub = argv[i++];

        if (!strcmp(sub, "list")) {
            COND_CMD("NAMESPACE_LIST");
            goto done;
        }
        if (!strcmp(sub, "add")) {
            if (i + 2 >= argc) {
                fprintf(stderr, "error: ns add <name> <max_replicas> <cpu_quota_pct>\n");
                return 1;
            }
            char wire[256];
            snprintf(wire, sizeof(wire), "NAMESPACE_ADD|%s|%s|%s",
                     argv[i], argv[i+1], argv[i+2]);
            COND_CMD(wire);
            goto done;
        }
        if (!strcmp(sub, "revoke")) {
            if (i >= argc) {
                fprintf(stderr, "error: ns revoke <name>\n"); return 1;
            }
            char wire[256];
            snprintf(wire, sizeof(wire), "NAMESPACE_REVOKE|%s", argv[i]);
            COND_CMD(wire);
            goto done;
        }
        fprintf(stderr, "error: unknown ns subcommand: %s\n", sub);
        return 1;
    }

    if (!strcmp(cmd, "autoscale")) {
        if (i >= argc) {
            fprintf(stderr, "error: autoscale requires: rules\n"); return 1;
        }
        if (!strcmp(argv[i], "rules")) {
            COND_CMD("AUTOSCALE_RULES");
            goto done;
        }
        fprintf(stderr, "error: unknown autoscale subcommand: %s\n", argv[i]);
        return 1;
    }

    /* ── RBAC admin commands ── */

    if (!strcmp(cmd, "rbac")) {
        if (i >= argc) {
            fprintf(stderr, "error: rbac requires: team\n"); return 1;
        }
        const char* obj = argv[i++];

        if (!strcmp(obj, "team")) {
            if (i >= argc) {
                fprintf(stderr, "error: rbac team requires: list | add | revoke\n");
                return 1;
            }
            const char* sub = argv[i++];

            if (!team || !rkey) {
                fprintf(stderr, "error: rbac commands require --team and --rkey\n");
                return 1;
            }

            if (!strcmp(sub, "list")) {
                rc = rbac_admin_cmd(host, rbac_port, timeout_s,
                                   team, "TEAM_LIST", rkey,
                                   resp, sizeof(resp));
                goto done;
            }
            if (!strcmp(sub, "revoke")) {
                if (i >= argc) {
                    fprintf(stderr, "error: rbac team revoke <name>\n"); return 1;
                }
                char sub_cmd[256];
                snprintf(sub_cmd, sizeof(sub_cmd), "TEAM_REVOKE|%s", argv[i]);
                rc = rbac_admin_cmd(host, rbac_port, timeout_s,
                                   team, sub_cmd, rkey,
                                   resp, sizeof(resp));
                goto done;
            }
            if (!strcmp(sub, "add")) {
                if (i + 3 >= argc) {
                    fprintf(stderr,
                            "error: rbac team add <name> <ns> <perms_hex> <pubkey_hex>\n");
                    return 1;
                }
                char sub_cmd[CLI_HEXSIG_LEN + 256];
                snprintf(sub_cmd, sizeof(sub_cmd),
                         "TEAM_ADD|%s|%s|%s|%s",
                         argv[i], argv[i+1], argv[i+2], argv[i+3]);
                rc = rbac_admin_cmd(host, rbac_port, timeout_s,
                                   team, sub_cmd, rkey,
                                   resp, sizeof(resp));
                goto done;
            }
            fprintf(stderr, "error: unknown rbac team subcommand: %s\n", sub);
            return 1;
        }
        fprintf(stderr, "error: unknown rbac object: %s\n", obj);
        return 1;
    }

    fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
    usage();
    return 1;

done:
    if (rc < 0) return 1;
    print_response(resp);
    /* Exit 1 if response indicates error */
    return (strncmp(resp, "ERR|", 4) == 0) ? 1 : 0;
}
