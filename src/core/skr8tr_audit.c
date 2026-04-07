/*
 * skr8tr_audit.c — Cryptographic Audit Ledger — Implementation
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * Each audit entry is SHA-256 chained:
 *   entry_hash = SHA256(prev_hash || seq || ts || event || app || src_ip || detail)
 *
 * Optional AES-256-GCM at-rest encryption:
 *   Each log line is individually encrypted.  Format when enabled:
 *     ENC:<nonce_hex(24)>:<ciphertext_hex>:<tag_hex(32)>\n
 *   The hash chain operates over plaintext before encryption so that
 *   skraudit_verify_chain() can verify after decryption.
 *
 * Compliance mapping:
 *   HIPAA § 164.312(a)(2)(iv) — Encryption of PHI data at rest (AES-256-GCM)
 *   HIPAA § 164.312(b)        — Audit Controls (activity tracking)
 *   HITRUST CSF 09.aa         — Audit Logging  (event capture)
 *   HITRUST CSF 09.ac         — Log Integrity  (hash chain = tamper evidence)
 *   NIST 800-53 AU-9          — Protection of Audit Information (chmod 600)
 *   NIST 800-53 AU-10         — Non-repudiation (SHA-256 over all fields)
 *   PCI DSS 3.4               — Protect stored data (AES-256-GCM)
 *   PCI DSS 10.2–10.3         — Audit Log Events and Content
 *   SOC 2 CC7.2               — Logical Access Monitoring
 */

#include "skr8tr_audit.h"
#include "skr8tr_syslog.h"

#include <openssl/evp.h>      /* EVP_* — AES-256-GCM encryption */
#include <openssl/rand.h>     /* RAND_bytes() — cryptographic nonce */
#include <openssl/sha.h>      /* SHA256() */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static char            g_log_path[512]             = SKRAUDIT_LOG_PATH;
static pthread_mutex_t g_audit_mu                  = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_seq                       = 0;
static uint8_t         g_prev_hash[SKRAUDIT_HASH_BYTES];
static int             g_initialised               = 0;

/* AES-256-GCM at-rest encryption — opt-in via skraudit_set_encryption() */
#define AUDIT_KEY_LEN   32   /* AES-256 */
#define AUDIT_NONCE_LEN 12   /* GCM recommended nonce */
#define AUDIT_TAG_LEN   16   /* GCM authentication tag */

static int             g_encrypt                   = 0;
static uint8_t         g_aes_key[AUDIT_KEY_LEN]    = {0};

/* Syslog forwarding — opt-in via skraudit_set_syslog() */
static int             g_syslog_enabled            = 0;

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
 * AES-256-GCM helpers
 * ---------------------------------------------------------------------- */

/* Encrypt `plain` (null-terminated) into a hex-encoded result string.
 * Result format: ENC:<nonce_hex>:<ciphertext_hex>:<tag_hex>
 * Returns 0 on success, -1 on failure.  out must be >= 2*len+200 bytes. */
