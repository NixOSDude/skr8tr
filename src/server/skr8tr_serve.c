/*
 * skr8tr_serve.c — Skr8tr Sovereign Static File Server
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every static workload uses this daemon.
 *
 * Serves any app's build output directory over HTTP/1.1.
 * No nginx. No Apache. No Node.js. No dependencies beyond libc + pthreads.
 *
 * Usage:
 *   skr8tr_serve --dir <static_root> --port <port>
 *   skr8tr_serve ./dist 3000          (positional form also accepted)
 *
 * Capabilities:
 *   - HTTP/1.1 GET and HEAD requests
 *   - MIME detection by file extension (html, css, js, wasm, json, svg,
 *     png, jpg, gif, ico, webp, pdf, txt, map)
 *   - Directory requests → serve index.html
 *   - Connection: keep-alive up to MAX_KEEPALIVE requests per connection
 *   - Path traversal protection: resolves realpath and verifies prefix
 *   - 200, 304 (ETag via mtime), 400, 403, 404, 405, 500
 *   - One pthread per accepted connection; connection threads are detached
 *   - Graceful SIGTERM/SIGINT shutdown
 *
 * Default port: 7773 (Skr8tr gateway — external ingress)
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define SKRTR_SERVE_DEFAULT_PORT  7773
#define REQUEST_BUF_SIZE          8192
#define RESPONSE_HDR_SIZE         2048
#define MAX_PATH_LEN              4096
#define MAX_KEEPALIVE             100     /* max requests per connection */
#define RECV_TIMEOUT_S            30

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static char              g_static_root[MAX_PATH_LEN];
static int               g_port;
static volatile int      g_running = 1;
static atomic_int        g_conn_count = 0;

/* -------------------------------------------------------------------------
 * MIME type table
 * ---------------------------------------------------------------------- */

typedef struct { const char* ext; const char* mime; } MimeEntry;

static const MimeEntry MIME_TABLE[] = {
    { "html",  "text/html; charset=utf-8"       },
    { "htm",   "text/html; charset=utf-8"       },
    { "css",   "text/css; charset=utf-8"        },
    { "js",    "text/javascript; charset=utf-8" },
    { "mjs",   "text/javascript; charset=utf-8" },
    { "json",  "application/json"               },
    { "wasm",  "application/wasm"               },
    { "svg",   "image/svg+xml"                  },
    { "png",   "image/png"                      },
    { "jpg",   "image/jpeg"                     },
    { "jpeg",  "image/jpeg"                     },
    { "gif",   "image/gif"                      },
    { "webp",  "image/webp"                     },
    { "ico",   "image/x-icon"                   },
    { "pdf",   "application/pdf"                },
    { "txt",   "text/plain; charset=utf-8"      },
    { "map",   "application/json"               },
    { "xml",   "application/xml"                },
    { "ttf",   "font/ttf"                       },
    { "woff",  "font/woff"                      },
    { "woff2", "font/woff2"                     },
    { NULL,    NULL                             },
};

static const char* mime_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;  /* skip the dot */
    for (const MimeEntry* e = MIME_TABLE; e->ext; e++)
        if (!strcasecmp(e->ext, dot)) return e->mime;
    return "application/octet-stream";
}

/* -------------------------------------------------------------------------
 * HTTP helpers
 * ---------------------------------------------------------------------- */

/* Write all `len` bytes of `buf` to `fd`, retrying on EINTR. */
static int write_all(int fd, const void* buf, size_t len) {
    const char* p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* Format HTTP date: "Sat, 05 Apr 2026 14:00:00 GMT" */
static void http_date(char* buf, size_t len, time_t t) {
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
}

/* ETag from mtime + size: simple but cache-correct */
static void make_etag(char* buf, size_t len, const struct stat* st) {
    snprintf(buf, len, "\"%lx-%lx\"",
             (unsigned long)st->st_mtime,
             (unsigned long)st->st_size);
}

static void send_error(int fd, int code, const char* reason) {
    char body[256];
    int  body_len = snprintf(body, sizeof(body),
        "<html><body><h1>%d %s</h1></body></html>\r\n", code, reason);
    char hdr[RESPONSE_HDR_SIZE];
    int  hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, body_len);
    write_all(fd, hdr, (size_t)hdr_len);
    write_all(fd, body, (size_t)body_len);
}

/* -------------------------------------------------------------------------
 * Request parser — minimal HTTP/1.1 line reader
 * ---------------------------------------------------------------------- */

typedef struct {
    char method[16];
    char uri[MAX_PATH_LEN];
    char version[16];
    char if_none_match[128];   /* ETag from client */
    int  keep_alive;
} HttpRequest;

/* Read until \r\n\r\n into buf. Returns total bytes read or -1 on error. */
static int recv_headers(int fd, char* buf, int buf_size) {
    int  total = 0;
    char c;
    int  state = 0;  /* state machine: 0=normal 1=\r 2=\r\n 3=\r\n\r 4=done */
    while (total < buf_size - 1) {
        int n = (int)recv(fd, &c, 1, 0);
        if (n <= 0) return n == 0 ? total : -1;
        buf[total++] = c;
        switch (state) {
            case 0: state = (c == '\r') ? 1 : 0; break;
            case 1: state = (c == '\n') ? 2 : 0; break;
            case 2: state = (c == '\r') ? 3 : 0; break;
            case 3: if (c == '\n') { buf[total] = '\0'; return total; }
                    state = 0; break;
        }
    }
    return -1;  /* headers too large */
}

