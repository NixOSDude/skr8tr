/*
 * skr8tr_audit.h — Cryptographic Audit Ledger — Public API
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Audit integrity depends on this chain.
 *
 * Mini-blockchain audit log:
 *   Each entry is a 64-byte SHA-256 hash of:
 *     (prev_hash || timestamp || event || app_name || source_ip || detail)
 *   appended to a flat append-only file at SKRAUDIT_LOG_PATH.
 *
 * HIPAA § 164.312(b) — Audit Controls
 * HITRUST CSF 09.aa   — Audit Logging
 * HITRUST CSF 09.ac   — Log Integrity
 * NIST 800-53 AU-9    — Protection of Audit Information
 * PCI DSS 10.2        — Audit Log Events
 * SOC 2 CC7.2         — Logical Access Monitoring
 *
 * Hash: OpenSSL SHA256() — same dependency as skr8tr_ingress
 * Format: newline-delimited text, one entry per line:
 *   <seq>|<timestamp>|<event>|<app>|<src_ip>|<detail>|<entry_hash>
 * The entry_hash covers (prev_hash + all fields above) so any
 * tampering breaks the chain from that point forward.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SKRAUDIT_LOG_PATH    "/var/log/skr8tr_audit.log"
#define SKRAUDIT_HASH_BYTES  32   /* SHA-256 → 32 bytes */
#define SKRAUDIT_HASH_HEX    64   /* 32 bytes hex-encoded */

/* -------------------------------------------------------------------------
 * Audit event types — maps to HIPAA/HITRUST/NIST event categories
 * ---------------------------------------------------------------------- */

#define SKRAUDIT_SUBMIT      "SUBMIT"       /* workload submitted (HIPAA: resource access) */
#define SKRAUDIT_EVICT       "EVICT"        /* workload evicted  (HIPAA: resource removal) */
#define SKRAUDIT_ROLLOUT     "ROLLOUT"      /* rolling update    (HIPAA: modification)      */
#define SKRAUDIT_EXEC        "EXEC"         /* remote exec       (HIPAA: privileged action) */
#define SKRAUDIT_AUTH_FAIL   "AUTH_FAIL"    /* bad PQC sig       (HIPAA: auth failure)      */
#define SKRAUDIT_NODE_DIED   "NODE_DIED"    /* proc death        (HIPAA: system event)      */
#define SKRAUDIT_SCALE_UP    "SCALE_UP"     /* autoscale up      (NIST AU-2: audit event)   */
#define SKRAUDIT_SCALE_DOWN  "SCALE_DOWN"   /* autoscale down    (NIST AU-2: audit event)   */
#define SKRAUDIT_CONDUCTOR   "CONDUCTOR"    /* conductor start   (SOC 2 CC6.1)              */

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * skraudit_init — open/create the audit log file, write genesis entry.
 * Call once at Conductor startup.
 * path: override default log path, or NULL to use SKRAUDIT_LOG_PATH.
 * Sets chmod 600 on the log file — access control hardening.
 */
void skraudit_init(const char* path);

/*
 * skraudit_set_encryption — enable AES-256-GCM at-rest encryption.
 *
 * key_path: path to a 32-byte raw binary key file.  Generate with:
 *             openssl rand -out ~/.skr8tr/audit.key 32
 *             chmod 600 ~/.skr8tr/audit.key
 *
 * Each log entry is encrypted individually.  The hash chain still
 * operates over plaintext so integrity verification works after
 * decryption.  Covers HIPAA § 164.312(a)(2)(iv) — Encryption at Rest,
 * PCI DSS 3.4 — Protect stored data.
 *
 * Must be called BEFORE skraudit_init() to encrypt the genesis entry.
 * Returns 0 on success, -1 if the key file cannot be read.
 */
int skraudit_set_encryption(const char* key_path);

/*
 * skraudit_set_syslog — wire audit events to a syslog forwarder.
 *
 * Must be called AFTER skrsyslog_init().  Every subsequent
 * skraudit_log() call will also call skrsyslog_send() with the
 * appropriate RFC 5424 severity.
 *
 * Covers HIPAA § 164.312(b) centralised log collection,
 * PCI DSS 10.5.3 — remote copy of audit logs.
 */
void skraudit_set_syslog(int enabled);

/*
 * skraudit_log — append a cryptographically chained audit entry.
 *
 * event:    one of the SKRAUDIT_* constants above (or any short string)
 * app:      workload/app name (may be empty string)
 * src_ip:   source IP of the operator command (or "local")
 * detail:   free-form detail string (manifest path, exit code, etc.)
 *
 * Thread-safe: internal mutex serialises writes.
 */
void skraudit_log(const char* event, const char* app,
                  const char* src_ip, const char* detail);

/*
 * skraudit_tail — read the last `n` entries from the log and write them
 * into buf (null-terminated).  Returns number of bytes written.
 * Used by the AUDIT|<n> Conductor command.
 */
int skraudit_tail(int n, char* buf, size_t buf_len);

/*
 * skraudit_verify_chain — walk the entire log file and verify every hash
 * link.  Returns 0 if chain is intact, -1 with error detail in err.
 */
int skraudit_verify_chain(char* err, size_t err_len);
