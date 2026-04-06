/*
 * SSoA LEVEL 2: MANIFOLD ANCHOR
 * FILE: src/cockpit/skr8tr_cockpit.c
 * MISSION: Skr8trView cockpit — sovereign WebSocket control plane for the
 *          Skr8tr mesh. Serves the UI, enforces SkrtrPass ML-DSA-65 auth,
 *          routes commands to Conductor/Tower/Node via UDP, and pushes live
 *          mesh state to all connected operator and admin sessions.
 *
 * PORT MAP:
 *   7780 — cockpit HTTP (static UI) + WebSocket (/ws)
 *   7771 — Conductor status queries (UDP)
 *   7775 — Conductor + Node command port (UDP)
 *   7772 — Tower registry queries (UDP)
 *
 * AUTH PROTOCOL:
 *   1. Client connects to ws://host:7780/ws
 *   2. First WS text frame MUST be: AUTH|<skrtrpass_token>
 *   3. Cockpit calls skrtrpass_verify() against skrtrview.pub
 *   4. Success → AUTH_OK|operator  or  AUTH_OK|admin
 *   5. Failure → AUTH_ERR|<reason>  then close
 *
 * WS COMMAND PROTOCOL (post-auth):
 *   NODES              → poll conductor STATUS  → NODES|n|id:ip:cpu:ram,...
 *   LIST               → poll conductor LIST    → LIST|n|app:node:pid,...
 *   SERVICES           → poll tower    LIST     → SERVICES|n|name:ip:port:ttl,...
 *   LOGS|<app>         → resolve node, fetch    → LOGS|<app>|<text>
 *   SUBMIT|<path>      → admin: SUBMIT to conductor
 *   EVICT|<app>        → admin: KILL to conductor
 *   PING               → PONG
 *
 * PUSH (every PUSH_INTERVAL_S to all authed sessions):
 *   PUSH|NODES|n|id:ip:cpu:ram,...
 *   PUSH|LIST|n|app:node:pid,...
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../../src/core/fabric.h"
#include "skrtrpass.h"

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

#define COCKPIT_PORT          7780
#define CONDUCTOR_IP          "127.0.0.1"
#define CONDUCTOR_STATUS_PORT 7771
#define CONDUCTOR_CMD_PORT    7775
#define TOWER_IP              "127.0.0.1"
#define TOWER_PORT            7772

#define PUSH_INTERVAL_S       5
#define UDP_TIMEOUT_MS        3000
#define MAX_SESSIONS          64
#define REQUEST_BUF           16384
#define FRAME_BUF             65536
#define UDPBUF                8192

/* Path to ML-DSA-65 public key file — override with --pubkey flag */
#define DEFAULT_PUBKEY_PATH   "./skrtrview.pub"

/* Path to static UI directory — override with --ui flag */
#define DEFAULT_UI_PATH       "./ui"

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static volatile int   g_running    = 1;
static int            g_udp_sock   = -1;         /* shared UDP socket (mutex-guarded) */
static pthread_mutex_t g_udp_mu    = PTHREAD_MUTEX_INITIALIZER;
static char           g_pubkey_path[512];
static char           g_ui_path[512];
static int            g_port       = COCKPIT_PORT;

/* -------------------------------------------------------------------------
 * Session table — one entry per active WebSocket connection
 * ---------------------------------------------------------------------- */

typedef struct {
    int      fd;
    int      active;
    int      authenticated;
    int      role;           /* SKRTRPASS_ROLE_OPERATOR | SKRTRPASS_ROLE_ADMIN */
} Session;

static Session         g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_sess_mu = PTHREAD_MUTEX_INITIALIZER;

static Session* session_alloc(int fd) {
    pthread_mutex_lock(&g_sess_mu);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            g_sessions[i] = (Session){ .fd = fd, .active = 1 };
            pthread_mutex_unlock(&g_sess_mu);
            return &g_sessions[i];
        }
    }
    pthread_mutex_unlock(&g_sess_mu);
    return NULL;
}

static void session_free(Session* s) {
    pthread_mutex_lock(&g_sess_mu);
    memset(s, 0, sizeof(*s));
    pthread_mutex_unlock(&g_sess_mu);
}

