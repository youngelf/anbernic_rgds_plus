# Anbernic RG DS Plus
Hand-written utilities for the Anbernic RG DS Plus

The Anbernic RG DS Plus is a dual screen Linux device. It can be used
to play retro games, but since it is a Linux machine, you can write
simple utilities that can be displayed on the screen, and controlled
using DPAD and A/B/X/Y keys.

Here are a few utilities that can be built both for the machine, and
also compile natively on Linux x11 so you can test them out before
copying it on the SD-card for running on your RG DS+.

Here's also a reverse engineered SDK that can be used to build your
own programs. This shows how to use the dual screens with SDL.

All these were tested in Sept 26, 2026 on the stock firmware since the
device was just released, and custom firmware is still not mature for
this device.


## [Metronome](metronome/)

A simple metronome that shows some common time signatures, and plays a
tick on the beat.

![Screenshot of metronome](/metronome/promo/metronome_grab.png)


## [Pomodoro](pomodoro/)

A work timer to let folks know that you are working.

![Screenshot of pomodoro](/pomodoro/promo/pomodoro_grab.png)

## [Night Clock](night_clock/)

A dual-screen bedside and desk clock featuring an app-local timezone shift (`±30m` increments), 24-hour Retro LED Dot-Matrix digital and minimalist analog faces (`X` button) with 10-minute IPS anti-burn-in orbit, a bottom date screen that automatically powers off after 5 seconds and wakes on any button press, D-Pad brightness control, a single alarm in the shifted timezone with loud confirmation audio (`A` button), and a toggleable `R1` Help screen.

![Screenshot of night_clock digital](/night_clock/promo/digital_clock.png)
![Screenshot of night_clock analog](/night_clock/promo/analog_clock.png)

## [Boot Logo Customizer](boot_logo/)

An on-device dual-screen boot logo customizer that updates both Stage 1 (U-Boot + Linux Kernel DRM FIT/Rockchip `RSCE` resource container in `/dev/mmcblk1p3` with automatic SHA-256 digest recalculation and read-back verification) and Stage 2 (`/mnt/vendor/resource/res{1,2}/boot/logo{,2}.png` cold-boot splash), with automatic backup and one-button factory restore. Includes four dual-screen themes sliced from `1024x1686` continuous compositions with a `150px` hinge gap buffer (`Cooking Pixels A`, `Cooking Pixels B`, `Tux Starry Sky`, and `Midnight Poetry`) plus Factory Stock restore.

![Screenshot of boot_logo Cooking Pixels](/boot_logo/promo/theme_chef_stew.png)
![Screenshot of boot_logo Tux Sky](/boot_logo/promo/theme_tux_sky.png)
![Screenshot of boot_logo Midnight Poetry](/boot_logo/promo/theme_poetry.png)

## [SDK](sdk/)

An SDK which can be used to develop further applications, supporting both `1024x768` dual screens via Weston/SDL2, all physical controls (`ANBERNIC-rk3568-keys`), and the 10-point capacitive touchscreen (`goodix-ts` Goodix GT911 on the lower screen, automatically enabled via `/sys/class/anbernic_misc/tpctrl`), along with a dual-screen Linux X11 desktop simulator and two heavily documented starter examples:

- **`hello_rgds` ([`sdk/examples/hello_rgds.c`](sdk/examples/hello_rgds.c))**: A dual-screen "Hello World" utility and live hardware button & touchscreen inspector.
![Screenshot of hello](/sdk/promo/hello.png)

- **`hello_touch` ([`sdk/examples/hello_touch.c`](sdk/examples/hello_touch.c))**: A multi-touch starter application demonstrating a simple clickable touch UI toolbar (color swatches, brush size, clear button), simultaneous 10-finger canvas drawing, a 2-finger pinch/spread distance gauge, and a live 10-slot coordinate inspector on the upper screen.

![Screenshot of hello_touch](/sdk/promo/hello_touch.png)