static int aes_gcm_encrypt(const char* plain, char* out, size_t out_len) {
    size_t plain_len = strlen(plain);

    uint8_t nonce[AUDIT_NONCE_LEN];
    if (RAND_bytes(nonce, AUDIT_NONCE_LEN) != 1) return -1;

    uint8_t* ciphertext = malloc(plain_len + AUDIT_TAG_LEN);
    if (!ciphertext) return -1;

    uint8_t tag[AUDIT_TAG_LEN];

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ciphertext); return -1; }

    int ok = 1;
    int outlen = 0, tmplen = 0;
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1;
    ok &= EVP_EncryptInit_ex(ctx, NULL, NULL, g_aes_key, nonce) == 1;
    ok &= EVP_EncryptUpdate(ctx, ciphertext, &outlen,
                            (const uint8_t*)plain, (int)plain_len) == 1;
    ok &= EVP_EncryptFinal_ex(ctx, ciphertext + outlen, &tmplen) == 1;
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                               AUDIT_TAG_LEN, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) { free(ciphertext); return -1; }

    int cipher_len = outlen + tmplen;

    /* Encode: "ENC:<nonce_hex>:<ciphertext_hex>:<tag_hex>" */
    char nonce_hex[AUDIT_NONCE_LEN * 2 + 1];
    hex_encode(nonce, AUDIT_NONCE_LEN, nonce_hex);

    char* tag_hex_str = malloc(AUDIT_TAG_LEN * 2 + 1);
    if (!tag_hex_str) { free(ciphertext); return -1; }
    hex_encode(tag, AUDIT_TAG_LEN, tag_hex_str);

    char* cipher_hex = malloc((size_t)cipher_len * 2 + 1);
    if (!cipher_hex) { free(ciphertext); free(tag_hex_str); return -1; }
    hex_encode(ciphertext, (size_t)cipher_len, cipher_hex);

    int n = snprintf(out, out_len, "ENC:%s:%s:%s",
                     nonce_hex, cipher_hex, tag_hex_str);

    free(ciphertext); free(tag_hex_str); free(cipher_hex);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

/* Decrypt one ENC:... line into plain_out.  Returns 0 on success, -1 on failure. */
static int aes_gcm_decrypt(const char* enc_line, char* plain_out,
                            size_t plain_out_len) {
    if (strncmp(enc_line, "ENC:", 4) != 0) {
        /* Not encrypted — pass through as-is (allows mixed plaintext/encrypted) */
        snprintf(plain_out, plain_out_len, "%s", enc_line);
        return 0;
    }

    const char* p = enc_line + 4;
    const char* c1 = strchr(p, ':');
    if (!c1) return -1;
    const char* c2 = strchr(c1 + 1, ':');
    if (!c2) return -1;

    /* Decode nonce */
    size_t nonce_hex_len = (size_t)(c1 - p);
    if (nonce_hex_len != AUDIT_NONCE_LEN * 2) return -1;
    uint8_t nonce[AUDIT_NONCE_LEN];
    for (int i = 0; i < AUDIT_NONCE_LEN; i++) {
        unsigned int hi, lo;
        char hc = p[i*2], lc = p[i*2+1];
        if (hc>='0'&&hc<='9') hi=(unsigned)(hc-'0');
        else if (hc>='a'&&hc<='f') hi=(unsigned)(hc-'a'+10); else return -1;
        if (lc>='0'&&lc<='9') lo=(unsigned)(lc-'0');
        else if (lc>='a'&&lc<='f') lo=(unsigned)(lc-'a'+10); else return -1;
        nonce[i] = (uint8_t)((hi<<4)|lo);
    }

    /* Decode tag */
    size_t tag_hex_len = strlen(c2 + 1);
    /* strip trailing newline/whitespace */
    while (tag_hex_len > 0 &&
           (((const char*)(c2+1))[tag_hex_len-1] == '\n' ||
            ((const char*)(c2+1))[tag_hex_len-1] == '\r'))
        tag_hex_len--;
    if (tag_hex_len != AUDIT_TAG_LEN * 2) return -1;
    uint8_t tag[AUDIT_TAG_LEN];
    for (int i = 0; i < AUDIT_TAG_LEN; i++) {
        unsigned int hi, lo;
        char hc = (c2+1)[i*2], lc = (c2+1)[i*2+1];
        if (hc>='0'&&hc<='9') hi=(unsigned)(hc-'0');
        else if (hc>='a'&&hc<='f') hi=(unsigned)(hc-'a'+10); else return -1;
        if (lc>='0'&&lc<='9') lo=(unsigned)(lc-'0');
        else if (lc>='a'&&lc<='f') lo=(unsigned)(lc-'a'+10); else return -1;
        tag[i] = (uint8_t)((hi<<4)|lo);
    }

    /* Decode ciphertext */
    size_t cipher_hex_len = (size_t)(c2 - (c1 + 1));
    if (cipher_hex_len % 2 != 0) return -1;
    size_t cipher_len = cipher_hex_len / 2;
    uint8_t* ciphertext = malloc(cipher_len);
    if (!ciphertext) return -1;
    for (size_t i = 0; i < cipher_len; i++) {
        unsigned int hi, lo;
        char hc = (c1+1)[i*2], lc = (c1+1)[i*2+1];
        if (hc>='0'&&hc<='9') hi=(unsigned)(hc-'0');
        else if (hc>='a'&&hc<='f') hi=(unsigned)(hc-'a'+10);
        else { free(ciphertext); return -1; }
        if (lc>='0'&&lc<='9') lo=(unsigned)(lc-'0');
        else if (lc>='a'&&lc<='f') lo=(unsigned)(lc-'a'+10);
        else { free(ciphertext); return -1; }
        ciphertext[i] = (uint8_t)((hi<<4)|lo);
    }

    uint8_t* plaintext = malloc(cipher_len + 1);
    if (!plaintext) { free(ciphertext); return -1; }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ciphertext); free(plaintext); return -1; }

    int ok = 1, outlen = 0, tmplen = 0;
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1;
    ok &= EVP_DecryptInit_ex(ctx, NULL, NULL, g_aes_key, nonce) == 1;
    ok &= EVP_DecryptUpdate(ctx, plaintext, &outlen,
                            ciphertext, (int)cipher_len) == 1;
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                               AUDIT_TAG_LEN, tag) == 1;
    ok &= EVP_DecryptFinal_ex(ctx, plaintext + outlen, &tmplen) == 1;
    EVP_CIPHER_CTX_free(ctx);
    free(ciphertext);

    if (!ok) { free(plaintext); return -1; }

    plaintext[outlen + tmplen] = '\0';
    snprintf(plain_out, plain_out_len, "%s", (char*)plaintext);
    free(plaintext);
    return 0;
}