/* -------------------------------------------------------------------------
 * write_discard — best-effort write for error/close paths where we
 * explicitly don't care if the peer has already disconnected.
 * ---------------------------------------------------------------------- */
static inline void write_discard(int fd, const void* buf, size_t len) {
    ssize_t r = write(fd, buf, len);
    (void)r;
}

/* -------------------------------------------------------------------------
 * UDP helpers — send command, wait for reply with timeout
 * ---------------------------------------------------------------------- */

static int udp_query(const char* ip, int port,
                     const char* cmd, size_t cmd_len,
                     char* reply, size_t reply_cap) {
    pthread_mutex_lock(&g_udp_mu);

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec  = UDP_TIMEOUT_MS / 1000,
        .tv_usec = (UDP_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(g_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int sent = fabric_send(g_udp_sock, ip, port, cmd, cmd_len);
    if (sent < 0) {
        pthread_mutex_unlock(&g_udp_mu);
        return -1;
    }

    int n = fabric_recv(g_udp_sock, reply, reply_cap - 1, NULL);
    pthread_mutex_unlock(&g_udp_mu);

    if (n < 0) return -1;
    reply[n] = '\0';
    return n;
}

/* Fire-and-forget UDP (admin commands) */
static void udp_fire(const char* ip, int port, const char* cmd) {
    pthread_mutex_lock(&g_udp_mu);
    fabric_send(g_udp_sock, ip, port, cmd, strlen(cmd));
    pthread_mutex_unlock(&g_udp_mu);
}

/* -------------------------------------------------------------------------
 * Mesh queries
 * ---------------------------------------------------------------------- */

/* NODES → reply "OK|NODES|n|id:ip:cpu:ram,..." or error */
static int query_nodes(char* out, size_t cap) {
    return udp_query(CONDUCTOR_IP, CONDUCTOR_STATUS_PORT,
                     "STATUS", 6, out, cap);
}

/* LIST → reply "OK|LIST|n|app:node:pid,..." */
static int query_list(char* out, size_t cap) {
    return udp_query(CONDUCTOR_IP, CONDUCTOR_CMD_PORT,
                     "LIST", 4, out, cap);
}

/* SERVICES → query tower LIST */
static int query_services(char* out, size_t cap) {
    return udp_query(TOWER_IP, TOWER_PORT,
                     "LIST", 4, out, cap);
}

/* LOGS|<app> — two-step: LIST → node_id → NODES → ip → LOGS */
static int query_logs(const char* app, char* out, size_t cap) {
    char list_reply[UDPBUF];
    if (query_list(list_reply, sizeof(list_reply)) < 0) {
        snprintf(out, cap, "ERR|conductor unreachable");
        return -1;
    }

    /* Parse OK|LIST|n|app:node:pid,app:node:pid,... */
    char* data = strstr(list_reply, "OK|LIST|");
    if (!data) { snprintf(out, cap, "ERR|bad conductor reply"); return -1; }
    data = strchr(data + 8, '|');
    if (!data) { snprintf(out, cap, "ERR|parse fail"); return -1; }
    data++;  /* skip past count '|' */
    data = strchr(data, '|');
    if (!data) { snprintf(out, cap, "ERR|parse fail"); return -1; }
    data++;

    char node_id[64] = {0};
    char* tok = strtok(data, ",");
    while (tok) {
        char tok_app[64], tok_node[64], tok_pid[32];
        if (sscanf(tok, "%63[^:]:%63[^:]:%31s", tok_app, tok_node, tok_pid) == 3) {
            if (!strcmp(tok_app, app)) {
                snprintf(node_id, sizeof(node_id), "%s", tok_node);
                break;
            }
        }
        tok = strtok(NULL, ",");
    }

    if (!node_id[0]) {
        snprintf(out, cap, "LOGS|%s|app not found in placement table", app);
        return 0;
    }

    /* Find node IP from NODES reply */
    char nodes_reply[UDPBUF];
    if (query_nodes(nodes_reply, sizeof(nodes_reply)) < 0) {
        snprintf(out, cap, "ERR|conductor NODES unreachable");
        return -1;
    }

    char node_ip[INET_ADDRSTRLEN] = {0};
    char* ndata = strstr(nodes_reply, "OK|NODES|");
    if (ndata) {
        ndata = strchr(ndata + 9, '|');
        if (ndata) {
            ndata++;
            ndata = strchr(ndata, '|');
            if (ndata) ndata++;
        }
    }
    if (ndata) {
        char* ntok = strtok(ndata, ",");
        while (ntok) {
            char nid[64], nip[INET_ADDRSTRLEN], ncpu[8], nram[8];
            if (sscanf(ntok, "%63[^:]:%15[^:]:%7[^:]:%7s", nid, nip, ncpu, nram) >= 2) {
                if (!strncmp(nid, node_id, strlen(node_id))) {
                    snprintf(node_ip, sizeof(node_ip), "%.15s", nip);
                    break;
                }
            }
            ntok = strtok(NULL, ",");
        }
    }

    if (!node_ip[0]) {
        snprintf(out, cap, "LOGS|%s|node IP not found (node may be down)", app);
        return 0;
    }

    /* Send LOGS|<app> to the node command port */
    char log_cmd[128];
    snprintf(log_cmd, sizeof(log_cmd), "LOGS|%s", app);
    char log_reply[UDPBUF];
    int r = udp_query(node_ip, CONDUCTOR_CMD_PORT,
                      log_cmd, strlen(log_cmd),
                      log_reply, sizeof(log_reply));
    if (r < 0) {
        snprintf(out, cap, "LOGS|%s|node %s unreachable", app, node_ip);
        return 0;
    }

    snprintf(out, cap, "LOGS|%s|%s", app, log_reply);
    return 0;
}

/* -------------------------------------------------------------------------
 * WebSocket framing — RFC 6455 minimal implementation
 * ---------------------------------------------------------------------- */

/* SHA-1 block compression — processes one 64-byte block into h[0..4] */
static void sha1_block(uint32_t h[5], const uint8_t blk[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]   << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] <<  8) |  (uint32_t)blk[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f=(b&c)|((~b)&d); k=0x5A827999; }
        else if (i < 40) { f=b^c^d;          k=0x6ED9EBA1; }
        else if (i < 60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else             { f=b^c^d;          k=0xCA62C1D6; }
        uint32_t tmp = ((a<<5)|(a>>27)) + f + e + k + w[i];
        e=d; d=c; c=(b<<30)|(b>>2); b=a; a=tmp;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

/* SHA-1 of arbitrary-length message (up to 119 bytes — covers WS handshake) */
static void sha1(const uint8_t* msg, size_t len, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};

    /* Build padded message in 128-byte buffer (max two 64-byte blocks) */
    uint8_t padded[128] = {0};
    memcpy(padded, msg, len);
    padded[len] = 0x80;

    /* Append big-endian 64-bit bit-length at the end of the last block */
    uint64_t bit_len = (uint64_t)len * 8;
    size_t   total   = (len < 56) ? 64 : 128;   /* one or two blocks */
    for (int i = 0; i < 8; i++)
        padded[total - 1 - i] = (uint8_t)(bit_len >> (8 * i));

    /* Process block(s) */
    sha1_block(h, padded);
    if (total == 128)
        sha1_block(h, padded + 64);

    /* Produce digest */
    for (int i = 0; i < 5; i++) {
        digest[4*i]   = (uint8_t)(h[i] >> 24);
        digest[4*i+1] = (uint8_t)(h[i] >> 16);
        digest[4*i+2] = (uint8_t)(h[i] >>  8);
        digest[4*i+3] = (uint8_t)(h[i]);
    }
}

/* Compute Sec-WebSocket-Accept from client key */
static void ws_accept_key(const char* client_key, char* out64, size_t out_len) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    char cat[256];
    int  cat_len = snprintf(cat, sizeof(cat), "%s%s", client_key, GUID);
    if (cat_len < 0) { out64[0] = '\0'; return; }

    uint8_t digest[20];
    sha1((const uint8_t*)cat, (size_t)cat_len, digest);

    /* Base64 encode digest */
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i < 20 && o + 4 < out_len) {
        uint32_t v = ((uint32_t)digest[i] << 16)
                   | (i+1 < 20 ? (uint32_t)digest[i+1] << 8 : 0)
                   | (i+2 < 20 ? (uint32_t)digest[i+2]      : 0);
        out64[o++] = B64[(v >> 18) & 63];
        out64[o++] = B64[(v >> 12) & 63];
        out64[o++] = (i+1 < 20) ? B64[(v >>  6) & 63] : '=';
        out64[o++] = (i+2 < 20) ? B64[ v        & 63] : '=';
        i += 3;
    }
    out64[o] = '\0';
}

