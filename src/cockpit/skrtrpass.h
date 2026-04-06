/*
 * SSoA LEVEL 1: FOUNDATION ANCHOR
 * FILE: src/cockpit/skrtrpass.h
 * MISSION: SkrtrPass — ML-DSA-65 sovereign access tokens for Skr8trView.
 *          Defines the token format, verify logic, and role constants.
 *          All cockpit authentication flows through this header. Immutable law.
 *
 * TOKEN FORMAT (pipe-delimited ASCII):
 *   <role>|<user>|<expiry_unix>|<hex_signature>
 *
 *   role        : "operator" (read-only) or "admin" (full control)
 *   user        : alphanumeric identity label (max 64 chars)
 *   expiry_unix : Unix timestamp (uint64 as decimal string)
 *   hex_sig     : ML-DSA-65 signature over "<role>|<user>|<expiry_unix>"
 *                 encoded as lowercase hex (3309 bytes → 6618 hex chars)
 *
 * VERIFICATION:
 *   skrtrpass_verify(token, pubkey_path) →
 *     SKRTRPASS_OK_OPERATOR | SKRTRPASS_OK_ADMIN | SKRTRPASS_ERR_*
 *
 * MINTING (gen_skrtrpass tool only):
 *   skrtrpass_mint(role, user, ttl_secs, seckey_path, out_token, out_len)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <oqs/oqs.h>
#include <oqs/sig_ml_dsa.h>

/* -------------------------------------------------------------------------
 * Role constants
 * ---------------------------------------------------------------------- */

#define SKRTRPASS_ROLE_OPERATOR  1   /* read-only: NODES, LIST, SERVICES, LOGS */
#define SKRTRPASS_ROLE_ADMIN     2   /* full: + SUBMIT, EVICT, node management */

/* -------------------------------------------------------------------------
 * Return codes
 * ---------------------------------------------------------------------- */

#define SKRTRPASS_OK_OPERATOR    SKRTRPASS_ROLE_OPERATOR
#define SKRTRPASS_OK_ADMIN       SKRTRPASS_ROLE_ADMIN
#define SKRTRPASS_ERR_PARSE     -1   /* token format malformed */
#define SKRTRPASS_ERR_EXPIRED   -2   /* expiry timestamp in the past */
#define SKRTRPASS_ERR_PUBKEY    -3   /* cannot load public key file */
#define SKRTRPASS_ERR_BADSIG    -4   /* ML-DSA-65 signature invalid */
#define SKRTRPASS_ERR_ROLE      -5   /* unknown role string */

/* -------------------------------------------------------------------------
 * Key / signature sizes
 * ---------------------------------------------------------------------- */

#define SKRTRPASS_PUBKEY_LEN   OQS_SIG_ml_dsa_65_length_public_key    /* 1952 */
#define SKRTRPASS_SECKEY_LEN   OQS_SIG_ml_dsa_65_length_secret_key    /* 4032 */
#define SKRTRPASS_SIG_LEN      OQS_SIG_ml_dsa_65_length_signature     /* 3309 */
#define SKRTRPASS_SIG_HEX_LEN  (SKRTRPASS_SIG_LEN * 2)                /* 6618 */

/* Maximum total token length:
 *   "admin|" + 64 + "|" + 20 + "|" + 6618 + NUL ≈ 6712 */
#define SKRTRPASS_TOKEN_MAX    7000

/* -------------------------------------------------------------------------
 * Internal: hex decode
 * Returns bytes decoded, -1 on invalid hex char.
 * ---------------------------------------------------------------------- */

static inline int skrtrpass_hex_decode(const char* hex, uint8_t* out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        char hi = hex[2*i];
        char lo = hex[2*i + 1];
        unsigned int hi_v, lo_v;
        if      (hi >= '0' && hi <= '9') hi_v = (unsigned)(hi - '0');
        else if (hi >= 'a' && hi <= 'f') hi_v = (unsigned)(hi - 'a' + 10);
        else if (hi >= 'A' && hi <= 'F') hi_v = (unsigned)(hi - 'A' + 10);
        else return -1;
        if      (lo >= '0' && lo <= '9') lo_v = (unsigned)(lo - '0');
        else if (lo >= 'a' && lo <= 'f') lo_v = (unsigned)(lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') lo_v = (unsigned)(lo - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)((hi_v << 4) | lo_v);
    }
    return (int)out_len;
}

/* -------------------------------------------------------------------------
 * Internal: hex encode
 * ---------------------------------------------------------------------- */

static inline void skrtrpass_hex_encode(const uint8_t* in, size_t in_len,
                                        char* out, size_t out_size) {
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len && (2*i + 2) < out_size; i++) {
        out[2*i]     = HEX[(in[i] >> 4) & 0xf];
        out[2*i + 1] = HEX[in[i] & 0xf];
    }
    out[in_len * 2] = '\0';
}

/* -------------------------------------------------------------------------
 * skrtrpass_verify — parse and verify a SkrtrPass token.
 *
 * Parameters:
 *   token       : NUL-terminated token string (format described above)
 *   pubkey_path : path to the 1952-byte raw binary public key file
 *
 * Returns: SKRTRPASS_OK_OPERATOR, SKRTRPASS_OK_ADMIN, or SKRTRPASS_ERR_*
 *
 * Side-effects: none (pure verify, no state mutation)
 * ---------------------------------------------------------------------- */

