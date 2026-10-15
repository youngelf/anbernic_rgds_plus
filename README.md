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

## [SDK](sdk/)

An SDK which can be used to develop further applications, supporting both `1024x768` dual screens via Weston/SDL2, all physical controls (`ANBERNIC-rk3568-keys`), and the 10-point capacitive touchscreen (`goodix-ts` Goodix GT911 on the lower screen, automatically enabled via `/sys/class/anbernic_misc/tpctrl`), along with a dual-screen Linux X11 desktop simulator and two heavily documented starter examples:

- **`hello_rgds` ([`sdk/examples/hello_rgds.c`](sdk/examples/hello_rgds.c))**: A dual-screen "Hello World" utility and live hardware button & touchscreen inspector.
- **`hello_touch` ([`sdk/examples/hello_touch.c`](sdk/examples/hello_touch.c))**: A multi-touch starter application demonstrating a simple clickable touch UI toolbar (color swatches, brush size, clear button), simultaneous 10-finger canvas drawing, a 2-finger pinch/spread distance gauge, and a live 10-slot coordinate inspector on the upper screen.