/* Write a WebSocket text frame (no masking — server→client) */
static int ws_send_text(int fd, const char* payload, size_t plen) {
    uint8_t header[10];
    size_t  hlen;

    header[0] = 0x81;  /* FIN + opcode text */
    if (plen < 126) {
        header[1] = (uint8_t)plen;
        hlen = 2;
    } else if (plen < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(plen >> 8);
        header[3] = (uint8_t)(plen);
        hlen = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2+i] = (uint8_t)(plen >> (56 - 8*i));
        hlen = 10;
    }

    /* Write header then payload — best-effort, ignore partial sends */
    ssize_t r = write(fd, header, hlen);
    if (r < 0) return -1;
    r = write(fd, payload, plen);
    return (r < 0) ? -1 : 0;
}

/* Read one WebSocket frame from fd.
 * Returns payload length (≥0) or -1 on error/close.
 * Client frames are masked; we unmask in-place. */
static int ws_recv_frame(int fd, char* buf, size_t cap) {
    uint8_t h[2];
    if (recv(fd, h, 2, MSG_WAITALL) != 2) return -1;

    int opcode = h[0] & 0x0f;
    if (opcode == 8) return -1;  /* close frame */

    int masked  = (h[1] & 0x80) != 0;
    uint64_t plen = h[1] & 0x7f;

    if (plen == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    uint8_t mask[4] = {0};
    if (masked && recv(fd, mask, 4, MSG_WAITALL) != 4) return -1;

    if (plen >= cap) return -1;
    if ((ssize_t)plen != recv(fd, buf, plen, MSG_WAITALL)) return -1;

    if (masked)
        for (uint64_t i = 0; i < plen; i++)
            buf[i] ^= mask[i & 3];

    buf[plen] = '\0';
    return (int)plen;
}

/* Send a WebSocket close frame */
static void ws_close(int fd) {
    uint8_t frame[2] = {0x88, 0x00};
    write_discard(fd, frame, 2);
}

/* -------------------------------------------------------------------------
 * HTTP upgrade — parse GET /ws and perform WS handshake
 * Returns 1 if upgrade succeeded, 0 on plain HTTP or error.
 * ---------------------------------------------------------------------- */

static int http_ws_upgrade(int fd, char* raw, int raw_len) {
    (void)raw_len;

    /* Must be GET /ws */
    if (strncmp(raw, "GET /ws", 7) != 0) return 0;

    /* Extract Sec-WebSocket-Key */
    char ws_key[128] = {0};
    const char* kp = strstr(raw, "Sec-WebSocket-Key:");
    if (!kp) return 0;
    kp += 18;
    while (*kp == ' ') kp++;
    int ki = 0;
    while (*kp && *kp != '\r' && ki < (int)sizeof(ws_key) - 1)
        ws_key[ki++] = *kp++;

    char accept[64];
    ws_accept_key(ws_key, accept, sizeof(accept));

    char response[512];
    int rlen = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept);

    write_discard(fd, response, (size_t)rlen);
    return 1;
}

