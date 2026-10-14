#!/bin/bash
# ==============================================================================
# deploy_to_sdcard.sh - Copy Packaged RG DS Plus App to a Mounted SD Card
# ==============================================================================
# Usage:
#   ./scripts/deploy_to_sdcard.sh [/path/to/mounted/sdcard]
#
# If no path is provided, this script auto-detects any mounted volume under
# /media/$USER, /run/media/$USER, or /mnt that contains a `Roms/APPS` or `Ports`
# directory (Partition 11 of the RG DS Plus TF1 card or TF2 card).
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PKG_DIR="${SDK_ROOT}/sdcard_package"

if [ ! -d "${PKG_DIR}/Roms/APPS" ]; then
    echo "[INFO] sdcard_package not found; building first..."
    make -C "${SDK_ROOT}" package
fi

TARGET_MOUNT="${1:-}"

if [ -z "${TARGET_MOUNT}" ]; then
    for base in "/media/${USER:-}" "/run/media/${USER:-}" "/mnt"; do
        [ -d "${base}" ] || continue
        for candidate in "${base}"/*; do
            [ -d "${candidate}" ] || continue
            if [ -d "${candidate}/Roms/APPS" ] || [ -d "${candidate}/Roms" ] || [ -d "${candidate}/Ports" ]; then
                TARGET_MOUNT="${candidate}"
                break 2
            fi
        done
    done
fi

if [ -z "${TARGET_MOUNT}" ] || [ ! -d "${TARGET_MOUNT}" ]; then
    echo "Usage: $0 </path/to/mounted/ROMS_partition>"
    echo ""
    echo "Could not auto-detect a mounted RG DS Plus SD card."
    echo "Tip: On the RG DS Plus, Partition 11 is the FAT32/exFAT ROMS partition."
    echo "     Insert the SD card into your Linux PC and pass its mount point, e.g.:"
    echo "       $0 /media/${USER:-user}/ROMS"
    exit 1
fi

echo "=== Deploying RG DS Plus Package to SD Card at: ${TARGET_MOUNT} ==="
mkdir -p "${TARGET_MOUNT}/Roms/APPS" "${TARGET_MOUNT}/Roms/APPS/Imgs" "${TARGET_MOUNT}/Ports"

cp -rv "${PKG_DIR}/Roms/APPS/"* "${TARGET_MOUNT}/Roms/APPS/"
cp -rv "${PKG_DIR}/Ports/"*     "${TARGET_MOUNT}/Ports/"

sync
echo "=== Sync complete! Safe to unmount ${TARGET_MOUNT}. ==="
echo "On your RG DS Plus, open 'App Center' -> 'APPS' (or 'Ports') to launch the program."
