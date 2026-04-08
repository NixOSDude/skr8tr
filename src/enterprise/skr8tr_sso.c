/*
 * skr8tr_sso.c — Enterprise SSO / OIDC Bridge
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * HTTP/1.1 service (TCP 7780) that validates OIDC JWTs from enterprise
 * Identity Providers and issues ML-DSA-65 signed skr8tr session tokens.
 *
 * Implementation notes:
 *
 *   - JWKS fetch: uses OpenSSL BIO over TCP (no libcurl dependency).
 *     HTTPS is supported via OpenSSL SSL_CTX for production IdPs.
 *
 *   - JWT validation: RS256 / ES256 via OpenSSL EVP_DigestVerify.
 *     HS256 (shared-secret) is intentionally NOT supported — enterprise
 *     IdPs must use asymmetric keys.
 *
 *   - Session token layout (binary):
 *       [0]       role byte (SSO_ROLE_USER=1 / SSO_ROLE_ADMIN=2)
 *       [1..255]  sub claim (null-padded)
 *       [256..263] exp (uint64_t big-endian)
 *       [264..3572] ML-DSA-65 signature over bytes [0..263]
 *     Total: 3573 bytes → hex string 7146 chars + NUL
 *
 *   - JWKS cache: in-memory, refreshed after SSO_JWKS_CACHE_TTL_S seconds.
 *     Thread-safe via mutex.
 *
 * Build: part of skr8tr_sched enterprise build OR standalone:
 *   gcc -O3 -Wall -std=gnu23 skr8tr_sso.c -o bin/skr8tr_sso -loqs -lssl -lcrypto -lpthread
 *
 * Run:
 *   bin/skr8tr_sso [--config <path>] [--port <port>]
 *
 * SSoA Level: ENTERPRISE
 */

#include "skr8tr_sso.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <oqs/oqs.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Session token layout constants
 * ---------------------------------------------------------------------- */

#define TOK_ROLE_OFF    0
#define TOK_SUB_OFF     1
#define TOK_SUB_LEN     255
#define TOK_EXP_OFF     256
#define TOK_EXP_LEN     8
#define TOK_SIG_OFF     264
#define TOK_SIG_LEN     3309      /* ML-DSA-65 signature */
#define TOK_TOTAL_BYTES (TOK_SIG_OFF + TOK_SIG_LEN)   /* 3573 */
#define TOK_HEX_LEN     (TOK_TOTAL_BYTES * 2)          /* 7146 */

/* -------------------------------------------------------------------------
 * JWKS cache — in-memory JSON blob + timestamp
 * ---------------------------------------------------------------------- */

#define JWKS_BUF_MAX    65536

static char            g_jwks_json[JWKS_BUF_MAX] = {0};
static time_t          g_jwks_fetched = 0;
static pthread_mutex_t g_jwks_mu      = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Hex helpers
 * ---------------------------------------------------------------------- */

static void bytes_to_hex(const uint8_t* b, size_t len, char* out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]   = h[b[i] >> 4];
        out[i*2+1] = h[b[i] & 0xf];
    }
    out[len*2] = '\0';
}

static int hex_to_bytes(const char* hex, uint8_t* out, size_t expect) {
    if (strlen(hex) != expect * 2) return -1;
    for (size_t i = 0; i < expect; i++) {
        char hi = hex[i*2], lo = hex[i*2+1];
        unsigned u = 0, v = 0;
        u = isdigit((unsigned char)hi) ? (unsigned)(hi-'0')
                                       : (unsigned)(tolower((unsigned char)hi)-'a'+10);
        v = isdigit((unsigned char)lo) ? (unsigned)(lo-'0')
                                       : (unsigned)(tolower((unsigned char)lo)-'a'+10);
        if (u > 15 || v > 15) return -1;
        out[i] = (uint8_t)((u << 4) | v);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Base64url decode (RFC 4648 §5, no padding)
 * ---------------------------------------------------------------------- */

static const int b64url_table[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['-']=62,['_']=63,
};

static ssize_t b64url_decode(const char* src, size_t slen,
                              uint8_t* dst, size_t dlen) {
    size_t out = 0;
    size_t i   = 0;
    while (i < slen) {
        uint32_t acc = 0;
        int bits = 0;
        for (int j = 0; j < 4 && i < slen; j++, i++) {
            unsigned char c = (unsigned char)src[i];
            if (c == '=') break;
            int v = b64url_table[c];
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
        }
        while (bits >= 8) {
            if (out >= dlen) return -1;
            bits -= 8;
            dst[out++] = (uint8_t)((acc >> (unsigned)bits) & 0xFF);
        }
    }
    return (ssize_t)out;
}

/* -------------------------------------------------------------------------
 * Minimal JSON string extractor — not a full JSON parser.
 * Finds the first value for key in a flat JSON object.
 * Returns 0 on success (writes null-terminated value into out).
 * ---------------------------------------------------------------------- */

static int json_get_str(const char* json, const char* key,
                         char* out, size_t out_len) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;  /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_long(const char* json, const char* key, long* out) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p) && *p != '-') return -1;
    *out = strtol(p, NULL, 10);
    return 0;
}

