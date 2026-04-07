/*
 * skrauth.c — Sovereign Command Authentication
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 */

#include "skrauth.h"

#include <errno.h>
#include <oqs/oqs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Hex encode / decode
 * ---------------------------------------------------------------------- */

static void hex_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = tbl[in[i] >> 4];
        out[i * 2 + 1] = tbl[in[i] & 0xf];
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, size_t hex_len, uint8_t *out) {
    if (hex_len % 2 != 0) return -1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Key loading — raw binary (OQS native format)
 * ---------------------------------------------------------------------- */

static int load_key(const char *path, uint8_t *out, size_t expected_len,
                    char *err, size_t err_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "cannot open '%s': %s", path, strerror(errno));
        return -1;
    }
    /* Read expected_len + 1 to detect oversized files */
    size_t n = fread(out, 1, expected_len + 1, f);
    fclose(f);
    if (n != expected_len) {
        snprintf(err, err_len,
                 "key '%s' wrong size: got %zu bytes, expected %zu",
                 path, n, expected_len);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * skrauth_sign
 * ---------------------------------------------------------------------- */

int skrauth_sign(const char *cmd, const char *seckey_path,
                 char *out, size_t out_len,
                 char *err, size_t err_len) {
    uint8_t sk[SKRAUTH_SK_LEN];
    if (load_key(seckey_path, sk, SKRAUTH_SK_LEN, err, err_len) < 0)
        return -1;

    /* payload = "<cmd>|<unix_ts>" */
    char payload[16384];
    int plen = snprintf(payload, sizeof(payload), "%s|%lld",
                        cmd, (long long)time(NULL));
    if (plen <= 0 || plen >= (int)sizeof(payload)) {
        snprintf(err, err_len, "command too long to sign");
        memset(sk, 0, sizeof(sk));
        return -1;
    }

    OQS_SIG *alg = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!alg) {
        snprintf(err, err_len, "OQS_SIG_new failed — is liboqs built with ML-DSA?");
        memset(sk, 0, sizeof(sk));
        return -1;
    }

    uint8_t sig_bytes[SKRAUTH_SIG_LEN];
    size_t  sig_len = SKRAUTH_SIG_LEN;
    OQS_STATUS rc = OQS_SIG_sign(alg,
                                  sig_bytes, &sig_len,
                                  (const uint8_t *)payload, (size_t)plen,
                                  sk);
    OQS_SIG_free(alg);
    memset(sk, 0, sizeof(sk));   /* scrub secret key from stack */

    if (rc != OQS_SUCCESS) {
        snprintf(err, err_len, "OQS_SIG_sign failed (status %d)", (int)rc);
        return -1;
    }

    /* Hex-encode signature */
    char hex_sig[SKRAUTH_HEXSIG_LEN + 1];
    hex_encode(sig_bytes, sig_len, hex_sig);
    hex_sig[SKRAUTH_HEXSIG_LEN] = '\0';

    /* out = "<payload>|<hex_sig>" */
    int written = snprintf(out, out_len, "%s|%s", payload, hex_sig);
    if (written <= 0 || written >= (int)out_len) {
        snprintf(err, err_len, "output buffer too small for signed command");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * skrauth_verify
 * ---------------------------------------------------------------------- */

int skrauth_verify(const char *signed_cmd, const char *pubkey_path,
                   char *cmd_out, size_t cmd_out_len) {
    size_t total = strlen(signed_cmd);

    /*
     * Layout: <cmd>|<ts>|<SKRAUTH_HEXSIG_LEN hex chars>
     * Minimum valid length: 1 + 1 + 1 + 1 + SKRAUTH_HEXSIG_LEN
     *                       cmd  |  ts  |  sig
     */
    if (total < SKRAUTH_HEXSIG_LEN + 4)
        return -1;

    /* The last SKRAUTH_HEXSIG_LEN chars are the hex signature.
     * The byte immediately before them must be '|'. */
    const char *sig_sep = signed_cmd + total - SKRAUTH_HEXSIG_LEN - 1;
    if (*sig_sep != '|')
        return -1;

    const char *hex_sig = sig_sep + 1;

    /* The signed payload is everything up to (not including) sig_sep.
     * payload = "<cmd>|<ts>" */
    size_t payload_len = (size_t)(sig_sep - signed_cmd);

    /* Find the timestamp: last '|' in the payload region */
    const char *ts_sep = NULL;
    for (const char *p = sig_sep - 1; p >= signed_cmd; p--) {
        if (*p == '|') { ts_sep = p; break; }
    }
    if (!ts_sep) return -1;

    /* Validate nonce */
    time_t ts  = (time_t)strtoll(ts_sep + 1, NULL, 10);
    time_t now = time(NULL);
    long long delta = (long long)now - (long long)ts;
    if (delta < -SKRAUTH_NONCE_WINDOW_S || delta > SKRAUTH_NONCE_WINDOW_S)
        return -1;

    /* Copy bare command (everything before ts_sep) to cmd_out */
    size_t cmd_len = (size_t)(ts_sep - signed_cmd);
    if (cmd_len == 0 || cmd_len >= cmd_out_len)
        return -1;
    memcpy(cmd_out, signed_cmd, cmd_len);
    cmd_out[cmd_len] = '\0';

    /* Decode hex signature */
    uint8_t sig_bytes[SKRAUTH_SIG_LEN];
    if (hex_decode(hex_sig, SKRAUTH_HEXSIG_LEN, sig_bytes) < 0)
        return -1;

    /* Load public key */
    uint8_t pk[SKRAUTH_PK_LEN];
    char err_buf[128];
    if (load_key(pubkey_path, pk, SKRAUTH_PK_LEN, err_buf, sizeof(err_buf)) < 0)
        return -1;

    /* Verify signature over the signed payload (cmd|ts) */
    OQS_SIG *alg = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!alg) return -1;

    OQS_STATUS rc = OQS_SIG_verify(alg,
                                    (const uint8_t *)signed_cmd, payload_len,
                                    sig_bytes, SKRAUTH_SIG_LEN,
                                    pk);
    OQS_SIG_free(alg);
    return (rc == OQS_SUCCESS) ? 0 : -1;
}
