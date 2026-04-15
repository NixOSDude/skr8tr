#!/usr/bin/env bash
# Sovereign Market — Create and provision QEMU VMs
# VMs: conductor (10.10.0.10), node1-3 (10.10.0.11-13)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$SCRIPT_DIR/images"
CLOUD_INIT_DIR="$SCRIPT_DIR/cloud-init"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-virt-3.21.3-x86_64.iso"
ALPINE_ISO="$VM_DIR/alpine-virt.iso"
BRIDGE="skr8tr-br0"

mkdir -p "$VM_DIR"

echo "[skr8tr] Downloading Alpine Linux virt ISO..."
if [ ! -f "$ALPINE_ISO" ]; then
  curl -L --progress-bar -o "$ALPINE_ISO" "$ALPINE_URL"
else
  echo "[skr8tr] Alpine ISO already present — skipping download"
fi

create_vm() {
  local name="$1"
  local ip="$2"
  local cpus="$3"
  local mem="$4"  # MiB
  local disk="$VM_DIR/${name}.qcow2"
  local mac="$5"

  echo "[skr8tr] Creating VM: $name ($ip, ${cpus}cpu, ${mem}MiB)"

  if [ -f "$disk" ]; then
    echo "[skr8tr] Disk $disk exists — skipping qemu-img create"
  else
    qemu-img create -f qcow2 "$disk" 8G
  fi

  # Write cloud-init user-data (Alpine doesn't use cloud-init but we embed setup via kernel args + answersfile)
  cat > "$CLOUD_INIT_DIR/${name}-answers" << ANSWERS
KEYMAPOPTS="us us"
HOSTNAMEOPTS="-n $name"
INTERFACESOPTS="auto lo
iface lo inet loopback
auto eth0
iface eth0 inet static
  address $ip
  netmask 255.255.255.0
  gateway 10.10.0.1
"
DNSOPTS="-d local -n 8.8.8.8"
TIMEZONEOPTS="-z UTC"
PROXYOPTS="none"
APKREPOSOPTS="-1"
SSHDOPTS="-c openssh"
NTPOPTS="-c chrony"
DISKOPTS="-m sys /dev/vda"
ANSWERS
}

# conductor: 2 cores, 1GB RAM
create_vm "conductor" "10.10.0.10" 2 1024 "52:54:00:aa:bb:10"
# node1: 4 cores, 2GB RAM
create_vm "node1"     "10.10.0.11" 4 2048 "52:54:00:aa:bb:11"
# node2: 4 cores, 2GB RAM
create_vm "node2"     "10.10.0.12" 4 2048 "52:54:00:aa:bb:12"
# node3: 4 cores, 2GB RAM
create_vm "node3"     "10.10.0.13" 4 2048 "52:54:00:aa:bb:13"

echo ""
echo "[skr8tr] VM disks created. Run launch-all.sh to start."
echo "[skr8tr] First boot: install Alpine manually, then run provision.sh"
