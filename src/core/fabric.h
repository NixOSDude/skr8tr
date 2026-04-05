/*
 * fabric.h — Skr8tr Mesh Fabric — UDP Primitive Layer
 * Skr8tr Sovereign Workload Orchestrator
 *
 * SSoA LEVEL 1 — Foundation Anchor
 * Mutability: Extreme caution. Every daemon uses this layer.
 *
 * Provides the bare UDP socket primitives that every Skr8tr daemon
 * builds on. No framing, no reliability, no ordering — sovereign UDP.
 */

#pragma once

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Maximum UDP datagram payload Skr8tr will send or receive */
#define FABRIC_MTU 8192

/* Source address filled in by fabric_recv */
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int  port;
} FabricAddr;

/*
 * fabric_bind — Create a UDP socket bound to `port` on all interfaces.
 * Enables SO_REUSEADDR and SO_BROADCAST.
 * Returns socket fd on success, -1 on error (errno set).
 */
int fabric_bind(int port);

/*
 * fabric_send — Send `len` bytes of `msg` to `ip:port`.
 * Returns bytes sent, or -1 on error.
 */
int fabric_send(int sock, const char* ip, int port,
                const void* msg, size_t len);

/*
 * fabric_broadcast — Broadcast `msg` to the subnet on `port`.
 * Uses 255.255.255.255; requires SO_BROADCAST on socket.
 * Returns bytes sent, or -1 on error.
 */
int fabric_broadcast(int sock, int port, const void* msg, size_t len);

/*
 * fabric_recv — Blocking receive into `buf` (up to `len` bytes).
 * Populates `src` with sender address if non-NULL.
 * Returns bytes received, or -1 on error.
 */
int fabric_recv(int sock, void* buf, size_t len, FabricAddr* src);
