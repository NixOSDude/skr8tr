/*
 * skr8tr_audit.c — Cryptographic Audit Ledger — Implementation
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * Each audit entry is SHA-256 chained:
 *   entry_hash = SHA256(prev_hash || seq || ts || event || app || src_ip || detail)
 *
 * Any tampering with a historical entry invalidates every subsequent hash.
 * The log is append-only; no delete, no update — skraudit_verify_chain()
 * detects any modification.
 *
 * Compliance mapping:
 *   HIPAA § 164.312(b)   — Audit Controls (activity tracking)
 *   HITRUST CSF 09.aa    — Audit Logging  (event capture)
 *   HITRUST CSF 09.ac    — Log Integrity  (hash chain = tamper evidence)
 *   NIST 800-53 AU-9     — Protection of Audit Information
 *   NIST 800-53 AU-10    — Non-repudiation (SHA-256 over all fields)
 *   PCI DSS 10.2–10.3    — Audit Log Events and Content
 *   SOC 2 CC7.2          — Logical Access Monitoring
 */

#include "skr8tr_audit.h"

#include <openssl/sha.h>      /* SHA256() — OpenSSL, same dep as ingress */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static char           g_log_path[512]              = SKRAUDIT_LOG_PATH;
static pthread_mutex_t g_audit_mu                  = PTHREAD_MUTEX_INITIALIZER;
static uint64_t       g_seq                        = 0;
static uint8_t        g_prev_hash[SKRAUDIT_HASH_BYTES]; /* last entry hash */
static int            g_initialised                = 0;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* hex-encode `len` bytes of `src` into `dst` (dst must be len*2+1 bytes) */
static void hex_encode(const uint8_t* src, size_t len, char* dst) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = H[src[i] >> 4];
        dst[i * 2 + 1] = H[src[i] & 0xf];
    }
    dst[len * 2] = '\0';
}

