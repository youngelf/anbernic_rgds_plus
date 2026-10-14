#!/bin/bash
# ==============================================================================
# deploy_over_ssh.sh - Push & Optionally Launch an RG DS Plus App Over Wi-Fi/SSH
# ==============================================================================
# Firmware SSH Details (verified from RG-DS-PLUS-EN16GB-V1.0-20260915.IMG):
#   - Service:      /etc/init.d/S50sshd (OpenSSH sshd enabled at boot)
#   - Default Host: anbernic.local (Avahi mDNS enabled via S50avahi-daemon)
#   - Default User: root
#   - Default Pass: root (SHA-256 crypt hash in /etc/shadow)
#   - Target Paths: /mnt/mmc/Roms/APPS (TF1 internal SD) or /mnt/sdcard/Roms/APPS (TF2)
#
# Usage:
#   ./scripts/deploy_over_ssh.sh [RGDS_HOST_OR_IP] [--run]
#
# Examples:
#   ./scripts/deploy_over_ssh.sh 192.168.1.88
#   ./scripts/deploy_over_ssh.sh anbernic.local --run
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PKG_DIR="${SDK_ROOT}/sdcard_package"

RGDS_HOST="${1:-${RGDS_HOST:-anbernic.local}}"
RUN_AFTER="${2:-}"
RGDS_USER="${RGDS_USER:-root}"
REMOTE_ROMS="${REMOTE_ROMS:-/mnt/mmc}"

if [ ! -d "${PKG_DIR}/Roms/APPS" ]; then
    echo "[INFO] Building sdcard_package first..."
    make -C "${SDK_ROOT}" package
fi

echo "=== Deploying to RG DS Plus (${RGDS_USER}@${RGDS_HOST}:${REMOTE_ROMS}) ==="
echo "[NOTE] Default root password on RG DS Plus stock firmware is: root"

ssh "${RGDS_USER}@${RGDS_HOST}" "mkdir -p ${REMOTE_ROMS}/Roms/APPS/Imgs ${REMOTE_ROMS}/Ports"

scp -r "${PKG_DIR}/Roms/APPS/"* "${RGDS_USER}@${RGDS_HOST}:${REMOTE_ROMS}/Roms/APPS/"
scp -r "${PKG_DIR}/Ports/"*     "${RGDS_USER}@${RGDS_HOST}:${REMOTE_ROMS}/Ports/"

ssh "${RGDS_USER}@${RGDS_HOST}" "chmod +x ${REMOTE_ROMS}/Roms/APPS/*.sh ${REMOTE_ROMS}/Roms/APPS/*/* ${REMOTE_ROMS}/Ports/*/*.sh 2>/dev/null; sync"

echo "=== Deployment complete! ==="

if [ "${RUN_AFTER}" = "--run" ]; then
    APP_SH="$(basename "$(ls "${PKG_DIR}/Roms/APPS/"*.sh | head -n 1)")"
    echo "=== Launching ${REMOTE_ROMS}/Roms/APPS/${APP_SH} on device... ==="
    ssh -t "${RGDS_USER}@${RGDS_HOST}" "${REMOTE_ROMS}/Roms/APPS/${APP_SH}"
fi
