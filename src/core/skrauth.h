/*
 * skrauth.h — Sovereign Command Authentication
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every daemon that gates mutations depends on this.
 *
 * ML-DSA-65 (CRYSTALS-Dilithium Level 3) command signing and verification.
 * Applied at the Conductor boundary (UDP 7771) for SUBMIT and EVICT.
 *
 * Wire format for signed commands:
 *   <cmd>|<unix_ts>|<6618-hex-ml-dsa65-sig>
 *
 * Signed payload (what the signature covers):
 *   <cmd>|<unix_ts>
 *
 * Key sizes (ML-DSA-65):
 *   Public key:  1952 bytes → skrtrview.pub
 *   Secret key:  4032 bytes → ~/.skr8tr/signing.sec (chmod 600)
 *   Signature:   3309 bytes → 6618 hex chars on the wire
 *
 * No password. No TLS. No bearer token. No YAML RBAC.
 * The operator holds the key. The cluster holds the pubkey. Done.
 */

#pragma once

#include <stddef.h>

/* Reject commands whose timestamp is more than ±30 seconds from now.
 * Prevents replay attacks without requiring per-command nonce storage. */
#define SKRAUTH_NONCE_WINDOW_S   30

/* Default filenames — resolved relative to daemon working directory. */
#define SKRAUTH_PUBKEY_FILENAME  "skrtrview.pub"
#define SKRAUTH_SECKEY_FILENAME  ".skr8tr/signing.sec"

/* ML-DSA-65 constants */
#define SKRAUTH_PK_LEN   1952
#define SKRAUTH_SK_LEN   4032
#define SKRAUTH_SIG_LEN  3309
#define SKRAUTH_HEXSIG_LEN  (SKRAUTH_SIG_LEN * 2)   /* 6618 */

/*
 * skrauth_sign — sign a command string.
 *
 * Forms payload = "<cmd>|<now>", signs it with ML-DSA-65 using the secret key
 * at seckey_path, then writes "<cmd>|<now>|<hex_sig>" into out.
 *
 * out must be at least strlen(cmd) + SKRAUTH_HEXSIG_LEN + 32 bytes.
 * Returns 0 on success, -1 on error (err filled with a human-readable message).
 */
int skrauth_sign(const char *cmd, const char *seckey_path,
                 char *out, size_t out_len,
                 char *err, size_t err_len);

/*
 * skrauth_verify — verify and strip a signed command.
 *
 * Checks ML-DSA-65 signature and nonce window (±SKRAUTH_NONCE_WINDOW_S seconds).
 * On success, copies the bare command (without |ts|sig) into cmd_out.
 * Returns 0 on success, -1 on failure (malformed, expired, or bad signature).
 */
int skrauth_verify(const char *signed_cmd, const char *pubkey_path,
                   char *cmd_out, size_t cmd_out_len);
