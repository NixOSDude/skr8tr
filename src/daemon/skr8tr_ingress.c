/*
 * skr8tr_ingress.c — Sovereign HTTP/1.1 (+HTTP/2 in Enterprise) Ingress / Reverse Proxy
 * Skr8tr Sovereign Workload Orchestrator
 *
 * REPO: oss
 * SSoA LEVEL 2 — Manifold Anchor
 *
 * Responsibilities:
 *   - Listens on TCP (plain or TLS)
 *   - HTTP/1.1 reverse proxy (OSS)
 *   - HTTP/2 over TLS via ALPN (Enterprise — requires -DENTERPRISE -lnghttp2)
 *   - Routes requests to backend services via longest-prefix path match
 *   - Resolves backend IP:port by querying the Tower (UDP 7772)
 *   - Injects X-Forwarded-For and X-Real-IP headers
 *   - Retries on backend failure (Tower round-robins to next replica)
 *   - h2 → HTTP/1.1 on the backend (industry standard reverse proxy model)
 *
 * OSS build deps:   -lssl -lcrypto -lpthread
 * Enterprise deps:  -lssl -lcrypto -lnghttp2 -lpthread  (add -DENTERPRISE)
 *
 * Usage:
 *   skr8tr_ingress [options]
 *     --listen <port>              TCP port (default: 80)
 *     --tower  <host>              Tower host (default: 127.0.0.1)
 *     --route  <prefix>:<service>  Route (longest prefix match, repeatable)
 *     --workers <n>                Max concurrent connections (default: 64)
 *     --tls-cert <path>            PEM certificate (enables HTTPS)
 *     --tls-key  <path>            PEM private key
 */

#include "../core/fabric.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef ENTERPRISE
#include <nghttp2/nghttp2.h>
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define INGRESS_PORT_DEFAULT  80
#define TOWER_PORT            7772
#define MAX_ROUTES            64
#define BACKLOG               256
#define CONNECT_TIMEOUT_S     5
#define FORWARD_TIMEOUT_S     30
#define MAX_RETRY             3

#ifdef ENTERPRISE
#define H2_MAX_STREAMS        128
#define H2_MAX_HDR_BUF        8192
#define H2_MAX_BODY           (4 * 1024 * 1024)   /* 4 MB request/response body cap */
#define H2_MAX_RESP_HEADERS   64
#endif /* ENTERPRISE */

/* -------------------------------------------------------------------------
 * Route table
 * ---------------------------------------------------------------------- */

typedef struct {
    char prefix[256];
    char service[128];
    int  prefix_len;
} Route;

static Route g_routes[MAX_ROUTES];
static int   g_route_count      = 0;
static char  g_tower_host[64]   = "127.0.0.1";
static int   g_listen_port      = INGRESS_PORT_DEFAULT;
static SSL_CTX *g_ssl_ctx       = NULL;
static char  g_tls_cert[512]    = {0};
static char  g_tls_key[512]     = {0};

/* -------------------------------------------------------------------------
 * Route matching — longest prefix wins
 * ---------------------------------------------------------------------- */

static const Route *route_match(const char *path) {
    const Route *best = NULL;
    int best_len = -1;
    for (int i = 0; i < g_route_count; i++) {
        const Route *r = &g_routes[i];
        if (!strncmp(path, r->prefix, (size_t)r->prefix_len) &&
            r->prefix_len > best_len) {
            best     = r;
            best_len = r->prefix_len;
        }
    }
    return best;
}

/* -------------------------------------------------------------------------
 * Tower lookup — resolve service_name → ip:port via UDP
 * ---------------------------------------------------------------------- */

