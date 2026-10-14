#!/bin/bash
# ==============================================================================
# setup_host.sh - Install or Provision Linux Host Dependencies for RG DS Plus SDK
# ==============================================================================
# Supports:
#   1. System installation via `sudo apt-get` (Debian/Ubuntu/gLinux)
#   2. Rootless local toolchain provisioning into ~/.cache/aarch64_toolchain
#      using `apt-get download` + `dpkg-deb -x` if you prefer not to touch /usr.
# ==============================================================================

set -euo pipefail

TOOLCHAIN_CACHE="${HOME}/.cache/aarch64_toolchain"

echo "=== Anbernic RG DS Plus C SDK - Host Dependency Setup ==="

# 1. Check host x86_64 X11 development headers (for local dual-screen simulator)
if [ -f /usr/include/X11/Xlib.h ]; then
    echo "[OK] Host X11 headers found (/usr/include/X11/Xlib.h)"
else
    echo "[INFO] Installing libx11-dev for the Linux desktop simulator..."
    sudo apt-get update && sudo apt-get install -y build-essential libx11-dev
fi

# 2. Check for aarch64-linux-gnu-gcc (either system-wide or in ~/.cache/aarch64_toolchain)
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    echo "[OK] System aarch64 cross-compiler found: $(command -v aarch64-linux-gnu-gcc)"
    exit 0
fi

if [ -x "${TOOLCHAIN_CACHE}/usr/bin/aarch64-linux-gnu-gcc-14" ]; then
    echo "[OK] Cached aarch64 cross-compiler found at ${TOOLCHAIN_CACHE}/usr/bin/aarch64-linux-gnu-gcc-14"
    exit 0
fi

echo "[INFO] Installing aarch64 cross-compiler..."
if [ "${1:-}" = "--user" ]; then
    echo "[INFO] Provisioning user-space toolchain in ${TOOLCHAIN_CACHE}..."
    mkdir -p "${TOOLCHAIN_CACHE}/debs"
    cd "${TOOLCHAIN_CACHE}/debs"
    apt-get download \
        binutils-aarch64-linux-gnu \
        cpp-14-aarch64-linux-gnu \
        gcc-14-aarch64-linux-gnu \
        libgcc-14-dev-arm64-cross \
        libc6-arm64-cross \
        libc6-dev-arm64-cross \
        linux-libc-dev-arm64-cross
    for deb in *.deb; do
        dpkg-deb -x "${deb}" "${TOOLCHAIN_CACHE}"
    done
    echo "[OK] User-space toolchain ready at ${TOOLCHAIN_CACHE}"
else
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        libx11-dev \
        rsync \
        openssh-client
    echo "[OK] System-wide aarch64 toolchain and host libraries installed."
fi