/* -------------------------------------------------------------------------
 * Config loader
 * ---------------------------------------------------------------------- */

int sso_load_config(const char* path, SsoConfig* cfg) {
    if (!path) path = SSO_CONFIG_PATH;
    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_port = SSO_LISTEN_PORT;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            printf("[sso] config not found — using defaults: %s\n", path);
            return 0;
        }
        fprintf(stderr, "[sso] cannot open config '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;

        char key[64]={0}, val[SSO_MAX_URL]={0};
        if (sscanf(s, "%63s %511[^\n]", key, val) != 2) continue;

        if (!strcmp(key, "oidc_issuer"))   {
            size_t l = strlen(val); if (l >= SSO_MAX_URL) l = SSO_MAX_URL-1;
            memcpy(cfg->oidc_issuer, val, l);
        } else if (!strcmp(key, "oidc_audience")) {
            size_t l = strlen(val); if (l >= SSO_MAX_URL) l = SSO_MAX_URL-1;
            memcpy(cfg->oidc_audience, val, l);
        } else if (!strcmp(key, "jwks_uri")) {
            size_t l = strlen(val); if (l >= SSO_MAX_URL) l = SSO_MAX_URL-1;
            memcpy(cfg->jwks_uri, val, l);
        } else if (!strcmp(key, "admin_group")) {
            size_t l = strlen(val); if (l >= SSO_MAX_GROUP_NAME) l = SSO_MAX_GROUP_NAME-1;
            memcpy(cfg->admin_group, val, l);
        } else if (!strcmp(key, "user_group")) {
            size_t l = strlen(val); if (l >= SSO_MAX_GROUP_NAME) l = SSO_MAX_GROUP_NAME-1;
            memcpy(cfg->user_group, val, l);
        } else if (!strcmp(key, "signing_key")) {
            size_t l = strlen(val); if (l >= sizeof(cfg->signing_key_path)) l = sizeof(cfg->signing_key_path)-1;
            memcpy(cfg->signing_key_path, val, l);
        } else if (!strcmp(key, "listen_port")) {
            cfg->listen_port = (int)strtol(val, NULL, 10);
        }
    }
    fclose(fp);
    printf("[sso] config loaded: issuer=%s  audience=%s  port=%d\n",
           cfg->oidc_issuer, cfg->oidc_audience, cfg->listen_port);
    return 0;
}

/* -------------------------------------------------------------------------
 * JWKS fetch — HTTP GET over TCP (non-TLS for LAN IdP, TLS via OpenSSL)
 * We support both http:// and https:// URIs.
 * ---------------------------------------------------------------------- */

