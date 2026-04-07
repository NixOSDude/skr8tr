#!/usr/bin/env bash
# SovereignMarket — Build all components
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="/home/sbaker/.local/bin:$PATH"
export PATH="/home/sbaker/.cargo/bin:$PATH"

echo "=============================================="
echo "  SovereignMarket — Full Build"
echo "=============================================="

# 1. Build Rust microservices
echo ""
echo "=== [1/3] Building Rust microservices ==="
cd "$SCRIPT_DIR/services"
cargo build --release
echo "  Binaries: $SCRIPT_DIR/services/target/release/"

# 2. Build Angular frontend
echo ""
echo "=== [2/3] Building Angular frontend ==="
cd "$SCRIPT_DIR/frontend/sovereign-market"
npm install --silent
ng build --configuration production --output-path "$SCRIPT_DIR/frontend/sovereign-market/dist/sovereign-market"
echo "  Static files: $SCRIPT_DIR/frontend/sovereign-market/dist/sovereign-market/browser/"

# 3. Stage for deployment
echo ""
echo "=== [3/3] Staging deploy artifacts ==="
STAGE="$SCRIPT_DIR/dist"
mkdir -p "$STAGE/bin" "$STAGE/www"

for svc in product-svc inventory-svc search-svc cart-svc order-svc user-svc review-svc recommendation-svc frontend-svc; do
  cp "$SCRIPT_DIR/services/target/release/$svc" "$STAGE/bin/"
done

cp -r "$SCRIPT_DIR/frontend/sovereign-market/dist/sovereign-market/browser/." "$STAGE/www/"

echo ""
echo "=============================================="
echo "  Build complete."
echo "  Binaries: $STAGE/bin/"
echo "  Static:   $STAGE/www/"
echo ""
echo "  Next steps:"
echo "  1. sudo vm/setup-network.sh"
echo "  2. vm/create-vms.sh"
echo "  3. vm/launch-all.sh"
echo "  4. (install Alpine on each VM)"
echo "  5. vm/provision.sh"
echo "  6. vm/start-cluster.sh"
echo "=============================================="
