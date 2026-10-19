#!/bin/sh
# Anbernic RG DS Plus - Custom Boot Logo Changer Launcher

PROGDIR=$(dirname "$(realpath "$0")")
if [ -d "$PROGDIR/boot_logo" ]; then
    APPDIR="$PROGDIR/boot_logo"
else
    APPDIR="$PROGDIR"
fi

cd "$APPDIR" || exit 1

export XDG_RUNTIME_DIR=/var/run
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=/usr/lib:/lib:$LD_LIBRARY_PATH

echo 0 > /sys/class/anbernic_misc/lcd0_off 2>/dev/null
echo 0 > /sys/class/anbernic_misc/lcd1_off 2>/dev/null

chmod +x ./boot_logo
./boot_logo > ./boot_logo.log 2>&1

echo 0 > /sys/class/anbernic_misc/lcd0_off 2>/dev/null
echo 0 > /sys/class/anbernic_misc/lcd1_off 2>/dev/null
exit 0
