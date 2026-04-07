#!/usr/bin/env bash
# Sovereign Market — VM bridge network setup
# Creates skr8tr-br0 on 10.10.0.1/24 for QEMU VMs
set -euo pipefail

BRIDGE="skr8tr-br0"
BRIDGE_IP="10.10.0.1/24"

echo "[skr8tr] Setting up VM bridge: $BRIDGE"

if ip link show "$BRIDGE" &>/dev/null; then
  echo "[skr8tr] Bridge $BRIDGE already exists — skipping create"
else
  ip link add name "$BRIDGE" type bridge
  ip link set "$BRIDGE" up
  ip addr add "$BRIDGE_IP" dev "$BRIDGE"
  echo "[skr8tr] Bridge created: $BRIDGE @ $BRIDGE_IP"
fi

# Enable IP forwarding
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# NAT: VMs can reach the internet via host
HOST_IF=$(ip route get 8.8.8.8 | awk '{print $5; exit}')
iptables -t nat -C POSTROUTING -s 10.10.0.0/24 -o "$HOST_IF" -j MASQUERADE 2>/dev/null || \
  iptables -t nat -A POSTROUTING -s 10.10.0.0/24 -o "$HOST_IF" -j MASQUERADE
iptables -C FORWARD -i "$BRIDGE" -j ACCEPT 2>/dev/null || \
  iptables -A FORWARD -i "$BRIDGE" -j ACCEPT

echo "[skr8tr] Network ready. Bridge: $BRIDGE | Host: 10.10.0.1"
echo "[skr8tr] VM IPs: conductor=10.10.0.10  node1=10.10.0.11  node2=10.10.0.12  node3=10.10.0.13"
