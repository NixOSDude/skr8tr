/*
 * skr8tr_syslog.h — RFC 5424 Syslog Forwarder — Public API
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * Forwards audit events to a remote syslog collector (SIEM, Splunk,
 * Graylog, Elasticsearch, QRadar, Microsoft Sentinel, etc.) in
 * RFC 5424 structured-data format.
 *
 * Two transport modes:
 *   UDP (default)  — RFC 5426, port 514 — zero-setup, most SIEMs accept it
 *   TLS/TCP        — RFC 5425, port 6514 — required for HIPAA in-transit
 *                    encryption; uses OpenSSL, same dep as ingress + audit
 *
 * Compliance:
 *   HIPAA § 164.312(b)   — Audit Controls: centralised log collection
 *   HIPAA § 164.312(e)   — Transmission Security: TLS mode
 *   HITRUST CSF 09.ab    — Monitoring System Use: SIEM integration
 *   PCI DSS 10.5         — Protect audit logs: remote copy in separate system
 *   NIST 800-53 AU-4     — Audit Log Storage Capacity (remote offload)
 *   SOC 2 CC7.2          — Logical Access Monitoring: centralised SIEM
 */

#pragma once

#include <stddef.h>

/* -------------------------------------------------------------------------
 * RFC 5424 severity levels (align with syslog(3))
 * ---------------------------------------------------------------------- */

#define SKRSYSLOG_EMERG   0   /* system is unusable */
#define SKRSYSLOG_ALERT   1   /* action must be taken immediately */
#define SKRSYSLOG_CRIT    2   /* critical conditions */
#define SKRSYSLOG_ERR     3   /* error conditions — use for AUTH_FAIL */
#define SKRSYSLOG_WARN    4   /* warning conditions */
#define SKRSYSLOG_NOTICE  5   /* normal but significant */
#define SKRSYSLOG_INFO    6   /* informational — use for normal audit events */
#define SKRSYSLOG_DEBUG   7   /* debug-level messages */

/* RFC 5424 facility — local0 (facility 16) is the standard for custom apps */
#define SKRSYSLOG_FACILITY_LOCAL0  16

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * skrsyslog_init — configure the syslog forwarder.
 *
 * host:     IP or hostname of the syslog collector
 * port:     UDP/TCP port (514 = UDP default, 6514 = TLS default)
 * use_tls:  0 = UDP (RFC 5426), 1 = TLS/TCP (RFC 5425, HIPAA-grade)
 * ca_cert:  path to PEM CA cert for TLS peer verification (NULL = no verify,
 *           acceptable for internal networks; required for HIPAA transport)
 *
 * Returns 0 on success, -1 on failure (error written to stderr).
 * Must be called before skrsyslog_send().
 */
int skrsyslog_init(const char* host, int port, int use_tls,
                   const char* ca_cert);

/*
 * skrsyslog_send — forward one event to the syslog collector.
 *
 * severity:  one of SKRSYSLOG_* constants
 * appname:   workload/app name (or "conductor" for system events)
 * msgid:     event type string (e.g. "SUBMIT", "AUTH_FAIL", "ROLLOUT")
 * msg:       free-form message (all fields are sanitised before send)
 *
 * Thread-safe. No-op if skrsyslog_init() was not called or failed.
 */
void skrsyslog_send(int severity, const char* appname,
                    const char* msgid, const char* msg);

/*
 * skrsyslog_close — close the syslog transport.
 * Safe to call even if init was not called.
 */
void skrsyslog_close(void);
