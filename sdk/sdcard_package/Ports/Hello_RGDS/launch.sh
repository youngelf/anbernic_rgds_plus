#!/bin/bash
# ==============================================================================
# App_Template.sh - Launcher Script for Anbernic RG DS Plus (Roms/APPS & Ports)
# ==============================================================================
# How dmenu.bin launches apps on RG DS Plus (verified from /mnt/vendor/bin/dmenu.bin):
#   1. Scans /mnt/mmc/Roms/APPS/*.sh and /mnt/sdcard/Roms/APPS/*.sh
#      (or /mnt/mmc/Ports/<App>/launch.sh).
#   2. Writes `/mnt/vendor/subscreen/launch.sh apk apps 0 &` followed by this
#      script's path into `/tmp/.next`, which `/mnt/vendor/ctrl/dmenu_ln` executes.
#   3. Therefore, any dual-screen C application MUST set
#      `/sys/class/anbernic_misc/runapp` to `0` and kill `subscreen.dge` so the
#      background subscreen daemon releases Weston Display 1 (Lower 1024x768 LCD).
# ==============================================================================

APP_BIN="hello_rgds"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${SCRIPT_DIR}/${APP_BIN}"

if [ -x "${SCRIPT_DIR}/${APP_BIN}" ] && [ ! -d "${SCRIPT_DIR}/${APP_BIN}" ]; then
    APP_DIR="${SCRIPT_DIR}"
else
    for candidate in \
        "${APP_DIR}" \
        "/mnt/mmc/Roms/APPS/${APP_BIN}" \
        "/mnt/sdcard/Roms/APPS/${APP_BIN}" \
        "/mnt/mmc/Ports/${APP_BIN}" \
        "/mnt/sdcard/Ports/${APP_BIN}"; do
        if [ -d "${candidate}" ] && [ -x "${candidate}/${APP_BIN}" ]; then
            APP_DIR="${candidate}"
            break
        fi
    done
fi

# 1. Stop subscreen.dge so our C program owns both 1024x768 displays, then restore runapp=1
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 0 > /sys/class/anbernic_misc/runapp
fi
killall -9 subscreen.dge 2>/dev/null
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 1 > /sys/class/anbernic_misc/runapp
fi

# 2. Power on both 1024x768 LCDs if user had single-screen mode (lcdoff != 0) active
SAVED_LCDOFF=""
if [ -r /sys/class/anbernic_misc/lcdoff ]; then
    SAVED_LCDOFF="$(cat /sys/class/anbernic_misc/lcdoff 2>/dev/null)"
    if [ "${SAVED_LCDOFF}" != "0" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
        echo 0 > /sys/class/anbernic_misc/lcdoff
        [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
    fi
fi

# 3. Configure Weston/Wayland SDL2 environment (matching Roms/APPS/Clock.sh)
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
if [ -d /var/run/0-runtime-dir ]; then
    export XDG_RUNTIME_DIR="/var/run/0-runtime-dir"
else
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
fi
export SDL_VIDEO_DOUBLE_BUFFER=1
export SDL_RENDER_VSYNC=1

if [ -r /sys/class/anbernic_misc/lcdswap ]; then
    if [ "$(cat /sys/class/anbernic_misc/lcdswap 2>/dev/null)" = "1" ]; then
        export SDL2_SWAP_LCD=1
    fi
fi

cd "${APP_DIR}" || exit 1
chmod +x "./${APP_BIN}" 2>/dev/null
"./${APP_BIN}"

# 4. Restore previous single-screen mode on exit if it was active
if [ -n "${SAVED_LCDOFF}" ] && [ "${SAVED_LCDOFF}" != "0" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
    echo "${SAVED_LCDOFF}" > /sys/class/anbernic_misc/lcdoff
    [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
fi

exit 0
