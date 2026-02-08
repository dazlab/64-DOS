#!/usr/bin/env bash
set -euo pipefail

# build.sh
# Rebuild /init, build COM64 programs (C or ASM), inject into Buildroot rootfs image,
# regenerate VDI, and optionally swap it into a VirtualBox VM.

HERE="$(cd "$(dirname "$0")/.." && pwd)"

# Optional local overrides (not committed)
ENV_FILE="$HERE/init/local.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

# Defaults
BR_DIR="${BR_DIR:-$HOME/buildroot}"
IMAGES_DIR="${IMAGES_DIR:-$BR_DIR/output/images}"
C_FILE="${C_FILE:-$HERE/init/init_shell.c}"

# Build outputs inside repo
BUILD_DIR="${BUILD_DIR:-$HERE/.build}"
INIT_OUT="${INIT_OUT:-$BUILD_DIR/init}"

# C:\ payload folder (host) -> injected into /dos/c in image
DOS_C_SRC="${DOS_C_SRC:-$HERE/dos_c}"

# COM64 inputs
COM64_SRC_DIR="${COM64_SRC_DIR:-$HERE/com64}"        # *.S and *.c live here
TOOLS_DIR="${TOOLS_DIR:-$HERE/tools}"
MKCOM64_C="${MKCOM64_C:-$TOOLS_DIR/mkcom64.c}"
MKCOM64_BIN="${MKCOM64_BIN:-$BUILD_DIR/mkcom64}"

ROOTFS_IMG="${ROOTFS_IMG:-$IMAGES_DIR/rootfs.ext2}"

VM_NAME="${VM_NAME:-64-DOS}"
STORAGE_CTL="${STORAGE_CTL:-AHCI}"    # must match VirtualBox controller name
ROOTFS_PORT="${ROOTFS_PORT:-1}"
ROOTFS_DEVICE="${ROOTFS_DEVICE:-0}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing: $1"; exit 1; }; }
need gcc
need ld
need VBoxManage
need sudo

[[ -f "$C_FILE" ]] || { echo "C source not found: $C_FILE"; exit 1; }
[[ -f "$ROOTFS_IMG" ]] || { echo "Rootfs image not found: $ROOTFS_IMG"; exit 1; }

mkdir -p "$BUILD_DIR"
mkdir -p "$DOS_C_SRC"

build_mkcom64() {
  [[ -f "$MKCOM64_C" ]] || { echo "mkcom64.c not found: $MKCOM64_C"; exit 1; }
  echo "  building mkcom64..."
  gcc -O2 -s -o "$MKCOM64_BIN" "$MKCOM64_C"
}

# Build one COM64 from an object file
wrap_obj_to_com64() {
  local base="$1"
  local obj="$2"
  local bin="$BUILD_DIR/${base}.bin"
  local out="$DOS_C_SRC/${base}.COM64"

  # Link as a flat binary image with entry com64_main
  ld -nostdlib -e com64_main -Ttext=0x0 --oformat=binary -o "$bin" "$obj"

  # Wrap with COM64 header (entry_rva=0, bss_size=0 for now)
  "$MKCOM64_BIN" "$bin" "$out" 0 0
}

build_com64_programs() {
  # Build mkcom64 if missing/outdated
  if [[ ! -x "$MKCOM64_BIN" ]] || [[ "$MKCOM64_C" -nt "$MKCOM64_BIN" ]]; then
    build_mkcom64
  fi

  shopt -s nullglob
  local any=0

  # Build C sources (freestanding, no libc)
  for src in "$COM64_SRC_DIR"/*.c; do
    any=1
    local base obj
    base="$(basename "$src" .c)"
    obj="$BUILD_DIR/${base}.o"

    echo "  COM64 (C): $base"
    gcc -c -O2 -ffreestanding -fno-pic -fno-pie -nostdlib \
        -fno-asynchronous-unwind-tables -fno-unwind-tables \
        -o "$obj" "$src"

    wrap_obj_to_com64 "$base" "$obj"
  done

  # Build ASM sources
  for src in "$COM64_SRC_DIR"/*.S; do
    any=1
    local base obj
    base="$(basename "$src" .S)"
    obj="$BUILD_DIR/${base}.o"

    echo "  COM64 (ASM): $base"
    gcc -c -nostdlib -fno-pic -fno-pie -o "$obj" "$src"

    wrap_obj_to_com64 "$base" "$obj"
  done

  shopt -u nullglob

  if [[ "$any" -eq 0 ]]; then
    echo "  (no COM64 sources found in $COM64_SRC_DIR)"
  fi
}

echo "[1/6] Build init binary..."
gcc -Os -static -s -o "$INIT_OUT" "$C_FILE"

echo "[2/6] Build COM64 programs into dos_c/ ..."
build_com64_programs

echo "[3/6] Inject /init and C: payload into rootfs image..."
MNT="$(mktemp -d)"
cleanup() {
  sudo umount "$MNT" >/dev/null 2>&1 || true
  rmdir "$MNT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sudo mount -o loop "$ROOTFS_IMG" "$MNT"

# Inject init
sudo cp "$INIT_OUT" "$MNT/init"
sudo chmod +x "$MNT/init"

# Inject C:\ contents (anything in ./dos_c -> /dos/c)
sudo mkdir -p "$MNT/dos/c"
sudo cp -a "$DOS_C_SRC/." "$MNT/dos/c/"

sync
sudo umount "$MNT"

echo "[4/6] Detach old rootfs disk (so it's not locked)..."
CUR_UUID=""
if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
  KEY="\"${STORAGE_CTL}\"-${ROOTFS_PORT}-${ROOTFS_DEVICE}"
  CUR_UUID="$(VBoxManage showvminfo "$VM_NAME" --machinereadable | \
    awk -F= -v key="$KEY" '$1==key {gsub(/"/,"",$2); print $2}' | head -n1)"

  VBoxManage storageattach "$VM_NAME" \
    --storagectl "$STORAGE_CTL" \
    --port "$ROOTFS_PORT" \
    --device "$ROOTFS_DEVICE" \
    --type hdd \
    --medium none >/dev/null
fi

echo "[5/6] Convert updated rootfs image to a new VDI..."
STAMP="$(date +%Y%m%d-%H%M%S)"
NEW_VDI="$IMAGES_DIR/dosmodern_${STAMP}.vdi"
VBoxManage convertfromraw "$ROOTFS_IMG" "$NEW_VDI" --format VDI >/dev/null
VBoxManage internalcommands sethduuid "$NEW_VDI" >/dev/null

echo "[6/6] Attach new VDI and unregister the old one..."
if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
  VBoxManage storageattach "$VM_NAME" \
    --storagectl "$STORAGE_CTL" \
    --port "$ROOTFS_PORT" \
    --device "$ROOTFS_DEVICE" \
    --type hdd \
    --medium "$NEW_VDI" >/dev/null

  if [[ -n "${CUR_UUID:-}" && "$CUR_UUID" != "none" ]]; then
    VBoxManage closemedium disk "$CUR_UUID" --delete >/dev/null 2>&1 || true
  fi
fi

echo "Using VDI: $NEW_VDI"
echo "C: payload source: $DOS_C_SRC"
echo "Done."

