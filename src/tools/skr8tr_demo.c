/* skr8tr_demo.c — SSoA Level 3 — REPO: oss
 * Live mesh status demo server.
 *
 * Listens on port 8080. Each HTTP request runs skr8tr CLI commands
 * against the conductor and returns a styled live status page.
 * Auto-refreshes every 5 seconds. Designed to be deployed as a
 * Skr8tr workload — the orchestrator serving its own demo.
 *
 * Build: gcc -O2 -std=gnu2x -o bin/skr8tr_demo src/tools/skr8tr_demo.c -lpthread
 * Run:   skr8tr_demo [--host <conductor-ip>] [--port <port>]
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define DEFAULT_PORT      8080
#define DEFAULT_CONDUCTOR "127.0.0.1"
#define BACKLOG           16
#define BUF_MAX           65536
#define CMD_OUT_MAX       8192
#define REFRESH_S         5

static char g_conductor[64] = DEFAULT_CONDUCTOR;
static int  g_port           = DEFAULT_PORT;

/* -------------------------------------------------------------------------
 * Run a shell command and capture stdout into buf (null-terminated).
 * Returns number of bytes written (excluding null terminator).
 * ---------------------------------------------------------------------- */
static int run_cmd(const char* cmd, char* buf, int bufsz) {
    FILE* f = popen(cmd, "r");
    if (!f) { snprintf(buf, bufsz, "(command failed)"); return 0; }
    int n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && n < bufsz - 1)
        buf[n++] = (char)c;
    buf[n] = '\0';
    pclose(f);
    return n;
}

/* -------------------------------------------------------------------------
 * HTML-escape a string into dst (returns dst).
 * ---------------------------------------------------------------------- */
static char* html_escape(const char* src, char* dst, int dstsz) {
    int di = 0;
    for (int si = 0; src[si] && di < dstsz - 6; si++) {
        switch (src[si]) {
            case '<':  memcpy(dst+di, "&lt;",   4); di += 4; break;
            case '>':  memcpy(dst+di, "&gt;",   4); di += 4; break;
            case '&':  memcpy(dst+di, "&amp;",  5); di += 5; break;
            case '"':  memcpy(dst+di, "&quot;", 6); di += 6; break;
            default:   dst[di++] = src[si];          break;
        }
    }
    dst[di] = '\0';
    return dst;
}

/* -------------------------------------------------------------------------
 * Build the full HTML response body.
 * ---------------------------------------------------------------------- */