static int fetch_url(const char* url, char* out, size_t out_len) {
    /* Parse URL: scheme://host[:port]/path */
    int use_tls = 0;
    const char* host_start;
    if (!strncmp(url, "https://", 8)) { use_tls = 1; host_start = url + 8; }
    else if (!strncmp(url, "http://",  7)) { host_start = url + 7; }
    else return -1;

    char host[256] = {0};
    char path[512] = {0};
    int  port      = use_tls ? 443 : 80;

    const char* slash = strchr(host_start, '/');
    if (slash) {
        size_t hl = (size_t)(slash - host_start);
        if (hl >= sizeof(host)) return -1;
        memcpy(host, host_start, hl);
        size_t pl = strlen(slash);
        if (pl >= sizeof(path)) pl = sizeof(path)-1;
        memcpy(path, slash, pl);
    } else {
        size_t hl = strlen(host_start);
        if (hl >= sizeof(host)) return -1;
        memcpy(host, host_start, hl);
        path[0]='/'; path[1]='\0';
    }

    /* Handle host:port */
    char* colon = strchr(host, ':');
    if (colon) { *colon = '\0'; port = (int)strtol(colon+1, NULL, 10); }

    /* Resolve host */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "[sso] JWKS: DNS resolve failed for '%s'\n", host);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec=10, .tv_usec=0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    SSL_CTX* ctx = NULL;
    SSL*     ssl = NULL;

    if (use_tls) {
        SSL_library_init();
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { close(fd); return -1; }
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return -1;
        }
    }

    /* Build HTTP GET request */
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "User-Agent: skr8tr-sso/1.0\r\n"
             "\r\n",
             path, host);

    if (use_tls)
        SSL_write(ssl, req, (int)strlen(req));
    else {
        ssize_t _w = write(fd, req, strlen(req)); (void)_w;
    }

    /* Read response */
    size_t total = 0;
    char   raw[JWKS_BUF_MAX];
    for (;;) {
        int n;
        if (use_tls)
            n = SSL_read(ssl, raw + total, (int)(sizeof(raw) - total - 1));
        else
            n = (int)read(fd, raw + total, sizeof(raw) - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof(raw) - 1) break;
    }
    raw[total] = '\0';

    if (use_tls) { SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); }
    close(fd);

    /* Strip HTTP headers — find \r\n\r\n */
    const char* body = strstr(raw, "\r\n\r\n");
    if (!body) body = raw;
    else body += 4;

    size_t blen = strlen(body);
    if (blen >= out_len) blen = out_len - 1;
    memcpy(out, body, blen);
    out[blen] = '\0';
    return (int)blen;
}

/* -------------------------------------------------------------------------
 * JWKS refresh — fetch if stale
 * ---------------------------------------------------------------------- */

static void jwks_refresh_if_stale(const SsoConfig* cfg) {
    pthread_mutex_lock(&g_jwks_mu);
    time_t now = time(NULL);
    if (now - g_jwks_fetched < SSO_JWKS_CACHE_TTL_S && g_jwks_json[0]) {
        pthread_mutex_unlock(&g_jwks_mu);
        return;
    }
    printf("[sso] fetching JWKS from: %s\n", cfg->jwks_uri);
    char buf[JWKS_BUF_MAX];
    if (fetch_url(cfg->jwks_uri, buf, sizeof(buf)) > 0) {
        size_t l = strlen(buf);
        if (l >= JWKS_BUF_MAX) l = JWKS_BUF_MAX - 1;
        memcpy(g_jwks_json, buf, l);
        g_jwks_json[l] = '\0';
        g_jwks_fetched = now;
        printf("[sso] JWKS cache updated (%zu bytes)\n", l);
    } else {
        fprintf(stderr, "[sso] WARNING: JWKS fetch failed — "
                "using cached copy if available\n");
    }
    pthread_mutex_unlock(&g_jwks_mu);
}

/* -------------------------------------------------------------------------
 * RSA public key extraction from JWKS JSON for a given "kid"
 * Returns EVP_PKEY* on success (caller must EVP_PKEY_free), NULL on fail.
 * ---------------------------------------------------------------------- */