/* hex-decode SKRAUDIT_HASH_HEX chars from `src` into dst[SKRAUDIT_HASH_BYTES] */
static int hex_decode(const char* src, uint8_t* dst) {
    for (int i = 0; i < SKRAUDIT_HASH_BYTES; i++) {
        unsigned int hi, lo;
        char hc = src[i * 2], lc = src[i * 2 + 1];
        if (hc >= '0' && hc <= '9') hi = (unsigned)(hc - '0');
        else if (hc >= 'a' && hc <= 'f') hi = (unsigned)(hc - 'a' + 10);
        else return -1;
        if (lc >= '0' && lc <= '9') lo = (unsigned)(lc - '0');
        else if (lc >= 'a' && lc <= 'f') lo = (unsigned)(lc - 'a' + 10);
        else return -1;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* compute SHA-256 of a string into out[32] */
static void sha256_str(const char* s, size_t len, uint8_t* out) {
    SHA256((const unsigned char*)s, len, out);
}

/* -------------------------------------------------------------------------
 * skraudit_init
 * ---------------------------------------------------------------------- */

void skraudit_init(const char* path) {
    pthread_mutex_lock(&g_audit_mu);

    if (path && path[0])
        snprintf(g_log_path, sizeof(g_log_path), "%s", path);

    /* Zero-out the genesis prev_hash */
    memset(g_prev_hash, 0, sizeof(g_prev_hash));
    g_seq = 0;

    /* If the log already exists, read the last entry to restore chain state */
    FILE* f = fopen(g_log_path, "r");
    if (f) {
        char line[1024];
        char last[1024] = {0};
        while (fgets(line, sizeof(line), f))
            memcpy(last, line, sizeof(last));
        fclose(f);

        if (last[0]) {
            /* Format: seq|ts|event|app|src_ip|detail|entry_hash */
            /* Find the last '|' — that's the hash field */
            char* p = strrchr(last, '|');
            if (p && strlen(p + 1) >= SKRAUDIT_HASH_HEX) {
                hex_decode(p + 1, g_prev_hash);
            }
            /* Restore sequence number from first field */
            uint64_t seq_r = 0;
            if (sscanf(last, "%lu", &seq_r) == 1)
                g_seq = seq_r + 1;
        }
    }

    g_initialised = 1;
    pthread_mutex_unlock(&g_audit_mu);

    /* Emit conductor-start event (seq 0 on fresh log, or resumed seq) */
    skraudit_log(SKRAUDIT_CONDUCTOR, "", "local", "conductor started");
}

/* -------------------------------------------------------------------------
 * skraudit_log
 * ---------------------------------------------------------------------- */

void skraudit_log(const char* event, const char* app,
                  const char* src_ip, const char* detail) {
    if (!event) event = "";
    if (!app)   app   = "";
    if (!src_ip) src_ip = "local";
    if (!detail) detail = "";

    pthread_mutex_lock(&g_audit_mu);

    if (!g_initialised) {
        /* Auto-init if someone calls skraudit_log before skraudit_init */
        memset(g_prev_hash, 0, sizeof(g_prev_hash));
        g_initialised = 1;
    }

    /* Build timestamp string */
    char ts_buf[32];
    time_t now = time(NULL);
    struct tm* tm_utc = gmtime(&now);
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

    /* Assemble the content to hash:
     *   prev_hash_hex + "|" + seq + "|" + ts + "|" + event + "|" + app
     *   + "|" + src_ip + "|" + detail                                   */
    char prev_hex[SKRAUDIT_HASH_HEX + 1];
    hex_encode(g_prev_hash, SKRAUDIT_HASH_BYTES, prev_hex);

    char hash_input[2048];
    int  hi_len = snprintf(hash_input, sizeof(hash_input),
                           "%s|%lu|%s|%s|%s|%s|%s",
                           prev_hex, (unsigned long)g_seq,
                           ts_buf, event, app, src_ip, detail);
    if (hi_len < 0 || (size_t)hi_len >= sizeof(hash_input))
        hi_len = (int)sizeof(hash_input) - 1;

    /* Compute entry hash */
    uint8_t entry_hash[SKRAUDIT_HASH_BYTES];
    sha256_str(hash_input, (size_t)hi_len, entry_hash);

    char entry_hex[SKRAUDIT_HASH_HEX + 1];
    hex_encode(entry_hash, SKRAUDIT_HASH_BYTES, entry_hex);

    /* Write log line:  seq|ts|event|app|src_ip|detail|entry_hash\n */
    FILE* f = fopen(g_log_path, "a");
    if (f) {
        fprintf(f, "%lu|%s|%s|%s|%s|%s|%s\n",
                (unsigned long)g_seq, ts_buf,
                event, app, src_ip, detail, entry_hex);
        fclose(f);
    } else {
        /* Fallback: stderr so nothing is silently lost */
        fprintf(stderr,
                "[audit-WARN] cannot open %s: %s — "
                "entry: %lu|%s|%s|%s|%s|%s\n",
                g_log_path, strerror(errno),
                (unsigned long)g_seq, ts_buf,
                event, app, src_ip, detail);
    }

    /* Advance chain state */
    memcpy(g_prev_hash, entry_hash, SKRAUDIT_HASH_BYTES);
    g_seq++;

    pthread_mutex_unlock(&g_audit_mu);
}

/* -------------------------------------------------------------------------
 * skraudit_tail — read last n lines from log into buf
 * ---------------------------------------------------------------------- */

int skraudit_tail(int n, char* buf, size_t buf_len) {
    if (n <= 0 || !buf || buf_len == 0) return 0;
    buf[0] = '\0';

    FILE* f = fopen(g_log_path, "r");
    if (!f) {
        snprintf(buf, buf_len, "(no audit log at %s)", g_log_path);
        return (int)strlen(buf);
    }

    /* Collect lines into a circular array of pointers into a flat buffer */
#define TAIL_MAX_LINES 256
#define TAIL_BUF_SZ    (256 * 1024)

    char*  flat = malloc(TAIL_BUF_SZ);
    char*  ptrs[TAIL_MAX_LINES];
    int    lens[TAIL_MAX_LINES];
    if (!flat) { fclose(f); return 0; }

    int   head = 0, count = 0;
    int   used = 0;
    char  line[1024];

    while (fgets(line, sizeof(line), f)) {
        int ll = (int)strlen(line);
        if (used + ll + 1 > TAIL_BUF_SZ) {
            /* Ran out of flat space — reset (sacrifice oldest batch) */
            used = 0; head = 0; count = 0;
        }
        memcpy(flat + used, line, (size_t)(ll + 1));
        ptrs[head % TAIL_MAX_LINES] = flat + used;
        lens[head % TAIL_MAX_LINES] = ll;
        head = (head + 1) % TAIL_MAX_LINES;
        if (count < TAIL_MAX_LINES) count++;
        used += ll + 1;
    }
    fclose(f);

    /* Determine start index for the last n entries */
    int start = count > n ? count - n : 0;
    /* Adjust head back to find the start in circular buffer */
    int abs_start = (head - count + start + TAIL_MAX_LINES * 2) % TAIL_MAX_LINES;

    size_t written = 0;
    for (int i = 0; i < count - start && written < buf_len - 1; i++) {
        int idx = (abs_start + i) % TAIL_MAX_LINES;
        size_t room = buf_len - 1 - written;
        size_t copy = (size_t)lens[idx] < room ? (size_t)lens[idx] : room;
        memcpy(buf + written, ptrs[idx], copy);
        written += copy;
    }
    buf[written] = '\0';

    free(flat);
    return (int)written;

#undef TAIL_MAX_LINES
#undef TAIL_BUF_SZ
}

/* -------------------------------------------------------------------------
 * skraudit_verify_chain — walk every entry, recompute hash, compare
 * ---------------------------------------------------------------------- */

int skraudit_verify_chain(char* err, size_t err_len) {
    FILE* f = fopen(g_log_path, "r");
    if (!f) {
        snprintf(err, err_len, "cannot open audit log: %s", strerror(errno));
        return -1;
    }

    uint8_t  prev[SKRAUDIT_HASH_BYTES];
    memset(prev, 0, sizeof(prev));

    char     line[1024];
    uint64_t expected_seq = 0;
    int      ok = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';

        if (ll == 0) continue;

        /* Extract the stored entry_hash (last field) */
        char* last_pipe = strrchr(line, '|');
        if (!last_pipe || strlen(last_pipe + 1) < SKRAUDIT_HASH_HEX) {
            snprintf(err, err_len,
                     "malformed entry at seq %lu", (unsigned long)expected_seq);
            fclose(f);
            return -1;
        }
        char stored_hex[SKRAUDIT_HASH_HEX + 1];
        strncpy(stored_hex, last_pipe + 1, SKRAUDIT_HASH_HEX);
        stored_hex[SKRAUDIT_HASH_HEX] = '\0';

        uint8_t stored[SKRAUDIT_HASH_BYTES];
        if (hex_decode(stored_hex, stored) < 0) {
            snprintf(err, err_len,
                     "invalid hash hex at seq %lu", (unsigned long)expected_seq);
            fclose(f);
            return -1;
        }

        /* Reconstruct the fields portion (everything before the last pipe) */
        size_t fields_len = (size_t)(last_pipe - line);
        char fields[1024];
        if (fields_len >= sizeof(fields)) fields_len = sizeof(fields) - 1;
        memcpy(fields, line, fields_len);
        fields[fields_len] = '\0';

        /* Extract seq from first field to validate ordering */
        uint64_t got_seq = 0;
        sscanf(fields, "%lu", &got_seq);
        if (got_seq != expected_seq) {
            snprintf(err, err_len,
                     "sequence gap: expected %lu got %lu",
                     (unsigned long)expected_seq, (unsigned long)got_seq);
            fclose(f);
            return -1;
        }

        /* Recompute hash: prev_hex + "|" + fields */
        char prev_hex[SKRAUDIT_HASH_HEX + 1];
        hex_encode(prev, SKRAUDIT_HASH_BYTES, prev_hex);

        char hash_input[2048];
        int  hi_len = snprintf(hash_input, sizeof(hash_input),
                               "%s|%s", prev_hex, fields);
        if (hi_len < 0) hi_len = 0;

        uint8_t computed[SKRAUDIT_HASH_BYTES];
        sha256_str(hash_input, (size_t)hi_len, computed);

        if (memcmp(computed, stored, SKRAUDIT_HASH_BYTES) != 0) {
            char comp_hex[SKRAUDIT_HASH_HEX + 1];
            hex_encode(computed, SKRAUDIT_HASH_BYTES, comp_hex);
            snprintf(err, err_len,
                     "CHAIN BROKEN at seq %lu: stored=%s computed=%s",
                     (unsigned long)expected_seq, stored_hex, comp_hex);
            fclose(f);
            return -1;
        }

        memcpy(prev, stored, SKRAUDIT_HASH_BYTES);
        expected_seq++;
        ok++;
    }
    fclose(f);

    if (ok == 0) {
        snprintf(err, err_len, "audit log is empty");
        return -1;
    }

    snprintf(err, err_len, "OK: %d entries verified — chain intact", ok);
    return 0;
}
