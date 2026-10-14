#!/bin/bash
# Universal Launcher for Anbernic RG DS Plus (Stock Linux OS APPS/Ports & Custom Linux)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${SCRIPT_DIR}/metronome"

if [ -x "${SCRIPT_DIR}/metronome" ] && [ ! -d "${SCRIPT_DIR}/metronome" ]; then
    APP_DIR="${SCRIPT_DIR}"
else
    for candidate in \
        "${APP_DIR}" \
        "/mnt/mmc/Roms/APPS/metronome" \
        "/mnt/sdcard/Roms/APPS/metronome" \
        "/mnt/mmc/Ports/Metronome" \
        "/mnt/sdcard/Ports/Metronome" \
        "/mnt/mmc/Roms/PORTS/metronome" \
        "/mnt/sdcard/Roms/PORTS/metronome"; do
        if [ -d "${candidate}" ] && [ -x "${candidate}/metronome" ]; then
            APP_DIR="${candidate}"
            break
        fi
    done
fi

# On RG DS Plus stock OS, dmenu.bin starts subscreen.dge in the background on Display 1.
# Signal subscreen.dge to exit so our dual-screen metronome owns both 1024x768 displays,
# then restore runapp=1 so system audio/power helpers know an application is active.
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 0 > /sys/class/anbernic_misc/runapp
fi
killall -9 subscreen.dge 2>/dev/null
if [ -w /sys/class/anbernic_misc/runapp ]; then
    echo 1 > /sys/class/anbernic_misc/runapp
fi

# Ensure both 1024x768 LCDs are powered on during the dual-screen metronome
SAVED_LCDOFF=""
if [ -r /sys/class/anbernic_misc/lcdoff ]; then
    SAVED_LCDOFF="$(cat /sys/class/anbernic_misc/lcdoff 2>/dev/null)"
    if [ "${SAVED_LCDOFF}" != "0" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
        echo 0 > /sys/class/anbernic_misc/lcdoff
        [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
    fi
fi

# Match RG DS Plus Clock.sh / System_Settings.sh Weston/Wayland environment
export SDL_VIDEO_DOUBLE_BUFFER=1
export SDL_RENDER_VSYNC=1
if [ -r /sys/class/anbernic_misc/lcdswap ]; then
    LCDSWAP_VAL="$(cat /sys/class/anbernic_misc/lcdswap 2>/dev/null)"
    if [ "${LCDSWAP_VAL}" = "1" ]; then
        export SDL2_SWAP_LCD=1
    fi
fi

cd "${APP_DIR}" || exit 1
chmod +x ./metronome 2>/dev/null
./metronome

# Restore previous lcdoff state if user had single-screen mode enabled
if [ -n "${SAVED_LCDOFF}" ] && [ "${SAVED_LCDOFF}" != "0" ] && [ -w /sys/class/anbernic_misc/lcdoff ]; then
    echo "${SAVED_LCDOFF}" > /sys/class/anbernic_misc/lcdoff
    [ -x /mnt/vendor/bin/bright.dge ] && /mnt/vendor/bin/bright.dge
fi

exit 0
