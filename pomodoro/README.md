# Dual-Screen Pomodoro Work Timer for Anbernic RG DS Plus (`pomodoro_rg_ds`)

A dual-screen Pomodoro timer and work-session history tracker built for the **Anbernic RG DS Plus** (`RG-DS-PLUS-EN16GB-V1.0`, Rockchip RK3568 `aarch64`, dual `1024×768` 4:3 IPS displays) using the **RG DS Plus Native C SDK** (`sdk` / [`../rg_ds_plus_sdk`](../rg_ds_plus_sdk)), plus a native **x86_64 Linux (X11)** target that renders both `1024×768` screens stacked vertically in a single desktop simulator window.

---

## 1. Dual-Screen Layout (`1024×768` Top + `1024×768` Bottom)

- **Top Screen (`RGDS_SCREEN_TOP` — Active Pomodoro Timer)**:
  - **Status Header Bar**: Displays `"WORKING: DO NOT DISTURB"` centered in red when in Work mode, `"SHORT BREAK: RELAX"`, `"LONG BREAK: RELAX"`, `"PAUSED: TIMER STOPPED"`, or `"WORK DONE! TAKE A BREAK"`.
  - **Live Status Banner**: Blinking status indicator and prompt (`STATUS: WORKING (PRESS A TO PAUSE)`, `STATUS: READY TO WORK (PRESS A)`).
  - **Main Countdown Box**: Large `MM:SS` countdown display and visual progress bar.
  - **Work Hours & Stats Panel**: Lifetime hours worked (`WORKED: X.XX HRS`), exact `Xh YYm ZZs (N POMS)`, today's hours and Pomodoros, current session work time, and active mode duration.
  - **Controls Footer**: Quick reminder for `[A] START/STOP`, `[X] MODE`, `[Y]RST`, and `[SEL]EXIT`.

- **Bottom Screen (`RGDS_SCREEN_BOTTOM` — Previous Sessions & Work History)**:
  - **Summary Cards**:
    1. **History Count**: Total recorded sessions (`N SESSIONS`) and completed full Pomodoros (`N COMPLETED`).
    2. **Work Sessions**: Average work session duration (`AVG: XXm YYs`) and longest work session (`MAX: XXm YYs`).
    3. **Last Session**: Most recent previous session's mode, duration, and date/time (`MM-DD HH:MM`).
  - **Scrollable Previous Sessions Table**:
    - Lists your previous sessions (newest at the top, plus a live `IN PROGRESS` row when a timer is running or paused) with columns for:
      - `#` (Sequential session ID)
      - `DATE/TIME` (Timestamp when the session was completed or logged)
      - `MODE` (`WORK`, `S-BRK`, `L-BRK`)
      - `DURATION` (`XXm YYs` or `Xh YYm`)
      - `PCT` (Visual mini progress bar showing completion percentage of the target duration)
      - `STATUS` (`DONE` in green for completed Pomodoros, `PART` in orange for partial logged sessions, or `LIVE`/`PAUS` for the current active session)
  - **Completion Rate & Last 10 Sessions Bar Chart**:
    - Displays overall session completion percentage, today's Pomodoro count, and a 10-bar visual duration chart of your most recent sessions.

---

## 2. What Is Needed on a Host Computer to Build This

### Required Packages (Ubuntu / Debian / gLinux `x86_64`)

To cross-compile the `aarch64` binary (`pomodoro`) for the RG DS Plus **and** build the local `x86_64` X11 dual-screen desktop simulator (`pomodoro_linux`), install:

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

Or run the SDK's automated host setup script directly from this directory:

```bash
make setup-host
# Or for a rootless toolchain install into ~/.cache/aarch64_toolchain:
sdk/scripts/setup_host.sh --user
```

> **Note**: You do **not** need target SDL2 or Wayland headers installed on the host. The RG DS Plus SDK dynamically loads `/usr/lib/libSDL2-2.0.so.0` on the device at runtime via `dlopen()` / `dlsym()` and reads `/dev/input/event*` (`ANBERNIC-rk3568-keys`) using standard Linux kernel headers (`<linux/input.h>`).

---

## 3. Building & Running on the Host Computer

```bash
cd pomodoro

# 1. Build the RG DS Plus aarch64 binary (pomodoro) + sdcard_package/
make

# 2. Build the x86_64 Linux dual-screen X11 desktop simulator (pomodoro_linux)
make linux

# 3. Build and launch the dual-screen X11 simulator on your Linux desktop
make run-linux
```

By default, `pomodoro_linux` scales each `1024×768` screen to `640×480` (`640×968` total window height including the 8px clamshell hinge) so both stacked screens fit comfortably on a 1080p/1200p monitor. To run at full unscaled `1024×1544` (`1024×768` × 2):