/* -------------------------------------------------------------------------
 * Serve static files (HTTP GET — not /ws)
 * ---------------------------------------------------------------------- */

static void serve_static(int fd, const char* uri) {
    char path[1024];
    const char* file = (*uri == '/') ? uri + 1 : uri;
    if (!*file) file = "index.html";

    /* Strip query string */
    char clean[512];
    strncpy(clean, file, sizeof(clean) - 1);
    clean[sizeof(clean)-1] = '\0';
    char* q = strchr(clean, '?');
    if (q) *q = '\0';

    /* Reject traversal */
    if (strstr(clean, "..")) {
        const char* e = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        write_discard(fd, e, strlen(e));
        return;
    }

    snprintf(path, sizeof(path), "%s/%s", g_ui_path, clean);

    int ffd = open(path, O_RDONLY);
    if (ffd < 0) {
        /* SPA fallback → index.html */
        snprintf(path, sizeof(path), "%s/index.html", g_ui_path);
        ffd = open(path, O_RDONLY);
        if (ffd < 0) {
            const char* e = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            write_discard(fd, e, strlen(e));
            return;
        }
    }

    struct stat st;
    fstat(ffd, &st);

    const char* mime = "text/plain";
    const char* ext  = strrchr(path, '.');
    if (ext) {
        if      (!strcmp(ext, ".html")) mime = "text/html; charset=utf-8";
        else if (!strcmp(ext, ".css"))  mime = "text/css";
        else if (!strcmp(ext, ".js"))   mime = "text/javascript";
        else if (!strcmp(ext, ".wasm")) mime = "application/wasm";
        else if (!strcmp(ext, ".json")) mime = "application/json";
        else if (!strcmp(ext, ".svg"))  mime = "image/svg+xml";
        else if (!strcmp(ext, ".png"))  mime = "image/png";
        else if (!strcmp(ext, ".ico"))  mime = "image/x-icon";
    }

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (long long)st.st_size);

    write_discard(fd, hdr, (size_t)hlen);

    off_t offset = 0;
    off_t rem    = st.st_size;
    while (rem > 0) {
        ssize_t s = sendfile(fd, ffd, &offset, (size_t)rem);
        if (s <= 0) { if (errno == EINTR) continue; break; }
        rem -= s;
    }
    close(ffd);
}

