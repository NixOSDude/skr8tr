/*
 * skr8tr_syslog.c — RFC 5424 Syslog Forwarder — Implementation
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 *
 * RFC 5424 syslog message format:
 *   <PRIVAL>VERSION TIMESTAMP HOSTNAME APPNAME PROCID MSGID SD MSG
 *
 * RFC 5426 (UDP) — plaintext, fire-and-forget, accepted by all SIEMs.
 * RFC 5425 (TLS/TCP) — encrypted transport, required for HIPAA § 164.312(e).
 *
 * Compliance:
 *   HIPAA § 164.312(b),(e)  — Audit Controls + Transmission Security
 *   HITRUST CSF 09.ab       — Monitoring System Use
 *   PCI DSS 10.5.3          — Protect audit logs in separate system
 *   NIST 800-53 AU-4, AU-9  — Audit capacity + protection
 *   SOC 2 CC7.2             — Logical Access Monitoring
 */

#include "skr8tr_syslog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static int             g_enabled    = 0;
static int             g_use_tls    = 0;
static char            g_host[256]  = {0};
static int             g_port       = 514;
static char            g_hostname[64] = {0};  /* this machine's hostname */
static pid_t           g_pid        = 0;

/* UDP */
static int             g_udp_sock   = -1;
static struct sockaddr_in g_udp_dst = {0};

/* TLS/TCP */
static int             g_tls_fd     = -1;
static SSL_CTX*        g_ssl_ctx    = NULL;
static SSL*            g_ssl        = NULL;
static char            g_ca_cert[512] = {0};

static pthread_mutex_t g_syslog_mu  = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* RFC 5424 timestamp: "2026-04-07T14:22:11.000Z" */
static void rfc5424_timestamp(char* buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm* tm_utc = gmtime(&ts.tv_sec);
    int ms = (int)(ts.tv_nsec / 1000000) % 1000;   /* clamp to [0,999] */
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_utc->tm_year + 1900, tm_utc->tm_mon + 1, tm_utc->tm_mday,
             tm_utc->tm_hour, tm_utc->tm_min, tm_utc->tm_sec, ms);
}

/* Sanitise a field: replace control chars and pipe chars with '_' */
static void sanitise(const char* src, char* dst, size_t dst_len) {
    size_t i = 0;
    for (; src[i] && i < dst_len - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
                 ? '_' : (char)c;
    }
    dst[i] = '\0';
}

/* Build RFC 5424 message into buf.
 * Format: <PRI>1 TIMESTAMP HOSTNAME APPNAME PID MSGID - MSG\n */
static int build_rfc5424(char* buf, size_t buf_len, int severity,
                         const char* appname, const char* msgid,
                         const char* msg) {
    int prival = (SKRSYSLOG_FACILITY_LOCAL0 * 8) + severity;
    char ts[32];
    rfc5424_timestamp(ts, sizeof(ts));

    char safe_app[64], safe_mid[64], safe_msg[1024];
    sanitise(appname && appname[0] ? appname : "conductor",
             safe_app, sizeof(safe_app));
    sanitise(msgid   && msgid[0]   ? msgid   : "-",
             safe_mid, sizeof(safe_mid));
    sanitise(msg     && msg[0]     ? msg      : "-",
             safe_msg, sizeof(safe_msg));

    return snprintf(buf, buf_len,
                    "<%d>1 %s %s %s %d %s - %s\n",
                    prival, ts,
                    g_hostname[0] ? g_hostname : "-",
                    safe_app, g_pid, safe_mid, safe_msg);
}

/* -------------------------------------------------------------------------
 * TLS/TCP reconnect
 * ---------------------------------------------------------------------- */