static int tower_lookup(const char *service, char *ip_out, int *port_out) {
    int sock = fabric_bind(0);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "LOOKUP|%.127s", service);
    if (fabric_send(sock, g_tower_host, TOWER_PORT, cmd, strlen(cmd)) < 0) {
        close(sock); return -1;
    }

    char buf[512] = {0};
    int n = fabric_recv(sock, buf, sizeof(buf) - 1, NULL);
    close(sock);
    if (n <= 0) return -1;
    buf[n] = '\0';

    if (strncmp(buf, "OK|LOOKUP|", 10)) return -1;
    char *p     = buf + 10;
    char *pipe1 = strchr(p, '|');       if (!pipe1) return -1;
    char *pipe2 = strchr(pipe1 + 1, '|'); if (!pipe2) return -1;

    size_t ip_len = (size_t)(pipe2 - (pipe1 + 1));
    if (ip_len >= INET_ADDRSTRLEN) return -1;
    memcpy(ip_out, pipe1 + 1, ip_len);
    ip_out[ip_len] = '\0';
    *port_out = (int)strtol(pipe2 + 1, NULL, 10);
    return (*port_out > 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * TCP connect to backend with timeout
 * ---------------------------------------------------------------------- */

static int backend_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv_send = { .tv_sec = CONNECT_TIMEOUT_S, .tv_usec = 0 };
    struct timeval tv_recv = { .tv_sec = FORWARD_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* -------------------------------------------------------------------------
 * TLS client I/O wrappers (ssl == NULL → plain TCP)
 * ---------------------------------------------------------------------- */

static ssize_t client_read(int fd, SSL *ssl, void *buf, size_t len) {
    if (ssl) { int r = SSL_read(ssl, buf, (int)len); return r > 0 ? r : -1; }
    return recv(fd, buf, len, 0);
}

static ssize_t client_write(int fd, SSL *ssl, const void *buf, size_t len) {
    if (ssl) { int r = SSL_write(ssl, buf, (int)len); return r > 0 ? r : -1; }
    return send(fd, buf, len, MSG_NOSIGNAL);
}

/* -------------------------------------------------------------------------
 * HTTP/1.1 bidirectional proxy
 * ---------------------------------------------------------------------- */

static void proxy_forward(int client_fd, int backend_fd, SSL *ssl) {
    fd_set rfds;
    char buf[8192];
    int max_fd = client_fd > backend_fd ? client_fd : backend_fd;

    for (;;) {
        if (ssl && SSL_pending(ssl) > 0) {
            ssize_t n = client_read(client_fd, ssl, buf, sizeof(buf));
            if (n <= 0) break;
            if (send(backend_fd, buf, (size_t)n, MSG_NOSIGNAL) <= 0) break;
            continue;
        }
        FD_ZERO(&rfds);
        FD_SET(client_fd,  &rfds);
        FD_SET(backend_fd, &rfds);
        struct timeval tv = { .tv_sec = FORWARD_TIMEOUT_S, .tv_usec = 0 };
        if (select(max_fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
        if (FD_ISSET(client_fd, &rfds)) {
            ssize_t n = client_read(client_fd, ssl, buf, sizeof(buf));
            if (n <= 0) break;
            if (send(backend_fd, buf, (size_t)n, MSG_NOSIGNAL) <= 0) break;
        }
        if (FD_ISSET(backend_fd, &rfds)) {
            ssize_t n = recv(backend_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (client_write(client_fd, ssl, buf, (size_t)n) <= 0) break;
        }
    }
}

/* =========================================================================
 * HTTP/2 support — Enterprise only
 * Compiled in only when -DENTERPRISE is passed to the compiler.
 * OSS builds omit all nghttp2 code and serve HTTP/1.1 only.
 * ====================================================================== */

#ifdef ENTERPRISE

/* Per-stream state — one entry per active h2 stream */
typedef struct {
    int32_t  stream_id;
    char     method[16];
    char     path[1024];
    char     authority[256];
    char     req_hdrs[H2_MAX_HDR_BUF];
    int      req_hdrs_len;
    uint8_t *req_body;
    size_t   req_body_len;
    size_t   req_body_cap;
    int      resp_status;
    char     resp_hdrs[H2_MAX_HDR_BUF];
    int      resp_hdrs_len;
    uint8_t *resp_body;
    size_t   resp_body_len;
    size_t   resp_body_offset;
} H2Stream;

/* Per-connection h2 state */
typedef struct {
    nghttp2_session *session;
    SSL             *ssl;
    int              fd;
    char             client_ip[INET_ADDRSTRLEN];
    H2Stream         streams[H2_MAX_STREAMS];
    int              stream_count;
} H2Conn;

/* ── Stream table helpers ── */

static H2Stream *h2_find(H2Conn *c, int32_t sid) {
    for (int i = 0; i < c->stream_count; i++)
        if (c->streams[i].stream_id == sid) return &c->streams[i];
    return NULL;
}

static H2Stream *h2_alloc(H2Conn *c, int32_t sid) {
    if (c->stream_count >= H2_MAX_STREAMS) return NULL;
    H2Stream *s = &c->streams[c->stream_count++];
    memset(s, 0, sizeof(*s));
    s->stream_id = sid;
    return s;
}

static void h2_free(H2Conn *c, int32_t sid) {
    for (int i = 0; i < c->stream_count; i++) {
        if (c->streams[i].stream_id != sid) continue;
        free(c->streams[i].req_body);
        free(c->streams[i].resp_body);
        if (i < c->stream_count - 1)
            c->streams[i] = c->streams[c->stream_count - 1];
        c->stream_count--;
        return;
    }
}

/* ── nghttp2 send callback ── */

static ssize_t h2_send_cb(nghttp2_session *session [[maybe_unused]],
                           const uint8_t *data, size_t length,
                           int flags [[maybe_unused]], void *user_data) {
    H2Conn *c = user_data;
    ssize_t n = c->ssl
        ? SSL_write(c->ssl, data, (int)length)
        : send(c->fd, data, length, MSG_NOSIGNAL);
    if (n <= 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    return n;
}

/* ── nghttp2 data provider — reads buffered response body ── */

static ssize_t h2_read_body(nghttp2_session *session [[maybe_unused]],
                             int32_t stream_id, uint8_t *buf, size_t length,
                             uint32_t *data_flags, nghttp2_data_source *src,
                             void *user_data) {
    H2Conn   *c = user_data;
    H2Stream *s = h2_find(c, stream_id);
    if (!s) { *data_flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }

    (void)src;
    size_t avail   = s->resp_body_len - s->resp_body_offset;
    size_t to_copy = avail < length ? avail : length;
    memcpy(buf, s->resp_body + s->resp_body_offset, to_copy);
    s->resp_body_offset += to_copy;
    if (s->resp_body_offset >= s->resp_body_len)
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)to_copy;
}

/* ── Dispatch one h2 stream → HTTP/1.1 backend ── */

static void h2_dispatch(H2Conn *c, H2Stream *s) {
    char path_clean[1024];
    snprintf(path_clean, sizeof(path_clean), "%s", s->path);
    char *qs = strchr(path_clean, '?'); if (qs) *qs = '\0';

    const Route *route = route_match(path_clean);
    if (!route) {
        nghttp2_nv hdrs[] = {
            { (uint8_t *)":status", (uint8_t *)"404", 7, 3, NGHTTP2_NV_FLAG_NONE },
        };
        nghttp2_submit_response(c->session, s->stream_id, hdrs, 1, NULL);
        return;
    }

    int backend_fd = -1;
    for (int attempt = 0; attempt < MAX_RETRY && backend_fd < 0; attempt++) {
        char be_ip[INET_ADDRSTRLEN] = {0};
        int  be_port = 0;
        if (tower_lookup(route->service, be_ip, &be_port) < 0) continue;
        backend_fd = backend_connect(be_ip, be_port);
    }
    if (backend_fd < 0) {
        nghttp2_nv hdrs[] = {
            { (uint8_t *)":status", (uint8_t *)"503", 7, 3, NGHTTP2_NV_FLAG_NONE },
        };
        nghttp2_submit_response(c->session, s->stream_id, hdrs, 1, NULL);
        return;
    }

    char req[H2_MAX_HDR_BUF + 256];
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "X-Forwarded-For: %s\r\n"
        "X-Real-IP: %s\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        s->method, s->path,
        s->authority[0] ? s->authority : route->service,
        c->client_ip, c->client_ip,
        s->req_hdrs);

    send(backend_fd, req, (size_t)req_len, MSG_NOSIGNAL);
    if (s->req_body && s->req_body_len > 0)
        send(backend_fd, s->req_body, s->req_body_len, MSG_NOSIGNAL);

    uint8_t raw[H2_MAX_BODY] = {0};
    size_t  raw_len = 0;
    {
        char tmp[8192];
        ssize_t n;
        while ((n = recv(backend_fd, tmp, sizeof(tmp), 0)) > 0) {
            if (raw_len + (size_t)n > H2_MAX_BODY) break;
            memcpy(raw + raw_len, tmp, (size_t)n);
            raw_len += (size_t)n;
        }
    }
    close(backend_fd);

    if (raw_len == 0) {
        nghttp2_nv hdrs[] = {{ (uint8_t *)":status", (uint8_t *)"502",
                               7, 3, NGHTTP2_NV_FLAG_NONE }};
        nghttp2_submit_response(c->session, s->stream_id, hdrs, 1, NULL);
        return;
    }

    char *eol1 = (char *)memchr(raw, '\n', raw_len);
    if (!eol1) { goto resp502; }

    int status_code = 200;
    {
        char *sp = memchr(raw, ' ', (size_t)(eol1 - (char *)raw));
        if (sp) status_code = (int)strtol(sp + 1, NULL, 10);
        if (status_code <= 0 || status_code > 999) status_code = 200;
    }

    nghttp2_nv resp_nv[H2_MAX_RESP_HEADERS];
    int nv_count = 0;

    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%d", status_code);
    resp_nv[nv_count++] = (nghttp2_nv){
        .name     = (uint8_t *)":status",
        .value    = (uint8_t *)status_str,
        .namelen  = 7,
        .valuelen = strlen(status_str),
        .flags    = NGHTTP2_NV_FLAG_NONE,
    };

    static const char *HOP_BY_HOP[] = {
        "connection", "keep-alive", "transfer-encoding",
        "te", "trailers", "upgrade", NULL
    };

    char *hdr_cur = s->resp_hdrs;
    int   hdr_rem = H2_MAX_HDR_BUF;

    char *line = eol1 + 1;
    while (nv_count < H2_MAX_RESP_HEADERS) {
        char *next_eol = memchr(line, '\n', raw_len - (size_t)(line - (char *)raw));
        if (!next_eol) break;
        size_t line_len = (size_t)(next_eol - line);
        if (line_len == 0 || (line_len == 1 && *line == '\r')) break;

        char *colon = memchr(line, ':', line_len);
        if (!colon) { line = next_eol + 1; continue; }

        size_t nlen = (size_t)(colon - line);
        char  *vstart = colon + 1;
        while (vstart < next_eol && (*vstart == ' ' || *vstart == '\t')) vstart++;
        size_t vlen = (size_t)(next_eol - vstart);
        if (vlen > 0 && vstart[vlen - 1] == '\r') vlen--;

        char name_lc[256] = {0};
        size_t nc = nlen < 255 ? nlen : 255;
        for (size_t k = 0; k < nc; k++)
            name_lc[k] = (char)((line[k] >= 'A' && line[k] <= 'Z')
                                ? line[k] + 32 : line[k]);

        bool skip = false;
        for (int h = 0; HOP_BY_HOP[h]; h++)
            if (!strcmp(name_lc, HOP_BY_HOP[h])) { skip = true; break; }

        if (!skip && hdr_rem > (int)(nlen + vlen + 4)) {
            uint8_t *name_ptr = (uint8_t *)hdr_cur;
            memcpy(hdr_cur, name_lc, nlen);
            hdr_cur += nlen; hdr_rem -= (int)nlen;
            *hdr_cur++ = '\0'; hdr_rem--;

            uint8_t *val_ptr = (uint8_t *)hdr_cur;
            memcpy(hdr_cur, vstart, vlen);
            hdr_cur += vlen; hdr_rem -= (int)vlen;
            *hdr_cur++ = '\0'; hdr_rem--;

            resp_nv[nv_count++] = (nghttp2_nv){
                .name     = name_ptr,
                .value    = val_ptr,
                .namelen  = nlen,
                .valuelen = vlen,
                .flags    = NGHTTP2_NV_FLAG_NONE,
            };
        }
        line = next_eol + 1;
    }

    {
        uint8_t *body_start = NULL;
        size_t   body_len   = 0;
        uint8_t *sep = (uint8_t *)memmem(raw, raw_len, "\r\n\r\n", 4);
        if (sep) { body_start = sep + 4; }
        else {
            sep = (uint8_t *)memmem(raw, raw_len, "\n\n", 2);
            if (sep) body_start = sep + 2;
        }
        if (body_start && body_start < raw + raw_len)
            body_len = raw_len - (size_t)(body_start - raw);

        if (body_len > 0) {
            s->resp_body = malloc(body_len);
            if (s->resp_body) {
                memcpy(s->resp_body, body_start, body_len);
                s->resp_body_len    = body_len;
                s->resp_body_offset = 0;
            }
        }
    }

    {
        nghttp2_data_provider dp = { .read_callback = h2_read_body };
        nghttp2_submit_response(c->session, s->stream_id,
                                resp_nv, (size_t)nv_count,
                                s->resp_body_len > 0 ? &dp : NULL);
    }
    return;

resp502:
    {
        nghttp2_nv hdrs[] = {{ (uint8_t *)":status", (uint8_t *)"502",
                               7, 3, NGHTTP2_NV_FLAG_NONE }};
        nghttp2_submit_response(c->session, s->stream_id, hdrs, 1, NULL);
    }
}

/* ── nghttp2 callbacks ── */

static int h2_on_begin_headers(nghttp2_session *session [[maybe_unused]],
                                const nghttp2_frame *frame, void *user_data) {
    H2Conn *c = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST)
        h2_alloc(c, frame->hd.stream_id);
    return 0;
}

static int h2_on_header(nghttp2_session *session [[maybe_unused]],
                         const nghttp2_frame *frame,
                         const uint8_t *name, size_t namelen,
                         const uint8_t *value, size_t valuelen,
                         uint8_t flags [[maybe_unused]], void *user_data) {
    H2Conn   *c = user_data;
    H2Stream *s = h2_find(c, frame->hd.stream_id);
    if (!s) return 0;

    if (namelen > 0 && name[0] == ':') {
        if (namelen == 7 && !memcmp(name, ":method", 7))
            snprintf(s->method, sizeof(s->method), "%.*s", (int)valuelen, value);
        else if (namelen == 5 && !memcmp(name, ":path", 5))
            snprintf(s->path, sizeof(s->path), "%.*s", (int)valuelen, value);
        else if (namelen == 10 && !memcmp(name, ":authority", 10))
            snprintf(s->authority, sizeof(s->authority), "%.*s",
                     (int)valuelen, value);
    } else {
        int avail = H2_MAX_HDR_BUF - s->req_hdrs_len - 1;
        if (avail > 0)
            s->req_hdrs_len += snprintf(s->req_hdrs + s->req_hdrs_len,
                                        (size_t)avail,
                                        "%.*s: %.*s\r\n",
                                        (int)namelen,  name,
                                        (int)valuelen, value);
    }
    return 0;
}

static int h2_on_data_chunk(nghttp2_session *session [[maybe_unused]],
                              uint8_t flags [[maybe_unused]],
                              int32_t stream_id,
                              const uint8_t *data, size_t len,
                              void *user_data) {
    H2Conn   *c = user_data;
    H2Stream *s = h2_find(c, stream_id);
    if (!s || s->req_body_len + len > H2_MAX_BODY) return 0;

    if (s->req_body_len + len > s->req_body_cap) {
        size_t cap = s->req_body_cap ? s->req_body_cap * 2 : 4096;
        while (cap < s->req_body_len + len) cap *= 2;
        uint8_t *nb = realloc(s->req_body, cap);
        if (!nb) return 0;
        s->req_body     = nb;
        s->req_body_cap = cap;
    }
    memcpy(s->req_body + s->req_body_len, data, len);
    s->req_body_len += len;
    return 0;
}

static int h2_on_frame_recv(nghttp2_session *session [[maybe_unused]],
                              const nghttp2_frame *frame, void *user_data) {
    H2Conn *c = user_data;
    if ((frame->hd.type == NGHTTP2_HEADERS ||
         frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        H2Stream *s = h2_find(c, frame->hd.stream_id);
        if (s) h2_dispatch(c, s);
    }
    return 0;
}

static int h2_on_stream_close(nghttp2_session *session [[maybe_unused]],
                               int32_t stream_id,
                               uint32_t error_code [[maybe_unused]],
                               void *user_data) {
    h2_free((H2Conn *)user_data, stream_id);
    return 0;
}

/* ── flush_h2 — drain pending nghttp2 output ── */

static int flush_h2(H2Conn *c) {
    for (;;) {
        const uint8_t *data;
        ssize_t n = nghttp2_session_mem_send(c->session, &data);
        if (n == 0) return 0;
        if (n < 0)  return -1;
        ssize_t sent = c->ssl
            ? SSL_write(c->ssl, data, (int)n)
            : send(c->fd, data, (size_t)n, MSG_NOSIGNAL);
        if (sent <= 0) return -1;
    }
}

/* ── handle_h2_connection — main I/O loop for h2 session ── */

static void handle_h2_connection(H2Conn *c) {
    nghttp2_submit_settings(c->session, NGHTTP2_FLAG_NONE, NULL, 0);
    if (flush_h2(c) < 0) return;

    uint8_t buf[65536];
    while (nghttp2_session_want_read(c->session) ||
           nghttp2_session_want_write(c->session)) {

        if (flush_h2(c) < 0) break;
        if (!nghttp2_session_want_read(c->session)) break;

        ssize_t n = c->ssl
            ? SSL_read(c->ssl, buf, sizeof(buf))
            : recv(c->fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        ssize_t processed = nghttp2_session_mem_recv(c->session, buf, (size_t)n);
        if (processed < 0) break;

        if (flush_h2(c) < 0) break;
    }
}

/* ── ALPN callback — prefer h2, fall back to http/1.1 ── */

static int alpn_select_cb(SSL *ssl [[maybe_unused]],
                           const unsigned char **out, unsigned char *outlen,
                           const unsigned char *in, unsigned int inlen,
                           void *arg [[maybe_unused]]) {
    if (nghttp2_select_next_protocol((unsigned char **)out, outlen,
                                     in, inlen) >= 0)
        return SSL_TLSEXT_ERR_OK;

    static const unsigned char HTTP11[] = "\x08http/1.1";
    if (SSL_select_next_proto((unsigned char **)out, outlen,
                              HTTP11, sizeof(HTTP11) - 1,
                              in, inlen) == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;

    return SSL_TLSEXT_ERR_NOACK;
}

#endif /* ENTERPRISE */

/* -------------------------------------------------------------------------
 * Connection handler — dispatches h2 (Enterprise) or HTTP/1.1 (OSS)
 * ---------------------------------------------------------------------- */

typedef struct { int fd; char ip[INET_ADDRSTRLEN]; SSL *ssl; } ConnArg;

static void *handle_connection(void *arg) {
    ConnArg *ca       = arg;
    int      client_fd = ca->fd;
    SSL     *ssl       = ca->ssl;
    char     client_ip[INET_ADDRSTRLEN];
    snprintf(client_ip, sizeof(client_ip), "%s", ca->ip);
    free(ca);

#ifdef ENTERPRISE
    /* Detect h2 via ALPN */
    bool is_h2 = false;
    if (ssl) {
        const unsigned char *proto;
        unsigned int proto_len;
        SSL_get0_alpn_selected(ssl, &proto, &proto_len);
        if (proto_len == 2 && !memcmp(proto, "h2", 2))
            is_h2 = true;
    }

    if (is_h2) {
        nghttp2_session_callbacks *cbs;
        nghttp2_session_callbacks_new(&cbs);
        nghttp2_session_callbacks_set_send_callback(cbs,             h2_send_cb);
        nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, h2_on_begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(cbs,        h2_on_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_on_data_chunk);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,    h2_on_frame_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs,  h2_on_stream_close);

        H2Conn conn = { .ssl = ssl, .fd = client_fd, .stream_count = 0 };
        snprintf(conn.client_ip, sizeof(conn.client_ip), "%s", client_ip);

        nghttp2_session_server_new(&conn.session, cbs, &conn);
        nghttp2_session_callbacks_del(cbs);

        handle_h2_connection(&conn);

        nghttp2_session_del(conn.session);
        for (int i = 0; i < conn.stream_count; i++) {
            free(conn.streams[i].req_body);
            free(conn.streams[i].resp_body);
        }
    } else {
#endif /* ENTERPRISE */

        /* ── HTTP/1.1 path ── */
        char head[4096] = {0};
        int  head_len   = 0;

        while (head_len < (int)sizeof(head) - 1) {
            ssize_t n = client_read(client_fd, ssl, head + head_len, 1);
            if (n <= 0) goto done;
            head_len++;
            if (head_len >= 4 && !memcmp(head + head_len - 4, "\r\n\r\n", 4))
                break;
        }
        head[head_len] = '\0';

        char method[16] = {0}, path[1024] = {0}, proto[16] = {0};
        if (sscanf(head, "%15s %1023s %15s", method, path, proto) < 2)
            goto done;

        char path_clean[1024];
        snprintf(path_clean, sizeof(path_clean), "%s", path);
        char *qs = strchr(path_clean, '?'); if (qs) *qs = '\0';

        const Route *route = route_match(path_clean);
        if (!route) {
            const char *r404 = "HTTP/1.1 404 Not Found\r\n"
                               "Content-Length: 24\r\n\r\n"
                               "No route for this path.\n";
            client_write(client_fd, ssl, r404, strlen(r404));
            goto done;
        }

        char injected[512];
        int inj_len = snprintf(injected, sizeof(injected),
                               "X-Forwarded-For: %s\r\n"
                               "X-Real-IP: %s\r\n",
                               client_ip, client_ip);

        int backend_fd = -1;
        for (int attempt = 0; attempt < MAX_RETRY && backend_fd < 0; attempt++) {
            char be_ip[INET_ADDRSTRLEN] = {0};
            int  be_port = 0;
            if (tower_lookup(route->service, be_ip, &be_port) < 0) {
                if (attempt == MAX_RETRY - 1) {
                    const char *r503 = "HTTP/1.1 503 Service Unavailable\r\n"
                                       "Content-Length: 27\r\n\r\n"
                                       "Service not found in Tower\n";
                    client_write(client_fd, ssl, r503, strlen(r503));
                }
                continue;
            }
            backend_fd = backend_connect(be_ip, be_port);
        }
        if (backend_fd < 0) goto done;

        char *eol = strstr(head, "\r\n");
        if (!eol) { close(backend_fd); goto done; }

        size_t line_len = (size_t)(eol - head) + 2;
        send(backend_fd, head, line_len, MSG_NOSIGNAL);
        send(backend_fd, injected, (size_t)inj_len, MSG_NOSIGNAL);
        send(backend_fd, eol + 2, (size_t)(head_len - (int)line_len), MSG_NOSIGNAL);

        proxy_forward(client_fd, backend_fd, ssl);
        close(backend_fd);

#ifdef ENTERPRISE
    } /* end is_h2 / HTTP/1.1 branch */
#endif

done:
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    close(client_fd);
    return NULL;
}

/* =========================================================================
 * main
 * ====================================================================== */

static void usage(void) {
    fprintf(stderr,
        "usage: skr8tr_ingress [options]\n"
        "  --listen   <port>              TCP listen port (default: 80)\n"
        "  --tower    <host>              Tower host (default: 127.0.0.1)\n"
        "  --route    <prefix>:<service>  Add route (longest prefix wins)\n"
        "  --workers  <n>                 Max concurrent connections (default: 64)\n"
        "  --tls-cert <path>              PEM certificate (enables HTTPS)\n"
        "  --tls-key  <path>              PEM private key\n"
#ifdef ENTERPRISE
        "                                 HTTP/2 via ALPN enabled (Enterprise)\n"
#endif
        "\nexamples:\n"
        "  skr8tr_ingress --route /api:api-svc --route /:frontend\n"
        "  skr8tr_ingress --tls-cert tls.crt --tls-key tls.key "
        "--listen 443 --route /:frontend\n"
    );
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    int max_workers = 64;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--listen")   && i + 1 < argc)
            g_listen_port = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--tower")    && i + 1 < argc)
            snprintf(g_tower_host, sizeof(g_tower_host), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--workers")  && i + 1 < argc)
            max_workers = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--tls-cert") && i + 1 < argc)
            snprintf(g_tls_cert, sizeof(g_tls_cert), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tls-key")  && i + 1 < argc)
            snprintf(g_tls_key, sizeof(g_tls_key), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--route")    && i + 1 < argc) {
            if (g_route_count >= MAX_ROUTES) {
                fprintf(stderr, "[ingress] too many routes (max %d)\n", MAX_ROUTES);
                return 1;
            }
            char *sep = strchr(argv[++i], ':');
            if (!sep) {
                fprintf(stderr, "[ingress] bad --route: use prefix:service\n");
                usage(); return 1;
            }
            Route *r  = &g_routes[g_route_count++];
            size_t pl = (size_t)(sep - argv[i]);
            if (pl >= sizeof(r->prefix)) pl = sizeof(r->prefix) - 1;
            memcpy(r->prefix, argv[i], pl);
            r->prefix[pl]  = '\0';
            r->prefix_len  = (int)pl;
            snprintf(r->service, sizeof(r->service), "%s", sep + 1);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(); return 0;
        } else {
            fprintf(stderr, "[ingress] unknown flag: %s\n", argv[i]);
            usage(); return 1;
        }
    }

    if (g_route_count == 0) {
        fprintf(stderr, "[ingress] no routes — add at least one --route\n");
        usage(); return 1;
    }

    /* Sort routes: longer prefixes first */
    for (int i = 0; i < g_route_count - 1; i++)
        for (int j = i + 1; j < g_route_count; j++)
            if (g_routes[j].prefix_len > g_routes[i].prefix_len) {
                Route tmp = g_routes[i];
                g_routes[i] = g_routes[j];
                g_routes[j] = tmp;
            }

    /* TLS initialization */
    if (g_tls_cert[0] && g_tls_key[0]) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();

        g_ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!g_ssl_ctx) {
            fprintf(stderr, "[ingress] FATAL: SSL_CTX_new failed\n");
            ERR_print_errors_fp(stderr); return 1;
        }
        if (SSL_CTX_use_certificate_file(g_ssl_ctx, g_tls_cert,
                                          SSL_FILETYPE_PEM) != 1) {
            fprintf(stderr, "[ingress] FATAL: cannot load cert: %s\n", g_tls_cert);
            ERR_print_errors_fp(stderr); return 1;
        }
        if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, g_tls_key,
                                         SSL_FILETYPE_PEM) != 1) {
            fprintf(stderr, "[ingress] FATAL: cannot load key: %s\n", g_tls_key);
            ERR_print_errors_fp(stderr); return 1;
        }
        if (SSL_CTX_check_private_key(g_ssl_ctx) != 1) {
            fprintf(stderr, "[ingress] FATAL: cert/key mismatch\n");
            ERR_print_errors_fp(stderr); return 1;
        }

        SSL_CTX_set_options(g_ssl_ctx,
            SSL_OP_CIPHER_SERVER_PREFERENCE |
            SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
            SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

#ifdef ENTERPRISE
        /* ALPN — advertise h2 and http/1.1 */
        SSL_CTX_set_alpn_select_cb(g_ssl_ctx, alpn_select_cb, NULL);
#endif
    }

    printf("[ingress] Skr8tr Ingress starting...\n");
    printf("[ingress] listen=%d  tower=%s:%d  workers=%d  tls=%s  h2=%s\n",
           g_listen_port, g_tower_host, TOWER_PORT, max_workers,
           g_ssl_ctx ? "enabled" : "disabled",
#ifdef ENTERPRISE
           g_ssl_ctx ? "enabled (ALPN)" : "disabled"
#else
           "disabled (OSS — HTTP/1.1 only)"
#endif
    );
    for (int i = 0; i < g_route_count; i++)
        printf("[ingress] route: %-32s → %s\n",
               g_routes[i].prefix, g_routes[i].service);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "[ingress] FATAL: socket: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)g_listen_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ingress] FATAL: bind port %d: %s\n",
                g_listen_port, strerror(errno));
        return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        fprintf(stderr, "[ingress] FATAL: listen: %s\n", strerror(errno));
        return 1;
    }
    printf("[ingress] ready — accepting connections\n");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int active = 0;
    pthread_mutex_t active_mu = PTHREAD_MUTEX_INITIALIZER;

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { if (errno == EINTR) continue; continue; }

        pthread_mutex_lock(&active_mu);
        int cap = (active < max_workers);
        if (cap) active++;
        pthread_mutex_unlock(&active_mu);

        if (!cap) {
            const char *r503 = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Length: 13\r\n\r\nAt capacity.\n";
            send(client_fd, r503, strlen(r503), MSG_NOSIGNAL);
            close(client_fd);
            continue;
        }

        ConnArg *ca = malloc(sizeof(ConnArg));
        if (!ca) {
            close(client_fd);
            pthread_mutex_lock(&active_mu); active--; pthread_mutex_unlock(&active_mu);
            continue;
        }
        ca->fd  = client_fd;
        ca->ssl = NULL;
        inet_ntop(AF_INET, &client_addr.sin_addr, ca->ip, sizeof(ca->ip));

        if (g_ssl_ctx) {
            SSL *ssl = SSL_new(g_ssl_ctx);
            if (!ssl || SSL_set_fd(ssl, client_fd) != 1) {
                if (ssl) SSL_free(ssl);
                free(ca); close(client_fd);
                pthread_mutex_lock(&active_mu); active--; pthread_mutex_unlock(&active_mu);
                continue;
            }
            if (SSL_accept(ssl) != 1) {
                SSL_free(ssl); free(ca); close(client_fd);
                pthread_mutex_lock(&active_mu); active--; pthread_mutex_unlock(&active_mu);
                continue;
            }
            ca->ssl = ssl;
        }

        pthread_t tid;
        if (pthread_create(&tid, &attr, handle_connection, ca) != 0) {
            if (ca->ssl) { SSL_shutdown(ca->ssl); SSL_free(ca->ssl); }
            free(ca); close(client_fd);
            pthread_mutex_lock(&active_mu); active--; pthread_mutex_unlock(&active_mu);
        }
    }

    pthread_attr_destroy(&attr);
    return 0;
}
