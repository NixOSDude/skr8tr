/*
 * skr8tr_rbac.h — Enterprise RBAC Gateway — Public API
 * Skr8tr Sovereign Workload Orchestrator — Enterprise Module
 *
 * PROPRIETARY AND CONFIDENTIAL
 * Copyright 2026 Scott Baker. All rights reserved.
 *
 * Role-based access control gateway for the Skr8tr Conductor.
 * Sits in front of skr8tr_sched (Conductor) on UDP port 7773.
 *
 * Team model:
 *   Each team has one ML-DSA-65 public key registered in the flat-file registry.
 *   Teams are scoped to a namespace — they cannot see or touch other namespaces.
 *   Each team has a permissions bitmask controlling which commands they may issue.
 *
 * Wire format (client → RBAC gateway, UDP 7773):
 *   RBAC|<team>|<namespace>|<bare_command>|<unix_ts>|<sig_hex>
 *
 *   Signed payload (what ML-DSA-65 sig covers):
 *     "<team>|<namespace>|<bare_command>|<unix_ts>"
 *
 * Admin wire format:
 *   RBAC_ADMIN|<admin_team>|TEAM_ADD|<name>|<ns>|<perms_hex>|<pubkey_hex>|<ts>|<sig>
 *   RBAC_ADMIN|<admin_team>|TEAM_REVOKE|<target_team>|<ts>|<sig>
 *   RBAC_ADMIN|<admin_team>|TEAM_LIST|<ts>|<sig>
 *
 * Registry file format (one team per line):
 *   # comment
 *   <team_name>:<namespace>:<perms_hex>:<pubkey_hex>
 *
 * Namespace enforcement:
 *   SUBMIT, EVICT, ROLLOUT commands are namespaced:
 *     the bare_command is prefixed with "<namespace>." before forwarding.
 *   NODES, PING, LIST, LOGS, LOOKUP pass through as-is.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Permission flags ───────────────────────────────────────────── */

#define RBAC_PERM_READ      0x01   /* NODES, LIST, PING, LOOKUP, LOGS */
#define RBAC_PERM_SUBMIT    0x02   /* SUBMIT */
#define RBAC_PERM_EVICT     0x04   /* EVICT */
#define RBAC_PERM_ROLLOUT   0x08   /* ROLLOUT */
#define RBAC_PERM_EXEC      0x10   /* EXEC */
#define RBAC_PERM_ADMIN     0x80   /* TEAM_ADD, TEAM_REVOKE, TEAM_LIST */
#define RBAC_PERM_ALL       0xFF

/* ── Constants ─────────────────────────────────────────────────── */

#define RBAC_PK_LEN          1952   /* ML-DSA-65 public key bytes */
#define RBAC_SIG_LEN         3309   /* ML-DSA-65 signature bytes */
#define RBAC_HEXSIG_LEN      6618   /* signature hex on the wire */
#define RBAC_HEXKEY_LEN      3904   /* pubkey hex in registry file */
#define RBAC_MAX_TEAMS       64
#define RBAC_MAX_NAME        64
#define RBAC_NONCE_WINDOW_S  30     /* replay protection window */
#define RBAC_GATEWAY_PORT    7773
#define RBAC_REGISTRY_PATH   "/etc/skr8tr/rbac.conf"

/* ── Team record ────────────────────────────────────────────────── */

typedef struct {
    char    name[RBAC_MAX_NAME];
    char    namespace_[RBAC_MAX_NAME];   /* trailing _ avoids C++ keyword clash */
    uint8_t permissions;
    uint8_t pubkey[RBAC_PK_LEN];
    int     active;
} RbacTeam;

/* ── Gateway config ─────────────────────────────────────────────── */

typedef struct {
    char registry_path[512];
    char conductor_host[64];
    int  conductor_port;
    int  gateway_port;
} RbacConfig;
