#!/usr/bin/env bash
# Sovereign Market — Start skr8tr cluster and deploy all services
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MARKET_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFESTS="$MARKET_ROOT/manifests"
SKR8TR="$HOME/.cargo/bin/skr8tr"
KEY="$HOME/.skr8tr/signing.sec"

SSH_OPTS="-o StrictHostKeyChecking=no"
CONDUCTOR="root@10.10.0.10"
NODE1="root@10.10.0.11"
NODE2="root@10.10.0.12"
NODE3="root@10.10.0.13"

echo "=== Starting Tower (skr8tr_reg) ==="
ssh $SSH_OPTS "$CONDUCTOR" 'pkill skr8tr_reg 2>/dev/null; nohup skr8tr_reg > /tmp/tower.log 2>&1 &'
sleep 1

echo "=== Starting Conductor (skr8tr_sched) ==="
ssh $SSH_OPTS "$CONDUCTOR" 'pkill skr8tr_sched 2>/dev/null; nohup skr8tr_sched --pubkey /usr/local/bin/skrtrview.pub > /tmp/conductor.log 2>&1 &'
sleep 1

echo "=== Starting Fleet Nodes ==="
for node in $NODE1 $NODE2 $NODE3; do
  ssh $SSH_OPTS "$node" 'pkill skr8tr_node 2>/dev/null; nohup skr8tr_node > /tmp/node.log 2>&1 &'
  echo "  Started node on $node"
done
sleep 3

echo ""
echo "=== Verifying cluster ==="
"$SKR8TR" ping
"$SKR8TR" nodes

echo ""
echo "=== Deploying SovereignMarket services ==="
for manifest in \
  "$MANIFESTS/product-svc.skr8tr" \
  "$MANIFESTS/inventory-svc.skr8tr" \
  "$MANIFESTS/search-svc.skr8tr" \
  "$MANIFESTS/cart-svc.skr8tr" \
  "$MANIFESTS/order-svc.skr8tr" \
  "$MANIFESTS/user-svc.skr8tr" \
  "$MANIFESTS/review-svc.skr8tr" \
  "$MANIFESTS/recommendation-svc.skr8tr" \
  "$MANIFESTS/frontend.skr8tr"; do
  name=$(basename "$manifest" .skr8tr)
  echo "  Deploying $name..."
  "$SKR8TR" --key "$KEY" up "$manifest"
  sleep 0.5
done

echo ""
echo "=== Deployment complete ==="
"$SKR8TR" list
echo ""
echo "  SovereignMarket is live at http://10.10.0.13:4200"
echo "  API gateway:              http://10.10.0.10:80"