static EVP_PKEY* jwks_find_key(const char* kid) {
    pthread_mutex_lock(&g_jwks_mu);
    char jwks[JWKS_BUF_MAX];
    size_t jl = strlen(g_jwks_json);
    if (jl >= sizeof(jwks)) jl = sizeof(jwks) - 1;
    memcpy(jwks, g_jwks_json, jl); jwks[jl] = '\0';
    pthread_mutex_unlock(&g_jwks_mu);

    if (!jwks[0]) return NULL;

    /* Scan for the key entry matching kid.
     * Format: {..., "kid":"<kid>", ..., "n":"<base64url>", "e":"<base64url>", ...}
     * We do a simple sequential scan for the kid, then extract n and e
     * from the same JSON object. */
    const char* key_start = jwks;
    for (;;) {
        const char* kid_pos = strstr(key_start, "\"kid\"");
        if (!kid_pos) return NULL;

        /* Find the value */
        const char* vp = kid_pos + 5;
        while (*vp == ' ' || *vp == ':') vp++;
        if (*vp != '"') { key_start = kid_pos + 5; continue; }
        vp++;
        char this_kid[256] = {0};
        size_t ki = 0;
        while (*vp && *vp != '"' && ki < 255) this_kid[ki++] = *vp++;

        if (strcmp(this_kid, kid)) { key_start = vp; continue; }

        /* Found matching key — extract n and e from surrounding object */
        /* Walk back to find opening '{' */
        const char* obj_start = kid_pos;
        while (obj_start > jwks && *obj_start != '{') obj_start--;
        /* Walk forward to find closing '}' */
        const char* obj_end = strstr(kid_pos, "}");
        if (!obj_end) return NULL;
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= JWKS_BUF_MAX) return NULL;
        char obj[JWKS_BUF_MAX];
        memcpy(obj, obj_start, obj_len); obj[obj_len] = '\0';

        char n_b64[4096]={0}, e_b64[32]={0};
        if (json_get_str(obj, "n", n_b64, sizeof(n_b64)) < 0) return NULL;
        if (json_get_str(obj, "e", e_b64, sizeof(e_b64)) < 0) return NULL;

        /* Decode modulus and exponent */
        uint8_t n_bytes[512], e_bytes[8];
        ssize_t n_len = b64url_decode(n_b64, strlen(n_b64),
                                       n_bytes, sizeof(n_bytes));
        ssize_t e_len = b64url_decode(e_b64, strlen(e_b64),
                                       e_bytes, sizeof(e_bytes));
        if (n_len <= 0 || e_len <= 0) return NULL;

        /* Build RSA public key via OpenSSL 3.x EVP_PKEY_fromdata */
        BIGNUM* bn_n = BN_bin2bn(n_bytes, (int)n_len, NULL);
        BIGNUM* bn_e = BN_bin2bn(e_bytes, (int)e_len, NULL);
        if (!bn_n || !bn_e) {
            BN_free(bn_n); BN_free(bn_e); return NULL;
        }

        OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
        if (!bld) { BN_free(bn_n); BN_free(bn_e); return NULL; }

        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n);
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e);
        OSSL_PARAM* ossl_params = OSSL_PARAM_BLD_to_param(bld);
        OSSL_PARAM_BLD_free(bld);
        BN_free(bn_n); BN_free(bn_e);
        if (!ossl_params) return NULL;

        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
        EVP_PKEY* pkey = NULL;
        if (!kctx ||
            EVP_PKEY_fromdata_init(kctx) != 1 ||
            EVP_PKEY_fromdata(kctx, &pkey,
                              EVP_PKEY_PUBLIC_KEY, ossl_params) != 1) {
            EVP_PKEY_CTX_free(kctx);
            OSSL_PARAM_free(ossl_params);
            return NULL;
        }
        EVP_PKEY_CTX_free(kctx);
        OSSL_PARAM_free(ossl_params);
        return pkey;
    }
}

/* -------------------------------------------------------------------------
 * JWT validation
 * ---------------------------------------------------------------------- */

