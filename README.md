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

An SDK which can be used to develop further applications, along with a
simple hello world utility and a key tester.

