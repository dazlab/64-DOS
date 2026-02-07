#!/usr/bin/env bash
set -euo pipefail

# build.sh
# Rebuild /init, inject into Buildroot rootfs image, regenerate VDI,
# and optionally swap it into a VirtualBox VM.

HERE="$(cd "$(dirname "$0")" && pwd)"

# Optional local overrides (not committed)
ENV_FILE="$HERE/local.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

# Defaults (override in init/local.env)
BR_DIR="${BR_DIR:-$HOME/buildroot}"
IMAGES_DIR="${IMAGES_DIR:-$BR_DIR/output/images}"
C_FILE="${C_FILE:-$HERE/init_shell.c}"

# Keep build outputs inside repo (clean + predictable)
BUILD_DIR="${BUILD_DIR:-$HERE/.build}"
INIT_OUT="${INIT_OUT:-$BUILD_DIR/init}"

ROOTFS_IMG="${ROOTFS_IMG:-$IMAGES_DIR/rootfs.ext2}"
NEW_VDI="${NEW_VDI:-$IMAGES_DIR/dosmodern_latest.vdi}"

VM_NAME="${VM_NAME:-64-DOS}"
STORAGE_CTL="${STORAGE_CTL:-AHCI}"    # must match VirtualBox controller name
ROOTFS_PORT="${ROOTFS_PORT:-1}"
ROOTFS_DEVICE="${ROOTFS_DEVICE:-0}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing: $1"; exit 1; }; }
need gcc
need VBoxManage
need sudo

[[ -f "$C_FILE" ]] || { echo "C source not found: $C_FILE"; exit 1; }
[[ -f "$ROOTFS_IMG" ]] || { echo "Rootfs image not found: $ROOTFS_IMG"; exit 1; }

mkdir -p "$BUILD_DIR"

echo "[1/4] Build init binary..."
gcc -Os -static -s -o "$INIT_OUT" "$C_FILE"

echo "[2/4] Inject /init into rootfs image..."
MNT="$(mktemp -d)"
cleanup() {
  sudo umount "$MNT" >/dev/null 2>&1 || true
  rmdir "$MNT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sudo mount -o loop "$ROOTFS_IMG" "$MNT"
sudo cp "$INIT_OUT" "$MNT/init"
sudo chmod +x "$MNT/init"
sync
sudo umount "$MNT"

echo "[3/4] Convert updated rootfs image to VDI..."
rm -f "$NEW_VDI"
VBoxManage convertfromraw "$ROOTFS_IMG" "$NEW_VDI" --format VDI >/dev/null

echo "[4/4] (Optional) Swap VDI into VM storage..."
if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
  KEY="\"${STORAGE_CTL}\"-${ROOTFS_PORT}-${ROOTFS_DEVICE}"

  CUR_UUID="$(VBoxManage showvminfo "$VM_NAME" --machinereadable | \
    awk -F= -v key="$KEY" '$1==key {gsub(/"/,"",$2); print $2}' | head -n1)"

  VBoxManage storageattach "$VM_NAME" \
    --storagectl "$STORAGE_CTL" \
    --port "$ROOTFS_PORT" \
    --device "$ROOTFS_DEVICE" \
    --type hdd \
    --medium "$NEW_VDI" >/dev/null

  if [[ -n "${CUR_UUID:-}" && "$CUR_UUID" != "none" ]]; then
    VBoxManage closemedium disk "$CUR_UUID" --delete >/dev/null 2>&1 || true
  fi

  echo "Updated VM '$VM_NAME' to use: $NEW_VDI"
else
  echo "VM '$VM_NAME' not found. VDI created at: $NEW_VDI"
fi

echo "Done."