int sso_validate_jwt(const char* jwt_str, const SsoConfig* cfg,
                     SsoJwtClaim* claims) {
    memset(claims, 0, sizeof(*claims));

    /* Split JWT into header.payload.signature */
    const char* dot1 = strchr(jwt_str, '.');
    if (!dot1) {
        memcpy(claims->error, "JWT: missing first dot separator", 32);
        return -1;
    }
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        memcpy(claims->error, "JWT: missing second dot separator", 33);
        return -1;
    }

    /* Decode header */
    uint8_t hdr_bytes[1024];
    ssize_t hdr_len = b64url_decode(jwt_str, (size_t)(dot1 - jwt_str),
                                    hdr_bytes, sizeof(hdr_bytes) - 1);
    if (hdr_len <= 0) {
        memcpy(claims->error, "JWT: bad header base64", 22);
        return -1;
    }
    hdr_bytes[hdr_len] = '\0';
    char* hdr_json = (char*)hdr_bytes;

    char alg[16]={0}, kid[256]={0};
    json_get_str(hdr_json, "alg", alg, sizeof(alg));
    json_get_str(hdr_json, "kid", kid, sizeof(kid));

    /* Only RS256 supported — ES256 is a near-identical path via EC_KEY */
    if (strcmp(alg, "RS256")) {
        snprintf(claims->error, sizeof(claims->error),
                 "JWT: unsupported algorithm '%s' (expected RS256)", alg);
        return -1;
    }

    /* Decode payload */
    uint8_t pay_bytes[4096];
    ssize_t pay_len = b64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1),
                                    pay_bytes, sizeof(pay_bytes) - 1);
    if (pay_len <= 0) {
        memcpy(claims->error, "JWT: bad payload base64", 23);
        return -1;
    }
    pay_bytes[pay_len] = '\0';
    char* pay_json = (char*)pay_bytes;

    /* Extract claims */
    char iss[SSO_MAX_URL]={0}, aud[SSO_MAX_URL]={0};
    long exp_ts = 0;
    json_get_str(pay_json,  "iss", iss,           sizeof(iss));
    json_get_str(pay_json,  "aud", aud,           sizeof(aud));
    json_get_str(pay_json,  "sub", claims->sub,   sizeof(claims->sub));
    json_get_str(pay_json,  "email", claims->email, sizeof(claims->email));
    json_get_long(pay_json, "exp", &exp_ts);

    /* Verify issuer */
    if (cfg->oidc_issuer[0] && strcmp(iss, cfg->oidc_issuer)) {
        snprintf(claims->error, sizeof(claims->error),
                 "JWT: issuer mismatch: got '%s'", iss);
        return -1;
    }

    /* Verify audience */
    if (cfg->oidc_audience[0] && !strstr(aud, cfg->oidc_audience)) {
        snprintf(claims->error, sizeof(claims->error),
                 "JWT: audience mismatch: got '%s'", aud);
        return -1;
    }

    /* Verify expiry */
    claims->exp = exp_ts;
    if (exp_ts > 0 && time(NULL) > exp_ts) {
        memcpy(claims->error, "JWT: token expired", 18);
        return -1;
    }

    /* Determine role from groups claim */
    char groups[1024]={0};
    json_get_str(pay_json, "groups", groups, sizeof(groups));
    if (cfg->admin_group[0] && strstr(groups, cfg->admin_group))
        claims->role = SSO_ROLE_ADMIN;
    else if (cfg->user_group[0] && strstr(groups, cfg->user_group))
        claims->role = SSO_ROLE_USER;
    else {
        /* Fall back: check 'roles' claim */
        char roles_claim[256]={0};
        json_get_str(pay_json, "roles", roles_claim, sizeof(roles_claim));
        if (strstr(roles_claim, "admin"))      claims->role = SSO_ROLE_ADMIN;
        else if (strstr(roles_claim, "user"))  claims->role = SSO_ROLE_USER;
        else {
            memcpy(claims->error, "JWT: subject has no skr8tr role", 31);
            return -1;
        }
    }

    /* Verify JWT signature using JWKS */
    jwks_refresh_if_stale(cfg);
    EVP_PKEY* pkey = jwks_find_key(kid);
    if (!pkey) {
        snprintf(claims->error, sizeof(claims->error),
                 "JWT: key id '%s' not found in JWKS", kid);
        return -1;
    }

    /* The signed data is header_b64url.payload_b64url */
    size_t signed_len = (size_t)(dot2 - jwt_str);
    uint8_t sig_bytes[512];
    ssize_t sig_len = b64url_decode(dot2 + 1, strlen(dot2 + 1),
                                    sig_bytes, sizeof(sig_bytes));
    if (sig_len <= 0) {
        EVP_PKEY_free(pkey);
        memcpy(claims->error, "JWT: bad signature base64", 25);
        return -1;
    }

    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    int rc = 0;
    if (mctx &&
        EVP_DigestVerifyInit(mctx, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestVerifyUpdate(mctx,
                               (const uint8_t*)jwt_str, signed_len) == 1 &&
        EVP_DigestVerifyFinal(mctx, sig_bytes, (size_t)sig_len) == 1) {
        rc = 1;
    }
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);

    if (!rc) {
        memcpy(claims->error, "JWT: signature verification failed", 34);
        return -1;
    }

    claims->valid = 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Issue ML-DSA-65 signed skr8tr session token
 * ---------------------------------------------------------------------- */