static int parse_request(const char* raw, HttpRequest* req) {
    memset(req, 0, sizeof(*req));
    req->keep_alive = 1;  /* HTTP/1.1 default */

    /* Parse request line */
    if (sscanf(raw, "%15s %4095s %15s", req->method, req->uri, req->version) != 3)
        return 0;

    /* Scan headers we care about */
    const char* p = strstr(raw, "\r\n");
    while (p) {
        p += 2;
        if (*p == '\r') break;  /* end of headers */

        if (!strncasecmp(p, "Connection:", 11)) {
            const char* v = p + 11;
            while (*v == ' ') v++;
            if (!strncasecmp(v, "close", 5)) req->keep_alive = 0;
        }
        if (!strncasecmp(p, "If-None-Match:", 14)) {
            const char* v = p + 14;
            while (*v == ' ') v++;
            int i = 0;
            while (*v && *v != '\r' && i < (int)sizeof(req->if_none_match) - 1)
                req->if_none_match[i++] = *v++;
        }
        p = strstr(p, "\r\n");
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Path resolution — reject traversal, resolve to filesystem path
 * ---------------------------------------------------------------------- */

/* Returns 1 and fills `fs_path` if the URI is safe, 0 otherwise. */
static int resolve_path(const char* uri, char* fs_path, size_t fs_len) {
    /* Strip query string */
    char clean_uri[MAX_PATH_LEN];
    strncpy(clean_uri, uri, sizeof(clean_uri) - 1);
    clean_uri[sizeof(clean_uri) - 1] = '\0';
    char* q = strchr(clean_uri, '?');
    if (q) *q = '\0';

    /* URL-decode %XX sequences (only what we need for safe paths) */
    char decoded[MAX_PATH_LEN];
    int  di = 0;
    for (int si = 0; clean_uri[si] && di < (int)sizeof(decoded) - 1; si++) {
        if (clean_uri[si] == '%' && clean_uri[si+1] && clean_uri[si+2]) {
            char hex[3] = { clean_uri[si+1], clean_uri[si+2], '\0' };
            decoded[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            decoded[di++] = clean_uri[si];
        }
    }
    decoded[di] = '\0';

    /* Build candidate path — root + decoded must fit in MAX_PATH_LEN */
    char candidate[MAX_PATH_LEN];
    size_t root_len = strlen(g_static_root);
    size_t dec_len  = strlen(decoded);
    if (root_len + dec_len + 1 > sizeof(candidate)) return 0;
    memcpy(candidate, g_static_root, root_len);
    memcpy(candidate + root_len, decoded, dec_len + 1);

    /* Resolve to real path — catches .. traversal */
    char resolved[MAX_PATH_LEN];
    if (!realpath(candidate, resolved)) {
        /* File might not exist yet — check the prefix manually */
        /* Only allow if candidate starts with static_root */
        if (strncmp(candidate, g_static_root, strlen(g_static_root)) != 0)
            return 0;
        snprintf(fs_path, fs_len, "%s", candidate);
        return 1;
    }

    /* Verify the resolved path is inside our root */
    if (strncmp(resolved, g_static_root, strlen(g_static_root)) != 0)
        return 0;

    snprintf(fs_path, fs_len, "%s", resolved);
    return 1;
}

/* -------------------------------------------------------------------------
 * Request handler — one call per HTTP request on a connection
 *
 * Returns 1 to keep the connection alive, 0 to close.
 * ---------------------------------------------------------------------- */

static int handle_request(int fd) {
    char raw[REQUEST_BUF_SIZE];
    int  n = recv_headers(fd, raw, sizeof(raw));
    if (n <= 0) return 0;

    HttpRequest req;
    if (!parse_request(raw, &req)) {
        send_error(fd, 400, "Bad Request");
        return 0;
    }

    /* Only GET and HEAD */
    int is_head = !strcmp(req.method, "HEAD");
    if (!is_head && strcmp(req.method, "GET")) {
        send_error(fd, 405, "Method Not Allowed");
        return 0;
    }

    /* Resolve path */
    char fs_path[MAX_PATH_LEN];
    if (!resolve_path(req.uri, fs_path, sizeof(fs_path))) {
        send_error(fd, 403, "Forbidden");
        return req.keep_alive;
    }

    /* Stat the path */
    struct stat st;
    if (stat(fs_path, &st) < 0) {
        /* Not found — try appending /index.html for SPA routing */
        char index_path[MAX_PATH_LEN];
        snprintf(index_path, sizeof(index_path), "%s/index.html", g_static_root);
        if (stat(index_path, &st) == 0) {
            snprintf(fs_path, sizeof(fs_path), "%s", index_path);
        } else {
            send_error(fd, 404, "Not Found");
            return req.keep_alive;
        }
    }

    /* Directory → serve index.html inside it */
    if (S_ISDIR(st.st_mode)) {
        size_t fp_len = strlen(fs_path);
        if (fp_len + 12 >= sizeof(fs_path)) {
            send_error(fd, 500, "Internal Server Error");
            return req.keep_alive;
        }
        memcpy(fs_path + fp_len, "/index.html", 12);  /* includes NUL */
        if (stat(fs_path, &st) < 0) {
            send_error(fd, 404, "Not Found");
            return req.keep_alive;
        }
    }

    /* ETag check — 304 Not Modified */
    char etag[64];
    make_etag(etag, sizeof(etag), &st);
    if (req.if_none_match[0] && !strcmp(req.if_none_match, etag)) {
        char hdr[RESPONSE_HDR_SIZE];
        int  hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 304 Not Modified\r\n"
            "ETag: %s\r\n"
            "Connection: %s\r\n"
            "\r\n",
            etag,
            req.keep_alive ? "keep-alive" : "close");
        write_all(fd, hdr, (size_t)hdr_len);
        return req.keep_alive;
    }

    /* Open the file */
    int file_fd = open(fs_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 500, "Internal Server Error");
        return req.keep_alive;
    }

    /* Format dates */
    char date_str[64], lm_str[64];
    http_date(date_str, sizeof(date_str), time(NULL));
    http_date(lm_str,   sizeof(lm_str),   st.st_mtime);

    const char* mime  = mime_for(fs_path);
    off_t       fsize = st.st_size;

    /* Send response headers */
    char hdr[RESPONSE_HDR_SIZE];
    int  hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Last-Modified: %s\r\n"
        "ETag: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Cache-Control: public, max-age=3600\r\n"
        "Connection: %s\r\n"
        "\r\n",
        date_str, lm_str, etag, mime,
        (long long)fsize,
        req.keep_alive ? "keep-alive" : "close");

    write_all(fd, hdr, (size_t)hdr_len);

    /* Send file body (skip for HEAD) */
    if (!is_head && fsize > 0) {
        off_t offset    = 0;
        off_t remaining = fsize;
        while (remaining > 0) {
            ssize_t sent = sendfile(fd, file_fd, &offset, (size_t)remaining);
            if (sent <= 0) {
                if (errno == EINTR) continue;
                break;  /* client closed — not an error on our side */
            }
            remaining -= sent;
        }
    }

    close(file_fd);
    return req.keep_alive;
}