static inline int skrtrpass_verify(const char* token, const char* pubkey_path) {
    /* --- Parse: split into 4 fields on '|' --- */
    char buf[SKRTRPASS_TOKEN_MAX];
    if (!token || strlen(token) >= SKRTRPASS_TOKEN_MAX) return SKRTRPASS_ERR_PARSE;
    strncpy(buf, token, SKRTRPASS_TOKEN_MAX - 1);
    buf[SKRTRPASS_TOKEN_MAX - 1] = '\0';

    char* fields[4];
    char* p = buf;
    for (int i = 0; i < 4; i++) {
        fields[i] = p;
        if (i < 3) {
            p = strchr(p, '|');
            if (!p) return SKRTRPASS_ERR_PARSE;
            *p++ = '\0';
        }
    }

    const char* role_str   = fields[0];
    const char* user_str   = fields[1];
    const char* expiry_str = fields[2];
    const char* hex_sig    = fields[3];

    /* --- Validate role --- */
    int role;
    if      (!strcmp(role_str, "operator")) role = SKRTRPASS_ROLE_OPERATOR;
    else if (!strcmp(role_str, "admin"))    role = SKRTRPASS_ROLE_ADMIN;
    else return SKRTRPASS_ERR_ROLE;

    (void)user_str; /* carried in payload for logging, not validated here */

    /* --- Check expiry --- */
    uint64_t expiry = (uint64_t)strtoull(expiry_str, NULL, 10);
    if (expiry < (uint64_t)time(NULL)) return SKRTRPASS_ERR_EXPIRED;

    /* --- Load public key --- */
    uint8_t pubkey[SKRTRPASS_PUBKEY_LEN];
    FILE* f = fopen(pubkey_path, "rb");
    if (!f) return SKRTRPASS_ERR_PUBKEY;
    size_t n = fread(pubkey, 1, SKRTRPASS_PUBKEY_LEN, f);
    fclose(f);
    if (n != SKRTRPASS_PUBKEY_LEN) return SKRTRPASS_ERR_PUBKEY;

    /* --- Decode hex signature --- */
    uint8_t sig[SKRTRPASS_SIG_LEN];
    if (skrtrpass_hex_decode(hex_sig, sig, SKRTRPASS_SIG_LEN) < 0)
        return SKRTRPASS_ERR_BADSIG;

    /* --- Reconstruct signed message: "<role>|<user>|<expiry>" --- */
    char msg[256];
    int msg_len = snprintf(msg, sizeof(msg), "%s|%s|%s",
                           role_str, user_str, expiry_str);
    if (msg_len < 0 || (size_t)msg_len >= sizeof(msg))
        return SKRTRPASS_ERR_PARSE;

    /* --- ML-DSA-65 verify --- */
    OQS_STATUS rc = OQS_SIG_ml_dsa_65_verify(
        (const uint8_t*)msg, (size_t)msg_len,
        sig, SKRTRPASS_SIG_LEN,
        pubkey
    );

    if (rc != OQS_SUCCESS) return SKRTRPASS_ERR_BADSIG;
    return role;
}

/* -------------------------------------------------------------------------
 * skrtrpass_mint — sign and emit a SkrtrPass token.
 *
 * Parameters:
 *   role        : "operator" or "admin"
 *   user        : identity label (max 64 chars, alphanumeric)
 *   ttl_secs    : token lifetime in seconds from now
 *   seckey_path : path to the 4032-byte raw binary secret key file
 *   out         : output buffer for the complete token string
 *   out_len     : size of out buffer (must be >= SKRTRPASS_TOKEN_MAX)
 *
 * Returns: 0 on success, negative on error.
 * ---------------------------------------------------------------------- */

static inline int skrtrpass_mint(const char* role, const char* user,
                                 uint64_t ttl_secs, const char* seckey_path,
                                 char* out, size_t out_len) {
    if (!role || !user || !seckey_path || !out || out_len < SKRTRPASS_TOKEN_MAX)
        return -1;
    if (strcmp(role, "operator") && strcmp(role, "admin"))
        return SKRTRPASS_ERR_ROLE;

    /* Load secret key */
    uint8_t seckey[SKRTRPASS_SECKEY_LEN];
    FILE* f = fopen(seckey_path, "rb");
    if (!f) return SKRTRPASS_ERR_PUBKEY;
    size_t n = fread(seckey, 1, SKRTRPASS_SECKEY_LEN, f);
    fclose(f);
    if (n != SKRTRPASS_SECKEY_LEN) return SKRTRPASS_ERR_PUBKEY;

    uint64_t expiry = (uint64_t)time(NULL) + ttl_secs;

    /* Build signed message */
    char msg[256];
    int msg_len = snprintf(msg, sizeof(msg), "%s|%s|%llu",
                           role, user, (unsigned long long)expiry);
    if (msg_len < 0 || (size_t)msg_len >= sizeof(msg)) return -1;

    /* Sign */
    uint8_t sig[SKRTRPASS_SIG_LEN];
    size_t  sig_len = SKRTRPASS_SIG_LEN;
    OQS_STATUS rc = OQS_SIG_ml_dsa_65_sign(
        sig, &sig_len,
        (const uint8_t*)msg, (size_t)msg_len,
        seckey
    );
    if (rc != OQS_SUCCESS) return -1;

    /* Hex-encode sig */
    char hex_sig[SKRTRPASS_SIG_HEX_LEN + 1];
    skrtrpass_hex_encode(sig, sig_len, hex_sig, sizeof(hex_sig));

    /* Assemble token */
    snprintf(out, out_len, "%s|%s|%llu|%s",
             role, user, (unsigned long long)expiry, hex_sig);
    return 0;
}