/* -------------------------------------------------------------------------
 * Command dispatcher — process one authenticated WS frame
 * ---------------------------------------------------------------------- */

static void dispatch(Session* sess, const char* frame, int flen) {
    (void)flen;

    char reply[UDPBUF * 2];
    reply[0] = '\0';

    if (!strcmp(frame, "PING")) {
        ws_send_text(sess->fd, "PONG", 4);
        return;
    }

    if (!strcmp(frame, "NODES")) {
        char raw[UDPBUF];
        if (query_nodes(raw, sizeof(raw)) > 0) {
            /* Forward raw conductor reply, prefixed for UI clarity */
            ws_send_text(sess->fd, raw, strlen(raw));
        } else {
            ws_send_text(sess->fd, "ERR|NODES|conductor unreachable",
                         strlen("ERR|NODES|conductor unreachable"));
        }
        return;
    }

    if (!strcmp(frame, "LIST")) {
        char raw[UDPBUF];
        if (query_list(raw, sizeof(raw)) > 0) {
            ws_send_text(sess->fd, raw, strlen(raw));
        } else {
            ws_send_text(sess->fd, "ERR|LIST|conductor unreachable",
                         strlen("ERR|LIST|conductor unreachable"));
        }
        return;
    }

    if (!strcmp(frame, "SERVICES")) {
        char raw[UDPBUF];
        if (query_services(raw, sizeof(raw)) > 0) {
            ws_send_text(sess->fd, raw, strlen(raw));
        } else {
            ws_send_text(sess->fd, "ERR|SERVICES|tower unreachable",
                         strlen("ERR|SERVICES|tower unreachable"));
        }
        return;
    }

    if (!strncmp(frame, "LOGS|", 5)) {
        const char* app = frame + 5;
        query_logs(app, reply, sizeof(reply));
        ws_send_text(sess->fd, reply, strlen(reply));
        return;
    }

    /* --- Admin-only commands --- */
    if (sess->role != SKRTRPASS_ROLE_ADMIN) {
        ws_send_text(sess->fd, "ERR|FORBIDDEN|admin role required",
                     strlen("ERR|FORBIDDEN|admin role required"));
        return;
    }

    if (!strncmp(frame, "SUBMIT|", 7)) {
        /* Bound path to 499 chars to fit "SUBMIT|<path>" in 512-byte cmd */
        char path[500];
        snprintf(path, sizeof(path), "%.499s", frame + 7);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "SUBMIT|%.499s", path);
        udp_fire(CONDUCTOR_IP, CONDUCTOR_CMD_PORT, cmd);
        ws_send_text(sess->fd, "OK|SUBMIT|command sent",
                     strlen("OK|SUBMIT|command sent"));
        return;
    }

    if (!strncmp(frame, "EVICT|", 6)) {
        /* Bound app name to 127 chars */
        char app[128];
        snprintf(app, sizeof(app), "%.127s", frame + 6);
        char cmd[140];
        snprintf(cmd, sizeof(cmd), "KILL|%.127s", app);
        udp_fire(CONDUCTOR_IP, CONDUCTOR_CMD_PORT, cmd);
        snprintf(reply, sizeof(reply), "OK|EVICT|%s", app);
        ws_send_text(sess->fd, reply, strlen(reply));
        return;
    }

    ws_send_text(sess->fd, "ERR|UNKNOWN|command not recognised",
                 strlen("ERR|UNKNOWN|command not recognised"));
}

