/*
 * skr8tr_sso.h — Enterprise SSO / OIDC Bridge — Public API
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Validates OIDC bearer tokens issued by an enterprise Identity Provider
 * (Okta, Azure AD, Google Workspace, Keycloak) and issues ML-DSA-65
 * signed skr8trpass-compatible session tokens.
 *
 * Architecture:
 *
 *   Browser / LambBook UI
 *       │
 *       │  POST /sso/validate   { "oidc_token": "<JWT>" }
 *       ▼
 *   skr8tr_sso (HTTP/1.1 on TCP port 7780)
 *       │  1. Fetch JWKS from IdP (cached, TTL 300s)
 *       │  2. Verify JWT signature (RSA-256 or EC-256)
 *       │  3. Check issuer, audience, expiry claims
 *       │  4. Map groups/roles claim → skr8tr role (user/admin)
 *       │  5. Issue ML-DSA-65 signed session token
 *       ▼
 *   Response: { "skr8tr_token": "<base64-session-token>" }
 *
 * The skr8tr_token is forwarded to the Shepherd Gateway which validates
 * it against the SSO signing key before granting WebSocket access.
 *
 * Config file (/etc/skr8tr/sso.conf):
 *   oidc_issuer     https://accounts.google.com
 *   oidc_audience   skr8tr.company.com
 *   jwks_uri        https://www.googleapis.com/oauth2/v3/certs
 *   admin_group     skr8tr-admins
 *   user_group      skr8tr-users
 *   signing_key     /etc/skr8tr/sso_signing.sec
 *   listen_port     7780
 *
 * Wire format (HTTP/1.1):
 *   POST /sso/validate HTTP/1.1
 *   Content-Type: application/json
 *   { "oidc_token": "<JWT>" }
 *
 *   200 OK:
 *   { "skr8tr_token": "<hex>", "role": "admin|user", "sub": "<email>", "exp": <unix_ts> }
 *
 *   401 Unauthorized:
 *   { "error": "<reason>" }
 *
 * SSoA Level: ENTERPRISE
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define SSO_LISTEN_PORT         7780
#define SSO_CONFIG_PATH         "/etc/skr8tr/sso.conf"
#define SSO_TOKEN_TTL_S         3600    /* session token validity: 1 hour */
#define SSO_JWKS_CACHE_TTL_S    300     /* JWKS refresh interval: 5 minutes */
#define SSO_MAX_GROUPS          32
#define SSO_MAX_URL             512
#define SSO_MAX_GROUP_NAME      128
#define SSO_TOKEN_LEN           256     /* skr8tr session token payload bytes */

/* ── Role ───────────────────────────────────────────────────────────── */

typedef enum {
    SSO_ROLE_NONE  = 0,
    SSO_ROLE_USER  = 1,
    SSO_ROLE_ADMIN = 2,
} SsoRole;

/* ── Config ─────────────────────────────────────────────────────────── */

typedef struct {
    char oidc_issuer[SSO_MAX_URL];
    char oidc_audience[SSO_MAX_URL];
    char jwks_uri[SSO_MAX_URL];
    char admin_group[SSO_MAX_GROUP_NAME];
    char user_group[SSO_MAX_GROUP_NAME];
    char signing_key_path[512];
    int  listen_port;
} SsoConfig;

/* ── JWT validation result ──────────────────────────────────────────── */

typedef struct {
    char     sub[256];     /* "sub" claim: user identifier / email */
    char     email[256];   /* "email" claim (optional) */
    SsoRole  role;         /* mapped from groups claim */
    long     exp;          /* "exp" claim: token expiry unix timestamp */
    int      valid;        /* 1 if validation succeeded */
    char     error[256];   /* human-readable reason if !valid */
} SsoJwtClaim;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/* Load config from path (NULL → SSO_CONFIG_PATH).
 * Returns 0 on success, -1 on error. */
int  sso_load_config(const char* path, SsoConfig* cfg);

/* Start the SSO HTTP listener.  Blocks — run in a dedicated thread.
 * Returns only on fatal error. */
int  sso_run(const SsoConfig* cfg);

/* ── JWT operations ─────────────────────────────────────────────────── */

/* Validate an OIDC JWT string.  Fetches JWKS if cache is stale.
 * Populates *claims on success.
 * Returns 0 on success, -1 on validation failure. */
int  sso_validate_jwt(const char* jwt_str, const SsoConfig* cfg,
                      SsoJwtClaim* claims);

/* Issue a skr8tr session token from validated claims.
 * The token is a binary blob: [ role(1) | sub(255) | exp(8) | sig(3309) ]
 * Writes hex-encoded token into out_hex (must be ≥ SSO_TOKEN_LEN*2+8 bytes).
 * Returns 0 on success, -1 on signing failure. */
int  sso_issue_token(const SsoJwtClaim* claims, const char* signing_key_path,
                     char* out_hex, size_t out_hex_len);

/* Verify a skr8tr session token (for use in Shepherd Gateway).
 * Returns 0 on success and populates *role and sub_out.
 * Returns -1 on invalid/expired token. */
int  sso_verify_token(const char* token_hex, const char* signing_pubkey_path,
                      SsoRole* role, char* sub_out, size_t sub_len);

/* Human-readable role name */
const char* sso_role_str(SsoRole role);