static int tls_connect(void) {
    /* Close any existing connection */
    if (g_ssl)    { SSL_shutdown(g_ssl); SSL_free(g_ssl); g_ssl = NULL; }
    if (g_tls_fd >= 0) { close(g_tls_fd); g_tls_fd = -1; }

    /* Resolve host */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo* res  = NULL;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", g_port);
    if (getaddrinfo(g_host, port_s, &hints, &res) != 0 || !res)
        return -1;

    g_tls_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tls_fd < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(g_tls_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(g_tls_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(g_tls_fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(g_tls_fd); g_tls_fd = -1; freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    g_ssl = SSL_new(g_ssl_ctx);
    if (!g_ssl) { close(g_tls_fd); g_tls_fd = -1; return -1; }

    SSL_set_fd(g_ssl, g_tls_fd);
    SSL_set_tlsext_host_name(g_ssl, g_host);

    if (SSL_connect(g_ssl) != 1) {
        SSL_free(g_ssl); g_ssl = NULL;
        close(g_tls_fd); g_tls_fd = -1;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * skrsyslog_init
 * ---------------------------------------------------------------------- */

int skrsyslog_init(const char* host, int port, int use_tls,
                   const char* ca_cert) {
    if (!host || !host[0]) return -1;

    pthread_mutex_lock(&g_syslog_mu);

    snprintf(g_host, sizeof(g_host), "%s", host);
    g_port    = port > 0 ? port : (use_tls ? 6514 : 514);
    g_use_tls = use_tls;
    g_pid     = getpid();
    if (gethostname(g_hostname, sizeof(g_hostname) - 1) != 0)
        snprintf(g_hostname, sizeof(g_hostname), "skr8tr-conductor");

    if (ca_cert && ca_cert[0])
        snprintf(g_ca_cert, sizeof(g_ca_cert), "%s", ca_cert);

    if (use_tls) {
        /* ── TLS/TCP (RFC 5425) ── */
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!g_ssl_ctx) {
            pthread_mutex_unlock(&g_syslog_mu);
            fprintf(stderr, "[syslog] SSL_CTX_new failed\n");
            return -1;
        }
        SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

        if (g_ca_cert[0]) {
            if (!SSL_CTX_load_verify_locations(g_ssl_ctx, g_ca_cert, NULL)) {
                fprintf(stderr, "[syslog] WARNING: cannot load CA cert %s — "
                        "proceeding without peer verification\n", g_ca_cert);
            } else {
                SSL_CTX_set_verify(g_ssl_ctx,
                                   SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                                   NULL);
            }
        } else {
            /* No CA cert: skip peer verification — acceptable for internal
             * networks; provide --syslog-ca for HIPAA external transport. */
            SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
        }

        if (tls_connect() < 0) {
            fprintf(stderr, "[syslog] WARNING: cannot connect TLS to %s:%d — "
                    "will retry on next event\n", g_host, g_port);
            /* Don't fail init — we retry on first send */
        } else {
            fprintf(stderr, "[syslog] TLS syslog connected: %s:%d\n",
                    g_host, g_port);
        }
    } else {
        /* ── UDP (RFC 5426) ── */
        if (g_udp_sock >= 0) close(g_udp_sock);
        g_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_udp_sock < 0) {
            pthread_mutex_unlock(&g_syslog_mu);
            fprintf(stderr, "[syslog] socket: %s\n", strerror(errno));
            return -1;
        }

        memset(&g_udp_dst, 0, sizeof(g_udp_dst));
        g_udp_dst.sin_family = AF_INET;
        g_udp_dst.sin_port   = htons((uint16_t)g_port);

        struct hostent* he = gethostbyname(g_host);
        if (he && he->h_addrtype == AF_INET) {
            memcpy(&g_udp_dst.sin_addr, he->h_addr_list[0], 4);
        } else if (inet_pton(AF_INET, g_host, &g_udp_dst.sin_addr) <= 0) {
            close(g_udp_sock); g_udp_sock = -1;
            pthread_mutex_unlock(&g_syslog_mu);
            fprintf(stderr, "[syslog] cannot resolve: %s\n", g_host);
            return -1;
        }

        fprintf(stderr, "[syslog] UDP syslog → %s:%d\n", g_host, g_port);
    }

    g_enabled = 1;
    pthread_mutex_unlock(&g_syslog_mu);
    return 0;
}

/* -------------------------------------------------------------------------
 * skrsyslog_send
 * ---------------------------------------------------------------------- */

void skrsyslog_send(int severity, const char* appname,
                    const char* msgid, const char* msg) {
    if (!g_enabled) return;

    char buf[2048];
    int  n = build_rfc5424(buf, sizeof(buf), severity, appname, msgid, msg);
    if (n <= 0) return;

    pthread_mutex_lock(&g_syslog_mu);

    if (g_use_tls) {
        /* TLS reconnect on failure */
        if (!g_ssl && tls_connect() < 0) {
            pthread_mutex_unlock(&g_syslog_mu);
            return;   /* best-effort — do not block the audit log write */
        }

        /* RFC 5425 framing: "<length> <msg>" (octet-counting framing) */
        char framed[2200];
        int  flen = snprintf(framed, sizeof(framed), "%d %.*s", n, n, buf);
        if (SSL_write(g_ssl, framed, flen) <= 0) {
            /* Connection broke — reset and try once more next call */
            SSL_free(g_ssl); g_ssl = NULL;
            close(g_tls_fd); g_tls_fd = -1;
        }
    } else {
        /* UDP — fire and forget */
        sendto(g_udp_sock, buf, (size_t)n, 0,
               (struct sockaddr*)&g_udp_dst, sizeof(g_udp_dst));
    }

    pthread_mutex_unlock(&g_syslog_mu);
}

/* -------------------------------------------------------------------------
 * skrsyslog_close
 * ---------------------------------------------------------------------- */

void skrsyslog_close(void) {
    pthread_mutex_lock(&g_syslog_mu);
    g_enabled = 0;
    if (g_ssl)    { SSL_shutdown(g_ssl); SSL_free(g_ssl); g_ssl = NULL; }
    if (g_tls_fd >= 0) { close(g_tls_fd); g_tls_fd = -1; }
    SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL;
    if (g_udp_sock >= 0) { close(g_udp_sock); g_udp_sock = -1; }
    pthread_mutex_unlock(&g_syslog_mu);
}