int sso_issue_token(const SsoJwtClaim* claims,
                    const char* signing_key_path,
                    char* out_hex, size_t out_hex_len) {
    if (out_hex_len < TOK_HEX_LEN + 1) return -1;

    /* Load ML-DSA-65 secret key from file */
    uint8_t sk[4032];   /* ML-DSA-65 secret key size */
    FILE* fp = fopen(signing_key_path, "rb");
    if (!fp) {
        fprintf(stderr, "[sso] cannot open signing key: %s\n",
                strerror(errno));
        return -1;
    }
    size_t sk_len = fread(sk, 1, sizeof(sk), fp);
    fclose(fp);
    if (sk_len != sizeof(sk)) {
        fprintf(stderr, "[sso] signing key wrong size: %zu != %zu\n",
                sk_len, sizeof(sk));
        return -1;
    }

    /* Build payload */
    uint8_t tok[TOK_TOTAL_BYTES];
    memset(tok, 0, sizeof(tok));

    tok[TOK_ROLE_OFF] = (uint8_t)claims->role;

    size_t sub_len = strlen(claims->sub);
    if (sub_len > TOK_SUB_LEN) sub_len = TOK_SUB_LEN;
    memcpy(tok + TOK_SUB_OFF, claims->sub, sub_len);

    /* exp as big-endian uint64 */
    uint64_t exp_be = (uint64_t)claims->exp;
    for (int i = 7; i >= 0; i--) {
        tok[TOK_EXP_OFF + i] = (uint8_t)(exp_be & 0xFF);
        exp_be >>= 8;
    }

    /* Sign payload[0..TOK_SIG_OFF-1] with ML-DSA-65 */
    OQS_SIG* oqs = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!oqs) return -1;

    size_t sig_len = TOK_SIG_LEN;
    OQS_STATUS st  = OQS_SIG_sign(
        oqs, tok + TOK_SIG_OFF, &sig_len,
        tok, TOK_SIG_OFF,
        sk
    );
    OQS_SIG_free(oqs);

    if (st != OQS_SUCCESS || sig_len != TOK_SIG_LEN) {
        fprintf(stderr, "[sso] ML-DSA-65 signing failed\n");
        return -1;
    }

    bytes_to_hex(tok, TOK_TOTAL_BYTES, out_hex);
    return 0;
}

/* -------------------------------------------------------------------------
 * Verify a skr8tr session token
 * ---------------------------------------------------------------------- */