/* -------------------------------------------------------------------------
 * skraudit_set_encryption
 * ---------------------------------------------------------------------- */

int skraudit_set_encryption(const char* key_path) {
    if (!key_path || !key_path[0]) return -1;

    FILE* f = fopen(key_path, "rb");
    if (!f) {
        fprintf(stderr, "[audit] cannot open key file %s: %s\n",
                key_path, strerror(errno));
        return -1;
    }
    size_t n = fread(g_aes_key, 1, AUDIT_KEY_LEN, f);
    fclose(f);
    if (n != AUDIT_KEY_LEN) {
        fprintf(stderr, "[audit] key file too short: need %d bytes, got %zu\n",
                AUDIT_KEY_LEN, n);
        return -1;
    }
    g_encrypt = 1;
    fprintf(stderr, "[audit] AES-256-GCM at-rest encryption ENABLED\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * skraudit_set_syslog
 * ---------------------------------------------------------------------- */

void skraudit_set_syslog(int enabled) {
    g_syslog_enabled = enabled;
    if (enabled)
        fprintf(stderr, "[audit] syslog forwarding ENABLED\n");
}

/* compute SHA-256 of a string into out[32] */

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

    /* Harden the log file permissions — NIST 800-53 AU-9
     * O_CREAT | O_APPEND | O_WRONLY + chmod 600 ensures only root/owner
     * can read or write the audit log.                                    */
    int fd = open(g_log_path, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (fd >= 0) {
        fchmod(fd, 0600);   /* enforce even if umask is permissive */
        close(fd);
    }

    /* If the log already exists, read the last entry to restore chain state.
     * When encryption is enabled, decrypt the last entry to recover the hash. */
    FILE* f = fopen(g_log_path, "r");
    if (f) {
        char line[4096];
        char last[4096] = {0};
        while (fgets(line, sizeof(line), f))
            memcpy(last, line, sizeof(last));
        fclose(f);

        if (last[0]) {
            /* Decrypt if necessary before looking for the hash field */
            char plain[4096] = {0};
            if (g_encrypt && strncmp(last, "ENC:", 4) == 0)
                aes_gcm_decrypt(last, plain, sizeof(plain));
            else
                snprintf(plain, sizeof(plain), "%s", last);

            /* Format: seq|ts|event|app|src_ip|detail|entry_hash */
            char* p = strrchr(plain, '|');
            if (p && strlen(p + 1) >= SKRAUDIT_HASH_HEX) {
                hex_decode(p + 1, g_prev_hash);
            }
            uint64_t seq_r = 0;
            if (sscanf(plain, "%lu", &seq_r) == 1)
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

    /* Build the plaintext log line */
    char plain_line[2048];
    snprintf(plain_line, sizeof(plain_line),
             "%lu|%s|%s|%s|%s|%s|%s",
             (unsigned long)g_seq, ts_buf,
             event, app, src_ip, detail, entry_hex);

    /* Encrypt if requested — HIPAA § 164.312(a)(2)(iv), PCI DSS 3.4 */
    char write_line[8192];
    if (g_encrypt) {
        if (aes_gcm_encrypt(plain_line, write_line, sizeof(write_line)) < 0) {
            /* Encryption failure — write plaintext with WARNING prefix so
             * no audit event is ever silently dropped                      */
            snprintf(write_line, sizeof(write_line),
                     "ENCRYPT_FAIL|%s", plain_line);
        }
    } else {
        snprintf(write_line, sizeof(write_line), "%s", plain_line);
    }

    /* Append to log — O_APPEND is atomic on POSIX for writes ≤ PIPE_BUF */
    FILE* f = fopen(g_log_path, "a");
    if (f) {
        fprintf(f, "%s\n", write_line);
        fclose(f);
    } else {
        /* Fallback: stderr — nothing silently lost */
        fprintf(stderr,
                "[audit-WARN] cannot open %s: %s — entry: %s\n",
                g_log_path, strerror(errno), plain_line);
    }

    /* Forward to syslog SIEM — HIPAA § 164.312(b), PCI DSS 10.5.3 */
    if (g_syslog_enabled) {
        int sev = strcmp(event, SKRAUDIT_AUTH_FAIL) == 0
                  ? SKRSYSLOG_ERR : SKRSYSLOG_INFO;
        char syslog_msg[512];
        snprintf(syslog_msg, sizeof(syslog_msg),
                 "%s src=%s %s", event, src_ip, detail);
        /* Unlock before syslog_send — it has its own mutex, and we don't
         * want to hold g_audit_mu across a network call               */
        memcpy(g_prev_hash, entry_hash, SKRAUDIT_HASH_BYTES);
        g_seq++;
        pthread_mutex_unlock(&g_audit_mu);
        skrsyslog_send(sev, app[0] ? app : "conductor", event, syslog_msg);
        return;
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
    char  line[8192];   /* large enough for encrypted lines */
    char  plain[4096];

    while (fgets(line, sizeof(line), f)) {
        /* Decrypt if encryption is enabled */
        const char* store_line = line;
        if (g_encrypt && strncmp(line, "ENC:", 4) == 0) {
            if (aes_gcm_decrypt(line, plain, sizeof(plain)) == 0)
                store_line = plain;
        }
        int ll = (int)strlen(store_line);
        if (used + ll + 1 > TAIL_BUF_SZ) {
            used = 0; head = 0; count = 0;
        }
        memcpy(flat + used, store_line, (size_t)(ll + 1));
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

    char     raw[8192];   /* raw line from file (may be encrypted) */
    char     line[8192];  /* decrypted plaintext line — same size as raw */
    uint64_t expected_seq = 0;
    int      ok = 0;

    while (fgets(raw, sizeof(raw), f)) {
        /* Decrypt if needed */
        if (g_encrypt && strncmp(raw, "ENC:", 4) == 0) {
            if (aes_gcm_decrypt(raw, line, sizeof(line)) < 0) {
                snprintf(err, err_len,
                         "decryption failed at seq %lu — wrong key or tampered entry",
                         (unsigned long)expected_seq);
                fclose(f);
                return -1;
            }
        } else {
            snprintf(line, sizeof(line), "%s", raw);
        }

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