/* -------------------------------------------------------------------------
 * Connection thread — handles keep-alive loop for one client
 * ---------------------------------------------------------------------- */

static void* conn_thread(void* arg) {
    int fd = (int)(intptr_t)arg;
    atomic_fetch_add(&g_conn_count, 1);

    /* Set receive timeout */
    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int requests = 0;
    while (g_running && requests < MAX_KEEPALIVE) {
        if (!handle_request(fd)) break;
        requests++;
    }

    close(fd);
    atomic_fetch_sub(&g_conn_count, 1);
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
    g_port = SKRTR_SERVE_DEFAULT_PORT;
    g_static_root[0] = '\0';

    /* --dir <path> --port <n> */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) {
            if (!realpath(argv[++i], g_static_root)) g_static_root[0] = '\0';
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            g_port = (int)strtol(argv[++i], NULL, 10);
        } else if (argv[i][0] != '-' && !g_static_root[0]) {
            /* positional: first non-flag arg is the directory */
            if (!realpath(argv[i], g_static_root)) g_static_root[0] = '\0';
        } else if (argv[i][0] != '-') {
            /* positional: subsequent non-flag arg is the port */
            if (!g_port) g_port = (int)strtol(argv[i], NULL, 10);
        }
    }

    if (!g_static_root[0]) {
        if (!realpath(".", g_static_root)) strncpy(g_static_root, ".", 2);
    }
    if (!g_port) g_port = SKRTR_SERVE_DEFAULT_PORT;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    parse_args(argc, argv);

    /* Verify static root exists */
    struct stat root_st;
    if (stat(g_static_root, &root_st) < 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr, "[serve] FATAL: static root '%s' is not a directory: %s\n",
                g_static_root, strerror(errno));
        return 1;
    }

    /* Bind TCP socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[serve] FATAL: socket: %s\n", strerror(errno));
        return 1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)g_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[serve] FATAL: bind port %d: %s\n",
                g_port, strerror(errno));
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        fprintf(stderr, "[serve] FATAL: listen: %s\n", strerror(errno));
        close(server_fd);
        return 1;
    }

    printf("[serve] Skr8tr static file server\n");
    printf("[serve] root: %s\n", g_static_root);
    printf("[serve] port: %d\n", g_port);
    printf("[serve] ready\n");

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!g_running) break;
            fprintf(stderr, "[serve] accept error: %s\n", strerror(errno));
            continue;
        }

        /* Detached thread per connection */
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, conn_thread, (void*)(intptr_t)client_fd);
        pthread_attr_destroy(&attr);
    }

    close(server_fd);
    printf("[serve] shutdown. connections served: (see access logs)\n");
    return 0;
}
