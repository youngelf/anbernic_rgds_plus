# Anbernic RG DS Plus Native C SDK

A complete, self-contained C Software Development Kit (SDK), dual-screen Linux desktop simulator, and deployment toolkit for the **Anbernic RG DS Plus** handheld (`RG-DS-PLUS-EN16GB-V1.0`), derived directly from reverse-engineering the stock firmware image (`/mnt/vendor/bin/dmenu.bin`, `/mnt/vendor/subscreen/subscreen.dge`, `/mnt/vendor/bin/ebook/green`, `Roms/APPS/clock/`, `Roms/APPS/mod_set/`, and `/etc/xdg/weston/weston.ini`).

---

## 1. Directory Layout

```text
sdk/
├── include/
│   └── rgds_sdk.h             # Public C API header (video, dual screens, input, drawing, timing)
├── src/
│   └── rgds_sdk.c             # Hardware backend (Weston/SDL2 + evdev) & Host X11 simulator
├── examples/
│   ├── hello_rgds.c           # Sample "HELLO" (Top) / "WORLD" + Button & Touch Inspector (Bottom)
│   └── hello_touch.c          # Multi-touch starter app: touch UI toolbar, 10-finger pad & pinch gauge
├── scripts/
│   ├── setup_host.sh          # Installs Linux host dependencies (system apt or rootless cache)
│   ├── App_Template.sh        # Launcher template that stops subscreen.dge & wakes both LCDs
│   ├── deploy_to_sdcard.sh    # Copies sdcard_package/ to an inserted microSD card
│   ├── deploy_over_ssh.sh     # Pushes & optionally runs the binary over Wi-Fi SSH (root@anbernic.local)
│   └── deploy_to_img.sh       # Injects the app directly into an unmounted .IMG file (Partition 11)
├── assets/
│   ├── icon.png               # 240x180 RGBA Graduation Cap icon for Hello_RGDS
│   └── icon_touch.png         # 240x180 RGBA Chemical Flask icon for Hello_Touch
├── Makefile                   # Builds `hello_rgds` & `hello_touch` (aarch64 + x86_64 simulator)
└── README.md                  # Programmer's Reference Manual (this document)
```

---

## 2. What Needs to Be Installed on the Linux Host (`x86_64`)

### Required Packages
To cross-compile for the RG DS Plus (`aarch64-linux-gnu`) **and** compile the local X11 dual-screen desktop simulator (`x86_64`), install the following Debian/Ubuntu packages:

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    linux-libc-dev-arm64-cross \
    libx11-dev \
    mtools \
    rsync \
    openssh-client