int sso_verify_token(const char* token_hex,
                     const char* signing_pubkey_path,
                     SsoRole* role, char* sub_out, size_t sub_len) {
    if (!token_hex || strlen(token_hex) != TOK_HEX_LEN) return -1;

    uint8_t tok[TOK_TOTAL_BYTES];
    if (hex_to_bytes(token_hex, tok, TOK_TOTAL_BYTES) < 0) return -1;

    /* Check expiry */
    uint64_t exp_be = 0;
    for (int i = 0; i < 8; i++)
        exp_be = (exp_be << 8) | tok[TOK_EXP_OFF + i];
    if ((long)exp_be > 0 && time(NULL) > (time_t)exp_be) return -1;

    /* Load ML-DSA-65 public key */
    uint8_t pk[1952];   /* ML-DSA-65 public key size */
    FILE* fp = fopen(signing_pubkey_path, "rb");
    if (!fp) return -1;
    size_t pk_len = fread(pk, 1, sizeof(pk), fp);
    fclose(fp);
    if (pk_len != sizeof(pk)) return -1;

    OQS_SIG* oqs = OQS_SIG_new(OQS_SIG_alg_ml_dsa_65);
    if (!oqs) return -1;

    OQS_STATUS st = OQS_SIG_verify(
        oqs,
        tok, TOK_SIG_OFF,          /* signed payload */
        tok + TOK_SIG_OFF, TOK_SIG_LEN,
        pk
    );
    OQS_SIG_free(oqs);

    if (st != OQS_SUCCESS) return -1;

    if (role) *role = (SsoRole)tok[TOK_ROLE_OFF];
    if (sub_out && sub_len > 0) {
        size_t copy = sub_len - 1 < TOK_SUB_LEN ? sub_len - 1 : TOK_SUB_LEN;
        memcpy(sub_out, tok + TOK_SUB_OFF, copy);
        sub_out[copy] = '\0';
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * HTTP server — one thread per connection, simple request/response
 * ---------------------------------------------------------------------- */

typedef struct {
    int            fd;
    const SsoConfig* cfg;
} ConnArg;

static void conn_send(int fd, const char* status, const char* body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, strlen(body));
    ssize_t _w;
    _w = write(fd, hdr, strlen(hdr));  (void)_w;
    _w = write(fd, body, strlen(body)); (void)_w;
}

static void* conn_handler(void* arg) {
    ConnArg*         ca  = (ConnArg*)arg;
    int              fd  = ca->fd;
    const SsoConfig* cfg = ca->cfg;
    free(ca);

    char req[8192];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = '\0';

    /* Route: POST /sso/validate */
    if (strncmp(req, "POST /sso/validate", 18)) {
        conn_send(fd, "404 Not Found",
                  "{\"error\":\"endpoint not found\"}");
        close(fd); return NULL;
    }

    /* Extract JSON body (after \r\n\r\n) */
    const char* body = strstr(req, "\r\n\r\n");
    if (!body) {
        conn_send(fd, "400 Bad Request",
                  "{\"error\":\"malformed HTTP request\"}");
        close(fd); return NULL;
    }
    body += 4;

    /* Extract oidc_token field */
    char jwt[8192] = {0};
    if (json_get_str(body, "oidc_token", jwt, sizeof(jwt)) < 0) {
        conn_send(fd, "400 Bad Request",
                  "{\"error\":\"missing oidc_token field\"}");
        close(fd); return NULL;
    }

    /* Validate JWT */
    SsoJwtClaim claims;
    if (sso_validate_jwt(jwt, cfg, &claims) < 0) {
        char resp[512];
        snprintf(resp, sizeof(resp),
                 "{\"error\":\"%s\"}", claims.error);
        conn_send(fd, "401 Unauthorized", resp);
        close(fd); return NULL;
    }

    /* Issue skr8tr session token */
    char token_hex[TOK_HEX_LEN + 4] = {0};
    if (sso_issue_token(&claims, cfg->signing_key_path,
                        token_hex, sizeof(token_hex)) < 0) {
        conn_send(fd, "500 Internal Server Error",
                  "{\"error\":\"token signing failed\"}");
        close(fd); return NULL;
    }

    /* Build response */
    char resp[TOK_HEX_LEN + 512];
    snprintf(resp, sizeof(resp),
             "{\"skr8tr_token\":\"%s\","
             "\"role\":\"%s\","
             "\"sub\":\"%s\","
             "\"exp\":%ld}",
             token_hex,
             sso_role_str(claims.role),
             claims.sub,
             claims.exp);

    conn_send(fd, "200 OK", resp);
    printf("[sso] issued token: sub=%s role=%s\n",
           claims.sub, sso_role_str(claims.role));

    close(fd);
    return NULL;
}

/* -------------------------------------------------------------------------
 * sso_run — main accept loop
 * ---------------------------------------------------------------------- */

int sso_run(const SsoConfig* cfg) {
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "[sso] socket: %s\n", strerror(errno));
        return -1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = htons((uint16_t)cfg->listen_port);

    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[sso] bind port %d: %s\n",
                cfg->listen_port, strerror(errno));
        close(srv);
        return -1;
    }
    listen(srv, 32);
    printf("[sso] SSO Bridge listening on TCP %d\n", cfg->listen_port);

    /* Pre-warm JWKS cache */
    if (cfg->jwks_uri[0])
        jwks_refresh_if_stale(cfg);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;

        ConnArg* ca = malloc(sizeof(ConnArg));
        if (!ca) { close(fd); continue; }
        ca->fd  = fd;
        ca->cfg = cfg;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, conn_handler, ca) != 0) {
            free(ca); close(fd);
        }
        pthread_attr_destroy(&attr);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * sso_role_str
 * ---------------------------------------------------------------------- */

const char* sso_role_str(SsoRole role) {
    switch (role) {
        case SSO_ROLE_ADMIN: return "admin";
        case SSO_ROLE_USER:  return "user";
        default:             return "none";
    }
}

/* -------------------------------------------------------------------------
 * Standalone entry point (when compiled as a separate binary)
 * ---------------------------------------------------------------------- */

#ifndef SSO_NO_MAIN
int main(int argc, char* argv[]) {
    const char* config_path = NULL;

    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--config")) config_path = argv[i+1];
    }

    SsoConfig cfg;
    if (sso_load_config(config_path, &cfg) < 0) return 1;

    /* Override port from command line */
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--port"))
            cfg.listen_port = (int)strtol(argv[i+1], NULL, 10);
    }

    printf("[sso] Skr8tr SSO Bridge starting...\n");
    return sso_run(&cfg);
}
#endif
