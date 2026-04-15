#!/usr/bin/env bash
# Sovereign Market — Launch all QEMU VMs
# Requires setup-network.sh to have been run first
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$SCRIPT_DIR/images"
BRIDGE="skr8tr-br0"
ALPINE_ISO="$VM_DIR/alpine-virt.iso"

launch_vm() {
  local name="$1"
  local mac="$2"
  local cpus="$3"
  local mem="$4"
  local disk="$VM_DIR/${name}.qcow2"
  local pidfile="/tmp/skr8tr-vm-${name}.pid"
  local logfile="/tmp/skr8tr-vm-${name}.log"

  if [ -f "$pidfile" ] && kill -0 "$(cat $pidfile)" 2>/dev/null; then
    echo "[skr8tr] $name already running (PID $(cat $pidfile))"
    return
  fi

  echo "[skr8tr] Launching $name..."
  qemu-system-x86_64 \
    -name "$name" \
    -machine type=q35,accel=kvm \
    -cpu host \
    -smp "$cpus" \
    -m "$mem" \
    -drive file="$disk",format=qcow2,if=virtio \
    -cdrom "$ALPINE_ISO" \
    -netdev bridge,id=net0,br="$BRIDGE" \
    -device virtio-net-pci,netdev=net0,mac="$mac" \
    -nographic \
    -serial mon:stdio \
    -pidfile "$pidfile" \
    -daemonize \
    2>"$logfile"

  echo "[skr8tr] $name launched. PID: $(cat $pidfile) | Log: $logfile"
}

launch_vm "conductor" "52:54:00:aa:bb:10" 2 1024
launch_vm "node1"     "52:54:00:aa:bb:11" 4 2048
launch_vm "node2"     "52:54:00:aa:bb:12" 4 2048
launch_vm "node3"     "52:54:00:aa:bb:13" 4 2048

echo ""
echo "[skr8tr] All VMs launched."
echo ""
echo "  conductor  10.10.0.10  — skr8tr_sched + skr8tr_reg"
echo "  node1      10.10.0.11  — workload node"
echo "  node2      10.10.0.12  — workload node"
echo "  node3      10.10.0.13  — workload node"
echo ""
echo "SSH in with: ssh root@10.10.0.10"