static int build_page(char* out, int outsz) {
    char cmd[256];
    char raw_ping[CMD_OUT_MAX], raw_nodes[CMD_OUT_MAX], raw_list[CMD_OUT_MAX];
    char esc_ping[CMD_OUT_MAX], esc_nodes[CMD_OUT_MAX], esc_list[CMD_OUT_MAX];

    snprintf(cmd, sizeof(cmd),
             "skr8tr --host %s ping 2>&1", g_conductor);
    run_cmd(cmd, raw_ping, sizeof(raw_ping));

    snprintf(cmd, sizeof(cmd),
             "skr8tr --host %s nodes 2>&1", g_conductor);
    run_cmd(cmd, raw_nodes, sizeof(raw_nodes));

    snprintf(cmd, sizeof(cmd),
             "skr8tr --host %s list 2>&1", g_conductor);
    run_cmd(cmd, raw_list, sizeof(raw_list));

    html_escape(raw_ping,  esc_ping,  sizeof(esc_ping));
    html_escape(raw_nodes, esc_nodes, sizeof(esc_nodes));
    html_escape(raw_list,  esc_list,  sizeof(esc_list));

    /* Timestamp */
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));

    int n = snprintf(out, outsz,
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"utf-8\">\n"
"  <meta http-equiv=\"refresh\" content=\"%d\">\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"  <title>Skr8tr — Live Mesh Status</title>\n"
"  <style>\n"
"    :root {\n"
"      --bg: #0d0f14;\n"
"      --card: #141720;\n"
"      --border: #1e2330;\n"
"      --accent: #00d4ff;\n"
"      --green: #00ff9d;\n"
"      --text: #c8d0e0;\n"
"      --muted: #556070;\n"
"      --font: 'Courier New', Courier, monospace;\n"
"    }\n"
"    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
"    body { background: var(--bg); color: var(--text); font-family: var(--font);\n"
"           min-height: 100vh; padding: 2rem; }\n"
"    header { border-bottom: 1px solid var(--border); padding-bottom: 1.5rem;\n"
"             margin-bottom: 2rem; display: flex; align-items: baseline;\n"
"             justify-content: space-between; flex-wrap: wrap; gap: 1rem; }\n"
"    header h1 { font-size: 1.6rem; color: var(--accent); letter-spacing: 0.05em; }\n"
"    header h1 span { color: var(--green); }\n"
"    .tagline { color: var(--muted); font-size: 0.8rem; }\n"
"    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));\n"
"            gap: 1.5rem; }\n"
"    .card { background: var(--card); border: 1px solid var(--border);\n"
"            border-radius: 6px; padding: 1.25rem; }\n"
"    .card h2 { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.1em;\n"
"               color: var(--accent); margin-bottom: 1rem;\n"
"               padding-bottom: 0.5rem; border-bottom: 1px solid var(--border); }\n"
"    pre { font-size: 0.8rem; line-height: 1.6; white-space: pre-wrap;\n"
"          word-break: break-all; color: var(--green); }\n"
"    .ping pre { color: var(--accent); }\n"
"    footer { margin-top: 2rem; padding-top: 1rem; border-top: 1px solid var(--border);\n"
"             font-size: 0.72rem; color: var(--muted);\n"
"             display: flex; justify-content: space-between; flex-wrap: wrap; gap: 0.5rem; }\n"
"    .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%%;\n"
"           background: var(--green); margin-right: 6px;\n"
"           animation: pulse 1.5s ease-in-out infinite; }\n"
"    @keyframes pulse { 0%%,100%% { opacity:1; } 50%% { opacity:0.3; } }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <header>\n"
"    <h1>Skr8<span>tr</span> &mdash; Live Mesh</h1>\n"
"    <span class=\"tagline\">masterless &bull; PQC-native &bull; container-free &bull; 15MB</span>\n"
"  </header>\n"
"  <div class=\"grid\">\n"
"    <div class=\"card ping\">\n"
"      <h2><span class=\"dot\"></span>Conductor</h2>\n"
"      <pre>%s</pre>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <h2>Worker Nodes</h2>\n"
"      <pre>%s</pre>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <h2>Running Workloads</h2>\n"
"      <pre>%s</pre>\n"
"    </div>\n"
"  </div>\n"
"  <footer>\n"
"    <span>Auto-refresh every %ds &mdash; served by Skr8tr</span>\n"
"    <span>%s</span>\n"
"  </footer>\n"
"</body>\n"
"</html>\n",
        REFRESH_S,
        esc_ping, esc_nodes, esc_list,
        REFRESH_S, ts);

    return n;
}

/* -------------------------------------------------------------------------
 * Handle one HTTP connection in its own thread.
 * ---------------------------------------------------------------------- */
static void* handle_conn(void* arg) {
    int fd = *(int*)arg;
    free(arg);

    /* Read request (we don't care about the content, just drain it) */
    char req[2048];
    recv(fd, req, sizeof(req) - 1, 0);

    /* Build page */
    char* body = malloc(BUF_MAX);
    if (!body) { close(fd); return NULL; }
    int blen = build_page(body, BUF_MAX);

    /* Write HTTP response */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", blen);

    send(fd, header, hlen, 0);
    send(fd, body, blen, 0);

    free(body);
    close(fd);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc) {
            if (!strcmp(argv[i], "--host") || !strcmp(argv[i], "--conductor"))
                snprintf(g_conductor, sizeof(g_conductor), "%s", argv[i+1]);
            if (!strcmp(argv[i], "--port"))
                g_port = atoi(argv[i+1]);
        }
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)g_port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[demo] FATAL: cannot bind port %d: %s\n",
                g_port, strerror(errno));
        return 1;
    }
    listen(srv, BACKLOG);
    printf("[demo] live mesh status at http://0.0.0.0:%d  conductor=%s\n",
           g_port, g_conductor);

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(srv, (struct sockaddr*)&client, &clen);
        if (cfd < 0) continue;

        int* pfd = malloc(sizeof(int));
        *pfd = cfd;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, handle_conn, pfd);
        pthread_attr_destroy(&attr);
    }
}
