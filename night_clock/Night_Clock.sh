#!/bin/bash
# Universal Launcher for Anbernic RG DS Plus Night Clock (Stock Linux OS APPS & Ports)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${SCRIPT_DIR}/night_clock"

if [ -x "${SCRIPT_DIR}/night_clock" ] && [ ! -d "${SCRIPT_DIR}/night_clock" ]; then
    APP_DIR="${SCRIPT_DIR}"
else
    for candidate in \
        "${APP_DIR}" \
        "/mnt/mmc/Roms/APPS/night_clock" \
        "/mnt/sdcard/Roms/APPS/night_clock" \
        "/mnt/mmc/Ports/Night_Clock" \
        "/mnt/sdcard/Ports/Night_Clock" \
        "/mnt/mmc/Roms/PORTS/night_clock" \
        "/mnt/sdcard/Roms/PORTS/night_clock"; do
        if [ -d "${candidate}" ] && [ -x "${candidate}/night_clock" ]; then
            APP_DIR="${candidate}"
            break
        fi
    done
fi

# Stop subscreen.dge on Display 1 so Night Clock owns both 1024x768 displays
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 0 > /sys/class/anbernic_misc/runapp
fi
killall -9 subscreen.dge 2>/dev/null
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 1 > /sys/class/anbernic_misc/runapp
fi

# Save initial lcdoff & brightness so we can restore them cleanly on exit
SAVED_LCDOFF=""
if [ -r /sys/class/anbernic_misc/lcdoff ]; then
    SAVED_LCDOFF="$(cat /sys/class/anbernic_misc/lcdoff 2>/dev/null)"
    if [ "${SAVED_LCDOFF}" != "0" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
        echo 0 > /sys/class/anbernic_misc/lcdoff
        [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
    fi
fi

export SDL_VIDEO_DOUBLE_BUFFER=1
export SDL_RENDER_VSYNC=1
if [ -r /sys/class/anbernic_misc/lcdswap ]; then
    LCDSWAP_VAL="$(cat /sys/class/anbernic_misc/lcdswap 2>/dev/null)"
    if [ "${LCDSWAP_VAL}" = "1" ]; then
        export SDL2_SWAP_LCD=1
    fi
fi

cd "${APP_DIR}" || exit 1
chmod +x ./night_clock 2>/dev/null
./night_clock

# Restore previous lcdoff state on exit
if [ -n "${SAVED_LCDOFF}" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
    echo "${SAVED_LCDOFF}" > /sys/class/anbernic_misc/lcdoff
    [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
fi

exit 0
