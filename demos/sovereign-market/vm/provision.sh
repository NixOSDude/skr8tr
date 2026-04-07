#!/usr/bin/env bash
# Sovereign Market — Provision skr8tr binaries + market services onto VMs
# Run this after Alpine is installed and SSH is reachable on all VMs
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"  # /home/sbaker/skr8tr
MARKET_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
SERVICES_BIN="$MARKET_ROOT/services/target/release"

CONDUCTOR="root@10.10.0.10"
NODE1="root@10.10.0.11"
NODE2="root@10.10.0.12"
NODE3="root@10.10.0.13"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"

scp_bin() {
  local host="$1"
  shift
  for bin in "$@"; do
    echo "[provision] → $host: $bin"
    scp $SSH_OPTS "$bin" "${host}:/usr/local/bin/"
  done
}

setup_node() {
  local host="$1"
  echo "[provision] Setting up $host..."
  ssh $SSH_OPTS "$host" 'apk update && apk add --no-cache libgcc && mkdir -p /opt/sovereign-market/bin /opt/sovereign-market/www'
}

echo "=== Provisioning conductor ($CONDUCTOR) ==="
setup_node "$CONDUCTOR"
scp_bin "$CONDUCTOR" \
  "$BIN_DIR/skr8tr_sched" \
  "$BIN_DIR/skr8tr_reg" \
  "$PROJECT_ROOT/skrtrview.pub"

echo ""
echo "=== Provisioning node1 ($NODE1) ==="
setup_node "$NODE1"
scp_bin "$NODE1" "$BIN_DIR/skr8tr_node"
scp $SSH_OPTS "$SERVICES_BIN/product-svc"    "$NODE1:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/inventory-svc"  "$NODE1:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/search-svc"     "$NODE1:/opt/sovereign-market/bin/"

echo ""
echo "=== Provisioning node2 ($NODE2) ==="
setup_node "$NODE2"
scp_bin "$NODE2" "$BIN_DIR/skr8tr_node"
scp $SSH_OPTS "$SERVICES_BIN/cart-svc"       "$NODE2:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/order-svc"      "$NODE2:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/user-svc"       "$NODE2:/opt/sovereign-market/bin/"

echo ""
echo "=== Provisioning node3 ($NODE3) ==="
setup_node "$NODE3"
scp_bin "$NODE3" "$BIN_DIR/skr8tr_node"
scp $SSH_OPTS "$SERVICES_BIN/review-svc"         "$NODE3:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/recommendation-svc" "$NODE3:/opt/sovereign-market/bin/"
scp $SSH_OPTS "$SERVICES_BIN/frontend-svc"        "$NODE3:/opt/sovereign-market/bin/"

echo ""
echo "[provision] All binaries deployed."
echo "[provision] Next: run start-cluster.sh"
