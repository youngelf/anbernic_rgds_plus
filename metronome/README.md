# Anbernic RG DS Plus Dual-Screen Metronome (`1024x768` × 2)

A dual-screen precision C metronome built specifically for the **Anbernic RG DS Plus** (Rockchip RK3568 `aarch64`, dual `1024x768` 4:3 IPS displays) running Anbernic's stock Linux OS (`RG-DS-PLUS-EN16GB-V1.0`), plus a native **x86_64 Linux (X11)** target that renders both screens in a stacked clamshell window for desktop testing.

## What Was Adapted from the RG DS Plus Firmware Image
By inspecting `RG-DS-PLUS-EN16GB-V1.0-20260915.IMG` (`Roms/APPS/Clock.sh`, `Roms/APPS/clock/`, `Roms/APPS/System_Settings.sh`, `Roms/APPS/mod_set/`, `/mnt/vendor/subscreen/subscreen.dge`, `/mnt/vendor/bin/dmenu.bin`, and `/mnt/vendor/bin/ebook/green`), the code and packaging were configured to match the exact RG DS Plus hardware and OS:
1. **Native Dual `1024x768` Resolution**:
   - Both the Upper and Lower screens render at the native **`1024x768`** resolution (`NATIVE_W = 1024`, `NATIVE_H = 768`, exact 4:3 aspect ratio).
2. **Weston/Wayland Dual-Display SDL2 Windowing**:
   - The RG DS Plus runs a Weston/Wayland compositor (`/var/run/0-runtime-dir/wayland-0`) with two `1024x768` outputs (`Display 0` and `Display 1`).
   - Like `mod_set/graphic.py` and `subscreen.dge`, our binary opens two `1024x768` SDL2 windows positioned on Display 0 (`SDL_WINDOWPOS_CENTERED_DISPLAY(0)`) and Display 1 (`SDL_WINDOWPOS_CENTERED_DISPLAY(1)`) with `SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN` (and fallback flags `0x3001`), while respecting `/sys/class/anbernic_misc/lcdswap` (`SDL2_SWAP_LCD`).
3. **Background Subscreen Daemon (`subscreen.dge`) & Dual-LCD Power (`lcdoff`)**:
   - When launching an app from `App Center` (`Roms/APPS`), `dmenu.bin` automatically spawns `/mnt/vendor/subscreen/launch.sh apk apps 0 &` (`subscreen.dge`) on the second screen. Both `Metronome.sh` and `metronome.c` write `0` to `/sys/class/anbernic_misc/runapp` and terminate `subscreen.dge` so our Lower Screen statistics window has exclusive control of the second `1024x768` screen, and ensure `/sys/class/anbernic_misc/lcdoff` is set to `0` (both screens powered on).
4. **Native `ANBERNIC-rk3568-keys` Input Mapping**:
   - Matches `Roms/APPS/clock/input.py` and `mod_set/input.py` on `/dev/input/event*` (`ANBERNIC-rk3568-keys`): `304` = **A**, `305` = **B**, `306` = **Y**, `307` = **X**, `308` = **L1**, `309` = **R1**, `310` = **SELECT**, `311` = **START**, `312` = **MENU (`M`)**, `314` = **L2**, `315` = **R2**, and `ABS_HAT0X` (`16`) / `ABS_HAT0Y` (`17`) for the D-Pad.

---

## Dual-Screen Layout (`1024x768` Upper + `1024x768` Lower)
- **Upper Screen (`1024x768` — Active Metronome)**:
  - Status banner (`METRONOME PLAYING` / `METRONOME PAUSED`)
  - Large BPM readout (`30`–`260` BPM) with classical tempo marking (*Grave*, *Largo*, *Adagio*, *Andante*, *Moderato*, *Allegro*, *Vivace*, *Presto*, *Prestissimo*)
  - Time Signature selector (`4/4`, `3/4`, `2/4`, `5/4`, `7/4`)
  - Dynamic Beat Visualizer boxes with red `ACC` highlight on Beat 1 and green highlights on subsequent beats, plus a swinging pendulum bar
- **Lower Screen (`1024x768` — Practice Statistics)**:
  - **Current Session Card**: Active practice duration, session beats played, and session measures (bars) completed
  - **All-Time Totals Card**: Lifetime practice time, lifetime beats, lifetime bars, and total practice sessions (automatically saved to `/mnt/sdcard/.metronome_rgds.cfg`, `/mnt/mmc/.metronome_rgds.cfg`, or `~/.metronome_rgds.cfg`)
  - **BPM Stats Strip**: Beat-weighted Average BPM, Minimum BPM practiced, and Maximum BPM practiced
  - **Time Signature Breakdown**: Horizontal bar chart and per-signature practice time & bar counts across `4/4`, `3/4`, `2/4`, `5/4`, and `7/4`

---

## Controls

| Action | Anbernic RG DS Plus Button | Linux Desktop Key |
| :--- | :--- | :--- |
| **Play / Pause** | **A** (or **B** while playing) | `A`, `Space`, `Enter`, or `Left Alt` |
| **Increase / Decrease Tempo (±1 BPM, hold for continuous)** | **D-Pad UP** / **DOWN** | `Up Arrow` / `Down Arrow` |
| **Cycle Time Signature (`4/4`, `3/4`, `2/4`, `5/4`, `7/4`)** | **D-Pad LEFT** / **RIGHT** | `Left Arrow` / `Right Arrow` |
| **Fast Tempo Step (±5 BPM)** | **R1 / R2** (+5) and **L1 / L2** (-5) | `Page Up` / `Page Down` or `]` / `[` |
| **Volume Up / Down (0–100%, hold for continuous)** | **VOL+** (`115`) / **VOL-** (`114`) | `+` / `=` (Vol Up) and `-` / `_` (Vol Down) |
| **Screen Brightness Up / Down** | **START + VOL+** / **START + VOL-** | *(Hardware only)* |
| **Reset Current Session Stats (while paused)** | **B** | `B` or `C` |
| **Save & Quit** | **MENU (`M`)**, **SELECT**, or **START + SELECT** | `Q` or `ESC` |

---

## Installing on Your Anbernic RG DS Plus SD Card

Build the package with:
```bash
cd metronome
make
```

This creates `sdcard_package/` formatted for the RG DS Plus stock firmware (`dmenu.bin`):
- **Option 1 — App Center (`Roms/APPS`)**:
  Copy `sdcard_package/Roms/APPS/` to the `ROMS` partition of your SD card (`/mnt/mmc/Roms/APPS/` or `/mnt/sdcard/Roms/APPS/`):
  - `Roms/APPS/Metronome.sh`
  - `Roms/APPS/Imgs/Metronome.png`
  - `Roms/APPS/metronome/metronome`
- **Option 2 — Ports Menu (`Ports`)**:
  Copy `sdcard_package/Ports/Metronome/` to `/mnt/mmc/Ports/Metronome/` (or `/mnt/sdcard/Ports/Metronome/`).

---

## Building & Running on Linux Desktop (`x86_64`)

```bash
cd metronome
make linux       # builds ./metronome_linux
make run-linux   # opens the dual-screen X11 window (Upper + Lower screens)
```
By default on Linux desktop, the X11 window scales the two `1024x768` screens by `75%` (`768x576` each = `768x1160` total height) so both screens fit comfortably on a standard laptop/desktop monitor. Set `RGDS_FULL_1024=1 ./metronome_linux` to open at full unscaled `1024x1544` (`1024x768` × 2).