```bash
RGDS_FULL_1024=1 ./pomodoro_linux
```

---

## 4. Controls (Anbernic RG DS Plus & Linux Desktop Simulator)

| Action | Anbernic RG DS Plus Button | Linux Desktop Simulator Key |
| :--- | :--- | :--- |
| **Toggle Start / Stop (Pause)** | **A Button** | `A`, `Space`, or `Left Alt` |
| **Stop / Pause Timer** | **B Button** | `B`, `Backspace`, or `Left Ctrl` |
| **Switch Mode** (`Work` / `Short Break` / `Long Break`) | **X Button** or **D-Pad LEFT / RIGHT** | `X`, `Left Shift`, or `Left` / `Right Arrow` |
| **Reset Current Timer** *(logs partial work if > 0s)* | **Y Button** | `Y` or `R` |
| **Adjust Duration** ($\pm 1$ min while stopped) | **D-Pad UP / DOWN** or **R1 / L1** | `Up` / `Down Arrow`, `E` / `Q`, or `]` / `[` |
| **Scroll Previous Sessions Table** (Bottom Screen) | **L2 / R2 Triggers** | `1` / `2` or `Page Down` / `Page Up` |
| **Log Current Partial Session to History** | **START** | `Enter` (`Return`) |
| **Save & Exit** | **SELECT**, **MENU (`M`)**, or **START + SELECT** | `Esc`, `M`, or `Tab` |

---

## 5. How to Install Onto an Anbernic RG DS Plus

Running `make` generates `sdcard_package/` containing both the **App Center** (`Roms/APPS`) and **Ports** (`Ports`) directory structures used by the RG DS Plus stock frontend (`/mnt/vendor/bin/dmenu.bin`):

```text
sdcard_package/
├── Roms/
│   └── APPS/
│       ├── Pomodoro.sh              # Launcher script (stops subscreen.dge & wakes both LCDs)
│       ├── Imgs/
│       │   └── Pomodoro.png         # App Center icon
│       └── pomodoro/
│           ├── pomodoro             # ARM64 (aarch64) executable
│           └── pomodoro.png
└── Ports/
    └── Pomodoro/
        ├── launch.sh
        ├── pomodoro
        └── icon.png
```

You can install `sdcard_package/` onto your RG DS Plus using any of the three methods below:

### Method A: Copy to a MicroSD Card (Recommended)

1. Insert your RG DS Plus microSD card (**TF1** or **TF2**, mounted as `/mnt/mmc` or `/mnt/sdcard` on the device) into your computer.
2. Run the automated SD card deployment target (or pass the mount point explicitly):
   ```bash
   make deploy-sd
   # Or specify the mount path of the ROMS partition:
   make deploy-sd SDCARD_PATH=/media/$USER/ROMS
   ```
   Alternatively, copy the folders manually to the root of the `ROMS` partition:
   ```bash
   cp -r sdcard_package/Roms sdcard_package/Ports /path/to/sdcard/
   ```
3. Eject the microSD card, insert it into your RG DS Plus, open **App Center (`APPS`)** (or **Ports**), and launch **Pomodoro**.

### Method B: Push Over Wi-Fi via SSH (`make deploy-ssh`)

The RG DS Plus stock firmware runs an OpenSSH server (`root` / password `root`, hostname `anbernic.local`):

```bash
# Deploy to anbernic.local (default) or specify your RG DS Plus IP address:
make deploy-ssh RGDS_HOST=192.168.1.88

# Or deploy AND launch immediately on the dual screens over SSH:
sdk/scripts/deploy_over_ssh.sh 192.168.1.88 --run
```

### Method C: Inject Into an Unmounted `.IMG` Firmware File (`make deploy-img`)

If you have an RG DS Plus disk image (`.IMG`) on your computer before flashing to an SD card:

```bash
make deploy-img IMG_PATH=<path_to>/RG-DS-PLUS-EN16GB-V1.0-20260915.IMG
```

---

## 6. Where Stats & Previous Sessions Are Saved

- **On Anbernic RG DS Plus**:
  - `/mnt/sdcard/Roms/APPS/pomodoro/.pomodoro_stats.cfg` (or `/mnt/mmc/Roms/APPS/pomodoro/.pomodoro_stats.cfg`)
  - `/mnt/sdcard/Roms/APPS/pomodoro/.pomodoro_sessions.log` (or `/mnt/mmc/Roms/APPS/pomodoro/.pomodoro_sessions.log`)
  - `/mnt/sdcard/Roms/APPS/pomodoro/pomodoro_work_log.txt` (human-readable summary + table of all previous sessions)
- **On Linux Desktop Simulator**:
  - `~/.pomodoro_stats.cfg`
  - `~/.pomodoro_sessions.log`
  - `~/pomodoro_work_log.txt`