```

Or run the included setup script:
```bash
./scripts/setup_host.sh          # Installs system-wide via sudo apt-get
./scripts/setup_host.sh --user   # Rootless install into ~/.cache/aarch64_toolchain
```

### Why Target `libSDL2-2.0.so.0` Headers Are Not Required on the Host
The SDK's hardware backend ([`src/rgds_sdk.c`](src/rgds_sdk.c)) dynamically loads `/usr/lib/libSDL2-2.0.so.0` at runtime using `dlopen()` / `dlsym()` and reads `/dev/input/event*` directly via standard Linux kernel headers (`<linux/input.h>`). As a result:
- Your `aarch64` cross-compiler only links against `-lm -lpthread -ldl -lc` (`glibc >= 2.17`).
- You never have to copy a full gigabyte Rockchip sysroot onto your development workstation just to link against Wayland/DRM/Mesa/SDL2.

---

## 3. Hardware & OS Architecture of the Anbernic RG DS Plus

### 3.1 SoC & Partition Table (`RG-DS-PLUS-EN16GB-V1.0-20260915.IMG`)
- **SoC**: Rockchip RK3568 (`aarch64`, Quad-Core ARM Cortex-A55 @ 1.8 GHz, Mali-G52 GPU).
- **Storage Layout** (GPT, 512-byte sectors):
  - **Partition 6 (`rootfs`)**: `ext4` mounted at `/` (5.0 GiB, start sector `491520`).
  - **Partition 8 (`vendor`)**: `ext4` mounted at `/mnt/vendor` (1.0 GiB, start sector `12025856`). Contains `/mnt/vendor/bin/dmenu.bin`, `/mnt/vendor/subscreen/subscreen.dge`, `/mnt/vendor/bin/bright.dge`, and `/mnt/vendor/ctrl/loadapp.sh`.
  - **Partition 9 (`userdata`)**: `ext4` mounted at `/userdata` (symlinked at `/mnt/data`).
  - **Partition 10 (`ports`)**: `ext4` mounted at `/mnt/ports` (contains `/mnt/ports/libs/SDL64`).
  - **Partition 11 (`ROMS`)**: `FAT32`/`exFAT` mounted at `/mnt/mmc` (TF1 internal SD) or `/mnt/sdcard` (TF2 external SD), start sector `22511616` (byte offset `11525947392`).

### 3.2 Dual `1024×768` Screen Setup (Weston + Wayland + SDL2)
Unlike older single-screen handhelds that expose a raw `/dev/fb0` without a compositor, the RG DS Plus runs **Weston** (`/etc/init.d/S49weston`) over the Rockchip DRM backend (`backend=drm-backend.so` in `/etc/xdg/weston/weston.ini`) with two MIPI-DSI outputs:
- **`DSI-1` (Weston Display 0)**: `1024×768` @ 60 Hz (`4:3` IPS) — Physical **Upper Screen** (unless `lcdswap=1`).
- **`DSI-2` (Weston Display 1)**: `1024×768` @ 60 Hz (`4:3` IPS) — Physical **Lower Screen** (unless `lcdswap=1`).
- **Compositor Socket**: `/var/run/wayland-0` (or `/var/run/0-runtime-dir/wayland-0`), configured in `/etc/profile.d/myenv.sh` with `WESTON_OUTPUT_FLOW=horizontal`.

#### How Dual-Screen Window Placement Works in SDL2
When `SDL_Init(SDL_INIT_VIDEO)` connects to Weston on the RG DS Plus:
1. `SDL_GetNumVideoDisplays()` returns **`2`**.
2. `SDL_GetDisplayBounds(0, &rect0)` returns `{ x: 0, y: 0, w: 1024, h: 768 }`.
3. `SDL_GetDisplayBounds(1, &rect1)` returns `{ x: 1024, y: 0, w: 1024, h: 768 }`.
4. To place a fullscreen window on **Display $i \in \{0, 1\}$**, pass `SDL_WINDOWPOS_CENTERED_DISPLAY(i)` (`0x2FFF0000 | i`) as both the `x` and `y` coordinates to `SDL_CreateWindow()` with flags `SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN` (`0x1005`):
   ```c
   SDL_Window *win_top = SDL_CreateWindow(
       "App - Top",
       SDL_WINDOWPOS_CENTERED_DISPLAY(0),
       SDL_WINDOWPOS_CENTERED_DISPLAY(0),
       1024, 768,
       SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN
   );
   SDL_Window *win_bottom = SDL_CreateWindow(
       "App - Bottom",
       SDL_WINDOWPOS_CENTERED_DISPLAY(1),
       SDL_WINDOWPOS_CENTERED_DISPLAY(1),
       1024, 768,
       SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN
   );
   ```

#### Critical RG DS Plus Quirks Handled Automatically by `rgds_init()` & `App_Template.sh`
1. **Stopping `subscreen.dge` on Display 1**:
   When the stock frontend (`/mnt/vendor/bin/dmenu.bin`) launches any script from `Roms/APPS`, it writes two lines into `/tmp/.next`:
   ```sh
   /mnt/vendor/subscreen/launch.sh apk apps 0 &
   /mnt/mmc/Roms/APPS/YourApp.sh
   ```
   `launch.sh` sets `/sys/class/anbernic_misc/runapp` to `1` and spawns `subscreen.dge` on Display 1! If your app does not stop it, `subscreen.dge` will fight your Lower Screen window for Display 1.
   Both [`scripts/App_Template.sh`](scripts/App_Template.sh) and [`rgds_init()`](src/rgds_sdk.c) execute:
   ```sh
   echo 0 > /sys/class/anbernic_misc/runapp
   killall -9 subscreen.dge
   echo 1 > /sys/class/anbernic_misc/runapp
   ```
   Because `/mnt/vendor/subscreen/launch.sh` is a single-shot script (not a loop), killing `subscreen.dge` releases Display 1 permanently for your application, and restoring `runapp` to `1` informs system daemons that an external application is running.
2. **Waking Both Panels (`lcdoff`)**:
   If the user pressed the screen-off hotkey before launching your app, `/sys/class/anbernic_misc/lcdoff` is `1` or `2` (one LCD powered down). `rgds_init()` saves the current `lcdoff` value, writes `0` to `/sys/class/anbernic_misc/lcdoff`, invokes `/mnt/vendor/bin/bright.dge` to turn on both backlights, and restores the user's previous setting in `rgds_shutdown()`.
3. **Respecting Screen Swap (`lcdswap`)**:
   If `/sys/class/anbernic_misc/lcdswap` is `1` (or `SDL2_SWAP_LCD=1` is set in the environment), `rgds_init()` automatically swaps the display indices so `RGDS_SCREEN_TOP` always appears on the logical primary screen.
4. **Hardware Volume Up/Down (`KEY_VOLUMEUP` / `KEY_VOLUMEDOWN`) & PulseAudio Routing**:
   When `dmenu.bin` launches an external script via `/mnt/vendor/ctrl/dmenu_ln`, `dmenu.bin` writes `/tmp/.next` and **exits**, so `dmenu.bin` is no longer running to poll the physical Volume buttons (`KEY_VOLUMEUP = 115`, `KEY_VOLUMEDOWN = 114`).
   [`rgds_poll_input()`](src/rgds_sdk.c) automatically handles `KEY_VOLUMEUP` (`115`) and `KEY_VOLUMEDOWN` (`114`) (including continuous hold auto-repeat) and updates:
   - `/sys/class/anbernic_misc/openbor_volume` (`0..10`),
   - PulseAudio speaker (`alsa_output.1.stereo-fallback`) and headphone (`alsa_output.0.HiFi__Speaker__sink`) sink volumes via `/mnt/vendor/ctrl/setPulseaudio.sh volume spk|hp <pct>` using the stock firmware lookup table `{ 0, 22, 36, 44, 50, 56, 62, 70, 80, 90, 100 }`, and
   - Screen brightness (`/sys/class/anbernic_misc/brightness` + `/mnt/vendor/bin/bright.dge`) when `SELECT` or `START` is held while pressing Volume Up/Down.
   You can also query or set the volume programmatically via `rgds_get_volume()`, `rgds_set_volume(level_0_to_10)`, and `rgds_volume_to_percent(level_0_to_10)`.
5. **Enabling the Capacitive Touchscreen (`tpctrl = 0` & Weston Calibration)**:
   When `dmenu.bin` exits to launch any external application (`set_touch_inpt(0)` at `0x15194`) and when `/mnt/vendor/subscreen/launch.sh` runs in the background, the stock firmware writes `1` to `/sys/class/anbernic_misc/tpctrl`. Inside the kernel (`gt9xx` driver at `0xc4931c` calling `allen_get_tpctrl()` at `0xc27d84`), **`tpctrl != 0` causes the kernel driver to silently drop all touch interrupts**.
   [`rgds_init()`](src/rgds_sdk.c) and [`scripts/App_Template.sh`](scripts/App_Template.sh) automatically write `0` to `/sys/class/anbernic_misc/tpctrl` and configure `/tmp/.weston_drm.conf` (`calibration:gt9xx-0:0.5 0 0.5 0 1 0`) so touch input is immediately active for your application.

---

## 4. Complete Button & Capacitive Touchscreen Reference

### 4.1 Physical Buttons (`ANBERNIC-rk3568-keys`)

On the RG DS Plus, physical buttons are exposed via the Linux kernel `evdev` subsystem (`/dev/input/event*`) under the device name **`ANBERNIC-rk3568-keys`**. The table below lists the exact Linux event type, decimal/hex code, SDK bitmask constant, and X11 desktop simulator key mapping:

| Physical Control | Linux Event Type | `ev.code` (Dec / Hex) | Kernel Macro | SDK Bitmask (`rgds_button_t`) | Linux X11 Simulator Key |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **D-Pad Left / Right** | `EV_ABS` (`3`) | `16` (`0x10`) | `ABS_HAT0X` (`-1` / `+1`) | `RGDS_BTN_LEFT` / `RGDS_BTN_RIGHT` | `Left Arrow` / `Right Arrow` |
| **D-Pad Up / Down** | `EV_ABS` (`3`) | `17` (`0x11`) | `ABS_HAT0Y` (`-1` / `+1`) | `RGDS_BTN_UP` / `RGDS_BTN_DOWN` | `Up Arrow` / `Down Arrow` |
| **A Button** (East) | `EV_KEY` (`1`) | `304` (`0x130`) | `BTN_SOUTH` | `RGDS_BTN_A` | `A` or `Space` |
| **B Button** (South) | `EV_KEY` (`1`) | `305` (`0x131`) | `BTN_EAST` | `RGDS_BTN_B` | `B` or `Backspace` |
| **Y Button** (West) | `EV_KEY` (`1`) | `306` (`0x132`) | `BTN_C` | `RGDS_BTN_Y` | `Y` |
| **X Button** (North) | `EV_KEY` (`1`) | `307` (`0x133`) | `BTN_NORTH` | `RGDS_BTN_X` | `X` |
| **L1 Shoulder** | `EV_KEY` (`1`) | `308` (`0x134`) | `BTN_WEST` | `RGDS_BTN_L1` | `Q` or `[` |
| **R1 Shoulder** | `EV_KEY` (`1`) | `309` (`0x135`) | `BTN_Z` | `RGDS_BTN_R1` | `E` or `]` |
| **SELECT** | `EV_KEY` (`1`) | `310` (`0x136`) | `BTN_TL` | `RGDS_BTN_SELECT` | `Tab` or `Right Shift` |
| **START** | `EV_KEY` (`1`) | `311` (`0x137`) | `BTN_TR` | `RGDS_BTN_START` | `Enter` (`Return`) |
| **MENU (`M` Button)** | `EV_KEY` (`1`) | `312` (`0x138`) | `BTN_TR2` | `RGDS_BTN_MENU` | `M` or `ESC` (Exits app) |
| **L2 Trigger** | `EV_KEY` (`1`) | `314` (`0x13a`) | `BTN_SELECT` | `RGDS_BTN_L2` | `1` or `Page Down` |
| **R2 Trigger** | `EV_KEY` (`1`) | `315` (`0x13b`) | `BTN_START` | `RGDS_BTN_R2` | `2` or `Page Up` |
| **L3 Stick Click** | `EV_KEY` (`1`) | `317` (`0x13d`) | `BTN_THUMBL` | `RGDS_BTN_L3` | *(N/A)* |
| **R3 Stick Click** | `EV_KEY` (`1`) | `318` (`0x13e`) | `BTN_THUMBR` | `RGDS_BTN_R3` | *(N/A)* |
| **Volume Down (`VOL-`)** | `EV_KEY` (`1`) | `114` (`0x72`) | `KEY_VOLUMEDOWN` | `RGDS_BTN_VOL_DOWN` | `-` or `_` |
| **Volume Up (`VOL+`)** | `EV_KEY` (`1`) | `115` (`0x73`) | `KEY_VOLUMEUP` | `RGDS_BTN_VOL_UP` | `+` or `=` |

> **Note on D-Pad Events**: On the RG DS Plus (`ANBERNIC-rk3568-keys`), the D-Pad does **not** emit `KEY_UP`/`KEY_DOWN`/`KEY_LEFT`/`KEY_RIGHT` (`EV_KEY`) by default; instead, it emits `EV_ABS` events on `ABS_HAT0X` (`code 16`, value `-1` for Left, `0` for Release, `+1` for Right) and `ABS_HAT0Y` (`code 17`, value `-1` for Up, `0` for Release, `+1` for Down). [`rgds_poll_input()`](src/rgds_sdk.c) handles **both** `ABS_HAT0X`/`ABS_HAT0Y` and `KEY_UP`/`DOWN`/`LEFT`/`RIGHT` transparently.

### 4.2 Capacitive Touchscreen (`goodix-ts`, Goodix GT911 on I2C5 `0x14`)

Verified from the kernel device tree (`/i2c@fe5e0000/gt9xx-0@14`), kernel driver (`gt9xx` @ `0xc49100`), and the built-in touchscreen tester in `/mnt/vendor/bin/dmenu.bin` (`Touch Panel Test`, `0x13434`..`0x136e4` and `0x48b58`..`0x49d50`):
- **Hardware Controller**: Goodix GT911 (`compatible = "goodix,gt9xx"`, `tp-size = <911>`, `max-x = <1024>`, `max-y = <768>`), mounted on the **Lower Screen** (`RGDS_SCREEN_BOTTOM`, or `RGDS_SCREEN_TOP` when `lcdswap=1`).
- **Kernel `evdev` Device Name**: `"goodix-ts"` (`phys = "input/ts"`, `bustype = BUS_I2C`).
- **Multi-Touch Protocol**: Linux Multi-Touch Protocol Type B (`RGDS_MAX_TOUCH_POINTS = 10` simultaneous slots `0..9`):
  - `ABS_MT_SLOT` (`0x2f` / `47`): Finger slot index (`0..9`)
  - `ABS_MT_TRACKING_ID` (`0x39` / `57`): Slot ID (`0..9`) on touch/motion, `-1` on lift/release
  - `ABS_MT_POSITION_X` (`0x35` / `53`): Native X coordinate (`0..1024`)
  - `ABS_MT_POSITION_Y` (`0x36` / `54`): Native Y coordinate (`0..768`)
  - `ABS_MT_TOUCH_MAJOR` (`0x30` / `48`) & `ABS_MT_WIDTH_MAJOR` (`0x32` / `50`): Contact size (`0..255`)
- **SDL2 Fallback**: If raw `/dev/input/event*` access is unavailable, [`rgds_poll_input()`](src/rgds_sdk.c) also parses `SDL_FINGERDOWN` (`0x700`), `SDL_FINGERUP` (`0x701`), and `SDL_FINGERMOTION` (`0x702`) events identically to `dmenu.bin`'s built-in Touch Panel Test.
- **SDK Touch State (`input.touch`)**:
  - Single-touch convenience fields: `input.touch.touching`, `input.touch.just_pressed`, `input.touch.just_released`, `input.touch.count`, `input.touch.x` (`0..319`), `input.touch.y` (`0..239`), `input.touch.dx`, `input.touch.dy`, `input.touch.native_x` (`0..1023`), `input.touch.native_y` (`0..767`), `input.touch.pressure`, `input.touch.screen`.
  - Full 10-point multi-touch array: `input.touch.points[0 .. RGDS_MAX_TOUCH_POINTS - 1]`.
  - In the Linux X11 desktop simulator (`-DHOST_X11`), left-clicking and dragging the mouse on either simulated screen emulates touch input on that screen.

---

## 5. Building & Running the Sample Applications (`hello_rgds` & `hello_touch`)

The SDK includes two heavily commented starter applications in [`examples/`](examples/):

### 5.1 `hello_rgds` ([`examples/hello_rgds.c`](examples/hello_rgds.c)) — Dual-Screen "Hello World" & Hardware Tester
- **Upper Screen (`RGDS_SCREEN_TOP`, `1024×768`)**: A large `"HELLO"` banner that you can move around with the **D-Pad** (press **X** to re-center), along with live uptime, **A**/**B** press counters, and real-time touchscreen coordinate telemetry.
- **Lower Screen (`RGDS_SCREEN_BOTTOM`, `1024×768`)**: A large `"WORLD"` banner above a real-time **Interactive Button & 10-Point Touchscreen Tester** that highlights every RG DS Plus button as you press it and draws live multi-touch crosshairs (`T0..T9`) and a persistent touch drawing trail (press **Y** to clear trail).

![Screenshot of hello](/sdk/promo/hello.png)

### 5.2 `hello_touch` ([`examples/hello_touch.c`](examples/hello_touch.c)) — Multi-Touch UI & Canvas Starter App
Designed as a documented starting point for writing touch-driven utilities and games:
- **Single-Touch UI Toolbar (`input.touch.just_pressed`)**: On-screen clickable color swatch buttons (`CYN`, `YEL`, `GRN`, `ORG`, `RED`, `WHT`), brush size toggle (`SZ:1` / `SZ:2` / `SZ:3`), and a `CLEAR` button on the Lower Touchscreen.
- **10-Point Simultaneous Multi-Touch Canvas (`input.touch.points[0..9]`)**: Uses Bresenham stroke interpolation (`pt->dx`, `pt->dy`) so up to 10 fingers can draw smooth, continuous strokes simultaneously on the Lower Screen, with pressure-reactive finger reticles (`F0..F9`).
- **2-Finger Pinch/Spread Chord Visualizer**: When 2 or more fingers touch the screen at the same time, draws a live connecting chord and midpoint marker between the first two active fingers on the Lower Screen and renders a real-time pixel distance gauge on the Upper Screen alongside a 10-slot coordinate inspector table.

![Screenshot of hello_touch](/sdk/promo/hello_touch.png)

### Build Both Targets (`aarch64` Device + `x86_64` Host Simulator)
```bash
cd sdk
make all linux
```
This produces:
1. `hello_rgds` and `hello_touch` (`ARM aarch64` stripped binaries) + `sdcard_package/` ready for the RG DS Plus.
2. `hello_rgds_linux` and `hello_touch_linux` (`x86_64` X11 binaries) for local testing on your Linux PC.

### Run the Dual-Screen Simulator on Your Linux Desktop
```bash
make run-linux         # Launches ./hello_rgds_linux
make run-touch-linux   # Launches ./hello_touch_linux
```
By default, the X11 simulator scales each `1024×768` screen to `640×480` (`640×968` total height including the 8px clamshell hinge) so both stacked screens fit comfortably on a 1080p/1200p desktop monitor. To open at unscaled `1024×1544` (`1024×768` × 2):
```bash
RGDS_FULL_1024=1 ./hello_rgds_linux
RGDS_FULL_1024=1 ./hello_touch_linux
```

### Build Your Own C Program Using the SDK
To compile and package any custom C file `my_game.c` into its own `sdcard_package/`:
```bash
make APP_NAME=My_Game APP_BIN=my_game APP_SRCS=my_game.c all linux
```

---

## 6. Deploying to an Anbernic RG DS Plus Machine

The `scripts/` directory provides three ways to copy your compiled programs to the device:

### Method A: Copy to a Mounted microSD Card (`scripts/deploy_to_sdcard.sh`)
Insert your RG DS Plus TF1 or TF2 microSD card into your Linux PC and run:
```bash
./scripts/deploy_to_sdcard.sh /media/$USER/ROMS
# Or let the script auto-detect the mounted SD card:
make deploy-sd
```
This installs both `Hello_RGDS` and `Hello_Touch` into:
- **App Center (`Roms/APPS`)**:
  - `Roms/APPS/Hello_RGDS.sh` & `Roms/APPS/hello_rgds/hello_rgds`
  - `Roms/APPS/Hello_Touch.sh` & `Roms/APPS/hello_touch/hello_touch`
  - `Roms/APPS/Imgs/Hello_RGDS.png` & `Roms/APPS/Imgs/Hello_Touch.png`
- **Ports (`Ports`)**:
  - `Ports/Hello_RGDS/launch.sh`, `hello_rgds`, `icon.png`
  - `Ports/Hello_Touch/launch.sh`, `hello_touch`, `icon.png`

### Method B: Push Over Wi-Fi via SSH (`scripts/deploy_over_ssh.sh`)
The RG DS Plus stock firmware runs OpenSSH (`S50sshd`) and Avahi mDNS (`S50avahi-daemon`, hostname `anbernic.local`) out of the box with `PermitRootLogin yes`.
- **Username**: `root`
- **Default Password**: `root` (verified from `/etc/shadow` in `RG-DS-PLUS-EN16GB-V1.0-20260915.IMG`)

To copy the package over Wi-Fi and optionally launch it immediately on the device's dual screens:
```bash
# Deploy to anbernic.local (or pass the device IP address):
./scripts/deploy_over_ssh.sh 192.168.1.88

# Deploy AND immediately run on the RG DS Plus dual screens:
./scripts/deploy_over_ssh.sh 192.168.1.88 --run
```

### Method C: Inject Directly Into an `.IMG` Firmware File (`scripts/deploy_to_img.sh`)
If you are preparing a disk image before flashing it to an SD card, you can inject the compiled package directly into Partition 11 (`offset = 22511616 * 512`) without root or loop mounts:
```bash
./scripts/deploy_to_img.sh <path_to>/RG-DS-PLUS-EN16GB-V1.0-20260915.IMG
```
