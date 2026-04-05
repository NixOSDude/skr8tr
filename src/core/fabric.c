/*
 * fabric.c — Skr8tr Mesh Fabric — UDP Primitive Layer
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every daemon uses this layer.
 */

#include "fabric.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int fabric_bind(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }

    return sock;
}

int fabric_send(int sock, const char* ip, int port,
                const void* msg, size_t len) {
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, ip, &dest.sin_addr) <= 0) return -1;

    return (int)sendto(sock, msg, len, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
}

int fabric_broadcast(int sock, int port, const void* msg, size_t len) {
    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_BROADCAST,
    };
    return (int)sendto(sock, msg, len, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
}

int fabric_recv(int sock, void* buf, size_t len, FabricAddr* src) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    int n = (int)recvfrom(sock, buf, len, 0,
                          (struct sockaddr*)&from, &from_len);
    if (n < 0) return -1;

    if (src) {
        inet_ntop(AF_INET, &from.sin_addr, src->ip, sizeof(src->ip));
        src->port = ntohs(from.sin_port);
    }

    return n;
}