/* -------------------------------------------------------------------------
 * WebSocket session thread — one per connection
 * ---------------------------------------------------------------------- */

static void* session_thread(void* arg) {
    int fd = (int)(intptr_t)arg;

    /* Set socket timeouts */
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read HTTP request */
    char raw[REQUEST_BUF];
    int  n = 0;
    while (n < (int)sizeof(raw) - 1) {
        int r = (int)recv(fd, raw + n, sizeof(raw) - 1 - (size_t)n, 0);
        if (r <= 0) goto done;
        n += r;
        raw[n] = '\0';
        if (strstr(raw, "\r\n\r\n")) break;
    }

    /* Determine if this is a WS upgrade or plain HTTP */
    if (strncmp(raw, "GET /ws", 7) == 0) {
        /* WebSocket path */
        if (!http_ws_upgrade(fd, raw, n)) goto done;

        Session* sess = session_alloc(fd);
        if (!sess) {
            ws_send_text(fd, "ERR|SERVER_FULL", 15);
            goto done;
        }

        char frame[FRAME_BUF];
        int  flen;

        /* First frame MUST be AUTH */
        flen = ws_recv_frame(fd, frame, sizeof(frame) - 1);
        if (flen < 0) { session_free(sess); goto done; }

        if (strncmp(frame, "AUTH|", 5) != 0) {
            ws_send_text(fd, "AUTH_ERR|first message must be AUTH|<token>",
                         strlen("AUTH_ERR|first message must be AUTH|<token>"));
            ws_close(fd);
            session_free(sess);
            goto done;
        }

        const char* token = frame + 5;

        /* Dev mode: if the pubkey file does not exist, bypass auth entirely */
        struct stat _pubkey_st;
        int _has_pubkey = (stat(g_pubkey_path, &_pubkey_st) == 0);

        int role;
        if (!_has_pubkey) {
            fprintf(stderr, "[cockpit] WARN: no pubkey at '%s' — dev mode, granting admin\n",
                    g_pubkey_path);
            role = SKRTRPASS_ROLE_ADMIN;
        } else {
            role = skrtrpass_verify(token, g_pubkey_path);
        }

        if (role < 0) {
            char err[64];
            const char* reason;
            switch (role) {
                case SKRTRPASS_ERR_PARSE:   reason = "malformed token";  break;
                case SKRTRPASS_ERR_EXPIRED: reason = "token expired";     break;
                case SKRTRPASS_ERR_BADSIG:  reason = "invalid signature"; break;
                case SKRTRPASS_ERR_ROLE:    reason = "unknown role";      break;
                default:                    reason = "auth failed";        break;
            }
            snprintf(err, sizeof(err), "AUTH_ERR|%s", reason);
            ws_send_text(fd, err, strlen(err));
            ws_close(fd);
            session_free(sess);
            goto done;
        }

        sess->role          = role;
        sess->authenticated = 1;

        const char* role_str = (role == SKRTRPASS_ROLE_ADMIN) ? "admin" : "operator";
        char ok[32];
        snprintf(ok, sizeof(ok), "AUTH_OK|%s", role_str);
        ws_send_text(fd, ok, strlen(ok));

        fprintf(stderr, "[cockpit] session fd=%d authenticated as %s\n", fd, role_str);

        /* Command loop */
        while (g_running) {
            flen = ws_recv_frame(fd, frame, sizeof(frame) - 1);
            if (flen < 0) break;
            if (flen == 0) continue;
            dispatch(sess, frame, flen);
        }

        session_free(sess);

    } else {
        /* Plain HTTP — serve static file */
        char method[8] = {0}, uri[512] = {0};
        sscanf(raw, "%7s %511s", method, uri);
        serve_static(fd, uri);
    }

done:
    close(fd);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Push thread — broadcasts live mesh state to all authed sessions
 * ---------------------------------------------------------------------- */

static void* push_thread(void* arg) {
    (void)arg;

    while (g_running) {
        sleep(PUSH_INTERVAL_S);
        if (!g_running) break;

        char nodes_raw[UDPBUF], list_raw[UDPBUF];
        int  got_nodes   = query_nodes(nodes_raw, sizeof(nodes_raw)) > 0;
        int  got_list    = query_list(list_raw,   sizeof(list_raw))  > 0;

        char push_nodes[UDPBUF + 8], push_list[UDPBUF + 8];
        size_t pn_len = 0, pl_len = 0;

        if (got_nodes) {
            pn_len = (size_t)snprintf(push_nodes, sizeof(push_nodes),
                                      "PUSH|%s", nodes_raw);
        }
        if (got_list) {
            pl_len = (size_t)snprintf(push_list, sizeof(push_list),
                                      "PUSH|%s", list_raw);
        }

        pthread_mutex_lock(&g_sess_mu);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!g_sessions[i].active || !g_sessions[i].authenticated) continue;
            int sfd = g_sessions[i].fd;
            if (got_nodes && pn_len > 0) ws_send_text(sfd, push_nodes, pn_len);
            if (got_list  && pl_len > 0) ws_send_text(sfd, push_list,  pl_len);
        }
        pthread_mutex_unlock(&g_sess_mu);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * Signal handler
 * ---------------------------------------------------------------------- */

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ---------------------------------------------------------------------- */

static void parse_args(int argc, char* argv[]) {
    snprintf(g_pubkey_path, sizeof(g_pubkey_path), "%s", DEFAULT_PUBKEY_PATH);
    snprintf(g_ui_path,     sizeof(g_ui_path),     "%s", DEFAULT_UI_PATH);
    g_port = COCKPIT_PORT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pubkey") && i + 1 < argc)
            snprintf(g_pubkey_path, sizeof(g_pubkey_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--ui") && i + 1 < argc)
            snprintf(g_ui_path, sizeof(g_ui_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            g_port = (int)strtol(argv[++i], NULL, 10);
    }
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    parse_args(argc, argv);

    /* Shared UDP socket for all mesh queries */
    g_udp_sock = fabric_bind(0);  /* ephemeral port */
    if (g_udp_sock < 0) {
        fprintf(stderr, "[cockpit] FATAL: cannot create UDP socket: %s\n",
                strerror(errno));
        return 1;
    }

    /* TCP listener */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "[cockpit] FATAL: socket: %s\n", strerror(errno));
        return 1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)g_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[cockpit] FATAL: bind port %d: %s\n",
                g_port, strerror(errno));
        return 1;
    }
    if (listen(srv, 32) < 0) {
        fprintf(stderr, "[cockpit] FATAL: listen: %s\n", strerror(errno));
        return 1;
    }

    /* Start push thread */
    pthread_t push_tid;
    pthread_create(&push_tid, NULL, push_thread, NULL);
    pthread_detach(push_tid);

    printf("[cockpit] Skr8trView cockpit ready\n");
    printf("[cockpit] UI:      http://127.0.0.1:%d/\n",  g_port);
    printf("[cockpit] WS:      ws://127.0.0.1:%d/ws\n",  g_port);
    printf("[cockpit] Pubkey:  %s\n", g_pubkey_path);
    printf("[cockpit] UI dir:  %s\n", g_ui_path);

    while (g_running) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &cli_len);
        if (cfd < 0) {
            if (errno == EINTR || !g_running) break;
            continue;
        }

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, session_thread, (void*)(intptr_t)cfd);
        pthread_attr_destroy(&attr);
    }

    close(srv);
    close(g_udp_sock);
    printf("[cockpit] shutdown\n");
    return 0;
}
