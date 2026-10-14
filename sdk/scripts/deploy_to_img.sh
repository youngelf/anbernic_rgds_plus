#!/bin/bash
# ==============================================================================
# deploy_to_img.sh - Inject Packaged App Directly Into an RG DS Plus .IMG File
# ==============================================================================
# Partition 11 of RG-DS-PLUS-EN16GB-V1.0-20260915.IMG is the FAT32 ROMS
# partition starting at sector 22511616 (byte offset = 22511616 * 512 = 11525947392).
# Uses `mtools` (mmd / mcopy) so no root/loop-mount privileges are required.
#
# Usage:
#   ./scripts/deploy_to_img.sh [/path/to/RG-DS-PLUS.IMG]
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PKG_DIR="${SDK_ROOT}/sdcard_package"
DEFAULT_IMG="${HOME}/xkdr/rg_ds_plus/RG-DS-PLUS-EN16GB-V1.0-20260915.IMG"

IMG_PATH="${1:-${DEFAULT_IMG}}"
ROMS_OFFSET=$((22511616 * 512))

if [ ! -f "${IMG_PATH}" ]; then
    echo "Usage: $0 </path/to/RG-DS-PLUS.IMG>"
    echo "Error: Image file not found at ${IMG_PATH}"
    exit 1
fi

if ! command -v mcopy >/dev/null 2>&1; then
    echo "Error: mtools (mcopy/mmd) is required. Install with: sudo apt-get install mtools"
    exit 1
fi

if [ ! -d "${PKG_DIR}/Roms/APPS" ]; then
    make -C "${SDK_ROOT}" package
fi

IMG_SPEC="${IMG_PATH}@@${ROMS_OFFSET}"
echo "=== Injecting package into ${IMG_PATH} (Partition 11 @ offset ${ROMS_OFFSET}) ==="

mmd -D s -i "${IMG_SPEC}" ::/Roms/APPS/hello_rgds 2>/dev/null || true
mmd -D s -i "${IMG_SPEC}" ::/Ports 2>/dev/null || true
mmd -D s -i "${IMG_SPEC}" ::/Ports/Hello_RGDS 2>/dev/null || true

mcopy -o -i "${IMG_SPEC}" "${PKG_DIR}/Roms/APPS/Hello_RGDS.sh"       ::/Roms/APPS/Hello_RGDS.sh
mcopy -o -i "${IMG_SPEC}" "${PKG_DIR}/Roms/APPS/Imgs/Hello_RGDS.png" ::/Roms/APPS/Imgs/Hello_RGDS.png
mcopy -o -i "${IMG_SPEC}" "${PKG_DIR}/Roms/APPS/hello_rgds/"*        ::/Roms/APPS/hello_rgds/
mcopy -o -i "${IMG_SPEC}" "${PKG_DIR}/Ports/Hello_RGDS/"*            ::/Ports/Hello_RGDS/

echo "=== Verification: Listing ::/Roms/APPS in image ==="
mdir -i "${IMG_SPEC}" ::/Roms/APPS
