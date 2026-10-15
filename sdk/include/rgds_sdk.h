/*
 * rgds_sdk.h - Native C SDK for the Anbernic RG DS Plus (and Linux X11 Desktop Simulator)
 *
 * Target Hardware:
 *   - SoC: Rockchip RK3568 (Quad-core ARM Cortex-A55 @ 1.8GHz, aarch64)
 *   - Displays: Dual 1024x768 (4:3) IPS LCDs via Weston/Wayland (DSI-1 & DSI-2)
 *   - Input: /dev/input/event* ("ANBERNIC-rk3568-keys" + "goodix-ts" GT911 10-point touch)
 *            + SDL2 GameController/Keyboard/TouchFingerEvent
 *   - Firmware: RG-DS-PLUS-EN16GB-V1.0 (Linux 4.19/5.10 + Weston + SDL2)
 *
 * Host Simulator (compiled with -DHOST_X11):
 *   - Opens a single X11 window containing both the Upper (1024x768) and Lower (1024x768)
 *     screens stacked vertically with a clamshell hinge separator.
 */

#ifndef RGDS_SDK_H
#define RGDS_SDK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. DISPLAY CONSTANTS & COLOR MACROS
 * ============================================================================ */

/* Physical hardware resolution of EACH screen on the Anbernic RG DS Plus */
#define RGDS_NATIVE_W   1024
#define RGDS_NATIVE_H    768

/* Default logical 4:3 canvas resolution (3.2x exact scale to 1024x768) */
#define RGDS_CANVAS_W    320
#define RGDS_CANVAS_H    240

/* Screen selector enum */
typedef enum {
    RGDS_SCREEN_TOP    = 0,  /* Upper screen (Display 0 on Weston, or Display 1 if lcdswap=1) */
    RGDS_SCREEN_BOTTOM = 1   /* Lower screen (Display 1 on Weston, or Display 0 if lcdswap=1) */
} rgds_screen_t;

/* 32-bit ARGB8888 / XRGB8888 color helper: 0xFFRRGGBB */
#define RGDS_RGB(r, g, b) \
    (0xFF000000u | (((uint32_t)(r) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(b) & 0xFFu))

/* Standard color palette */
#define RGDS_COLOR_BLACK    RGDS_RGB(0,   0,   0)
#define RGDS_COLOR_WHITE    RGDS_RGB(255, 255, 255)
#define RGDS_COLOR_NAVY     RGDS_RGB(14,  20,  34)
#define RGDS_COLOR_SLATE    RGDS_RGB(28,  38,  60)
#define RGDS_COLOR_BORDER   RGDS_RGB(70,  95,  140)
#define RGDS_COLOR_CYAN     RGDS_RGB(70,  210, 255)
#define RGDS_COLOR_GREEN    RGDS_RGB(60,  230, 120)
#define RGDS_COLOR_YELLOW   RGDS_RGB(255, 215, 60)
#define RGDS_COLOR_ORANGE   RGDS_RGB(255, 145, 50)
#define RGDS_COLOR_RED      RGDS_RGB(255, 75,  85)
#define RGDS_COLOR_GRAY     RGDS_RGB(150, 165, 190)

/* ============================================================================
 * 2. HARDWARE BUTTON DEFINITIONS ("ANBERNIC-rk3568-keys")
 * ============================================================================
 * Raw Linux evdev codes (/dev/input/event*) verified from RG DS Plus firmware:
 *   EV_KEY (type=1):
 *     304 (BTN_SOUTH)  -> A (East face button)
 *     305 (BTN_EAST)   -> B (South face button)
 *     306 (BTN_C)      -> Y (West face button)
 *     307 (BTN_NORTH)  -> X (North face button)
 *     308 (BTN_WEST)   -> L1 (Top left shoulder)
 *     309 (BTN_Z)      -> R1 (Top right shoulder)
 *     310 (BTN_TL)     -> SELECT
 *     311 (BTN_TR)     -> START
 *     312 (BTN_TR2)    -> MENU ('M' function button)
 *     314 (BTN_SELECT) -> L2 (Bottom left trigger)
 *     315 (BTN_START)  -> R2 (Bottom right trigger)
 *     317 (BTN_THUMBL) -> L3 (Left stick click)
 *     318 (BTN_THUMBR) -> R3 (Right stick click)
 *     114 (KEY_VOLUMEDOWN) -> VOL- (Physical Volume Down button)
 *     115 (KEY_VOLUMEUP)   -> VOL+ (Physical Volume Up button)
 *   EV_ABS (type=3):
 *     16  (ABS_HAT0X)  -> D-Pad Left (-1), Neutral (0), Right (+1)
 *     17  (ABS_HAT0Y)  -> D-Pad Up   (-1), Neutral (0), Down  (+1)
 * ============================================================================ */

typedef enum {
    RGDS_BTN_UP       = (1u << 0),
    RGDS_BTN_DOWN     = (1u << 1),
    RGDS_BTN_LEFT     = (1u << 2),
    RGDS_BTN_RIGHT    = (1u << 3),
    RGDS_BTN_A        = (1u << 4),   /* evdev 304 */
    RGDS_BTN_B        = (1u << 5),   /* evdev 305 */
    RGDS_BTN_X        = (1u << 6),   /* evdev 307 */
    RGDS_BTN_Y        = (1u << 7),   /* evdev 306 */
    RGDS_BTN_L1       = (1u << 8),   /* evdev 308 */
    RGDS_BTN_R1       = (1u << 9),   /* evdev 309 */
    RGDS_BTN_L2       = (1u << 10),  /* evdev 314 */
    RGDS_BTN_R2       = (1u << 11),  /* evdev 315 */
    RGDS_BTN_SELECT   = (1u << 12),  /* evdev 310 */
    RGDS_BTN_START    = (1u << 13),  /* evdev 311 */
    RGDS_BTN_MENU     = (1u << 14),  /* evdev 312 ('M' button) */
    RGDS_BTN_L3       = (1u << 15),  /* evdev 317 */
    RGDS_BTN_R3       = (1u << 16),  /* evdev 318 */
    RGDS_BTN_VOL_UP   = (1u << 17),  /* evdev 115 (KEY_VOLUMEUP) */
    RGDS_BTN_VOL_DOWN = (1u << 18)   /* evdev 114 (KEY_VOLUMEDOWN) */
} rgds_button_t;

/* ============================================================================
 * 2B. CAPACITIVE TOUCHSCREEN DEFINITIONS ("goodix-ts", Goodix GT911 on I2C5 0x14)
 * ============================================================================
 * Verified from RG-DS-PLUS-EN16GB-V1.0 kernel (vmlinux gt9xx driver @ 0xc49100),
 * device tree (/i2c@fe5e0000/gt9xx-0@14), and /mnt/vendor/bin/dmenu.bin Touch Test:
 *   - Hardware: Goodix GT911 10-point capacitive touch panel (1024x768) mounted on
 *     the Lower Screen (Weston Display 1, or Display 0 when lcdswap=1).
 *   - Kernel Gate (/sys/class/anbernic_misc/tpctrl):
 *       0 = Touch input ENABLED in kernel gt9xx IRQ handler (allen_get_tpctrl() == 0).
 *       1 = Touch input DISABLED in kernel (dmenu.bin and subscreen/launch.sh write 1
 *           when launching external apps, so rgds_init() writes 0 to re-enable touch).
 *   - Weston Calibration (/tmp/.weston_drm.conf):
 *       "calibration:gt9xx-0:0.5 0 0.5 0 1 0" maps touch across the 2048x768 dual-screen
 *       horizontal desktop onto Display 1 (x: 1024..2048, y: 0..768).
 *   - Linux evdev Multi-Touch Protocol Type B ("goodix-ts"):
 *       EV_ABS (0x03):
 *         ABS_MT_SLOT        (0x2f / 47) -> Finger slot index (0..9)
 *         ABS_MT_TRACKING_ID (0x39 / 57) -> Slot ID (0..9) on touch/move, -1 on release
 *         ABS_MT_POSITION_X  (0x35 / 53) -> Native X coordinate (0..1024)
 *         ABS_MT_POSITION_Y  (0x36 / 54) -> Native Y coordinate (0..768)
 *         ABS_MT_TOUCH_MAJOR (0x30 / 48) -> Touch contact size (0..255)
 *         ABS_MT_WIDTH_MAJOR (0x32 / 50) -> Touch width (0..255)
 *   - SDL2 Touch Events (used by dmenu.bin Touch Panel Test):
 *       SDL_FINGERDOWN (0x700), SDL_FINGERUP (0x701), SDL_FINGERMOTION (0x702)
 * ============================================================================ */

#define RGDS_MAX_TOUCH_POINTS 10

/* State of an individual multi-touch finger slot (0 .. RGDS_MAX_TOUCH_POINTS - 1) */
typedef struct {
    bool          active;        /* True while this finger slot is touching the screen */
    bool          just_pressed;  /* True on the frame this finger first touched down */
    bool          just_released; /* True on the frame this finger lifted off */
    int           id;            /* Slot / tracking ID (0 .. RGDS_MAX_TOUCH_POINTS - 1) */
    int           x;             /* Logical canvas X (0 .. RGDS_CANVAS_W - 1, i.e. 0..319) */
    int           y;             /* Logical canvas Y (0 .. RGDS_CANVAS_H - 1, i.e. 0..239) */
    int           dx;            /* Logical canvas delta X since previous frame */
    int           dy;            /* Logical canvas delta Y since previous frame */
    int           native_x;      /* Native panel X (0 .. RGDS_NATIVE_W - 1, i.e. 0..1023) */
    int           native_y;      /* Native panel Y (0 .. RGDS_NATIVE_H - 1, i.e. 0..767) */
    int           native_dx;     /* Native panel delta X since previous frame */
    int           native_dy;     /* Native panel delta Y since previous frame */
    int           pressure;      /* Contact size / pressure (0..255, or 0 when released) */
    rgds_screen_t screen;        /* Screen touched (RGDS_SCREEN_BOTTOM, or TOP in sim/swap) */
} rgds_touch_point_t;

/* Aggregate touchscreen state (convenient primary-finger fields + 10-point multi-touch array) */
typedef struct {
    bool               touching;      /* True if at least one finger is currently touching */
    bool               just_pressed;  /* True on the frame a touch began */
    bool               just_released; /* True on the frame a touch ended */
    int                count;         /* Number of currently active fingers (0..10) */
    int                x;             /* Primary touch logical canvas X (0..319) */
    int                y;             /* Primary touch logical canvas Y (0..239) */
    int                dx;            /* Primary touch logical canvas delta X */
    int                dy;            /* Primary touch logical canvas delta Y */
    int                native_x;      /* Primary touch native X (0..1023) */
    int                native_y;      /* Primary touch native Y (0..767) */
    int                native_dx;     /* Primary touch native delta X */
    int                native_dy;     /* Primary touch native delta Y */
    int                pressure;      /* Primary touch contact size/pressure (0..255) */
    rgds_screen_t      screen;        /* Screen of primary touch (default RGDS_SCREEN_BOTTOM) */
    rgds_touch_point_t points[RGDS_MAX_TOUCH_POINTS]; /* All 10 multi-touch finger slots */
} rgds_touch_state_t;

typedef struct {
    uint32_t           held;       /* Bitmask of buttons currently held down */
    uint32_t           pressed;    /* Bitmask of buttons newly pressed this frame (rising edge) */
    uint32_t           released;   /* Bitmask of buttons newly released this frame (falling edge) */
    bool               quit_req;   /* True if window close, ESC/Q, MENU, or START+SELECT was triggered */
    rgds_touch_state_t touch;      /* Capacitive touchscreen state (single-touch + 10-point multi-touch) */
} rgds_input_state_t;

/* ============================================================================
 * 3. CORE LIFECYCLE, TOUCHSCREEN & VOLUME CONTROL API
 * ============================================================================ */

/*
 * Initialize the RG DS Plus SDK:
 *   - On RG DS Plus hardware:
 *       1. Signals /sys/class/anbernic_misc/runapp = 0 and stops subscreen.dge so the
 *          application has exclusive ownership of Display 1 (Lower Screen), then sets
 *          runapp = 1 while running.
 *       2. Ensures both 1024x768 LCDs are powered on (/sys/class/anbernic_misc/lcdoff = 0)
 *          and records the previous lcdoff value to restore on shutdown.
 *       3. Enables kernel touchscreen reporting (/sys/class/anbernic_misc/tpctrl = 0)
 *          and configures Weston dual-display touch calibration in /tmp/.weston_drm.conf.
 *       4. Respects /sys/class/anbernic_misc/lcdswap (or SDL2_SWAP_LCD=1).
 *       5. Opens two 1024x768 fullscreen SDL2 windows on Weston Display 0 and Display 1
 *          (with automatic fallback to /dev/fb0 + /dev/fb1 if needed).
 *       6. Opens /dev/input/event* ("ANBERNIC-rk3568-keys" and "goodix-ts") in non-blocking
 *          mode and synchronizes system audio volume from /sys/class/anbernic_misc/openbor_volume.
 *   - On Linux Host (-DHOST_X11):
 *       Opens a dual-screen X11 simulator window showing both 1024x768 screens with mouse
 *       click-and-drag touchscreen emulation.
 *
 * Returns 0 on success, or -1 on error.
 */
int rgds_init(const char *app_title);

/*
 * Cleanly shut down the SDK, destroy windows, close input devices, and restore
 * any modified RG DS Plus hardware state (/sys/class/anbernic_misc/lcdoff and
 * /tmp/.weston_drm.conf).
 */
void rgds_shutdown(void);

/*
 * Poll all hardware buttons and touchscreen events (evdev + SDL2 events or X11
 * keyboard/mouse) and populate `out_input`.
 * Also handles physical Volume Up (115) / Volume Down (114) presses (and hold auto-repeat)
 * by updating /sys/class/anbernic_misc/openbor_volume and PulseAudio sink volume via
 * /mnt/vendor/ctrl/setPulseaudio.sh (or screen brightness when SELECT/START is held).
 * Returns false if the user requested application exit (MENU, START+SELECT, ESC, or window close).
 */
bool rgds_poll_input(rgds_input_state_t *out_input);

/*
 * Enable or disable hardware touchscreen input reporting.
 * On RG DS Plus hardware, controls /sys/class/anbernic_misc/tpctrl
 * (0 = touch enabled in kernel gt9xx driver, 1 = touch disabled in kernel)
 * and updates /tmp/.weston_drm.conf calibration for gt9xx-0.
 * Touchscreen reporting is enabled automatically by rgds_init().
 */
void rgds_set_touch_enabled(bool enabled);

/*
 * Return true if touchscreen input reporting is currently enabled.
 */
bool rgds_is_touch_enabled(void);

/*
 * Get the current system audio volume step (0..10, where 0 = Mute, 10 = 100%).
 */
int rgds_get_volume(void);

/*
 * Set the system audio volume step (0..10, where 0 = Mute, 10 = 100%).
 * On RG DS Plus hardware, writes /sys/class/anbernic_misc/openbor_volume and
 * updates the PulseAudio speaker/headphone sinks via /mnt/vendor/ctrl/setPulseaudio.sh.
 */
void rgds_set_volume(int level_0_to_10);

/*
 * Convert a 0..10 volume step to the stock firmware PulseAudio sink percentage
 * ({ 0, 22, 36, 44, 50, 56, 62, 70, 80, 90, 100 }).
 */
int rgds_volume_to_percent(int level_0_to_10);

/*
 * Present both the Upper and Lower screens to the hardware displays.
 * Automatically scales the 320x240 logical canvases (if used) to the 1024x768 native
 * framebuffers before flipping both screens.
 */
void rgds_present(void);

/*
 * Return monotonic time in milliseconds since an arbitrary epoch.
 */
uint64_t rgds_ticks_ms(void);

/*
 * Sleep for the specified number of milliseconds.
 */
void rgds_delay_ms(uint32_t ms);

/*
 * Return a human-readable description of the active video backend
 * (e.g., "SDL2 Dual-Window Weston (1024x768 x 2)" or "Linux X11 Dual-Screen Simulator").
 */
const char *rgds_backend_name(void);

/* ============================================================================
 * 4. DRAWING API (LOGICAL 320x240 CANVAS & NATIVE 1024x768 ACCESS)
 * ============================================================================
 * By default, drawing functions operate on the 320x240 4:3 logical canvas for each
 * screen, which is upscaled by 3.2x via a fast precomputed LUT to 1024x768 on present.
 *
 * If you prefer to write directly at native 1024x768 resolution, call
 * `rgds_set_direct_native_mode(true)` and draw into `rgds_get_native_buffer(screen)`.
 * ============================================================================ */

/* Enable/disable direct 1024x768 native framebuffer drawing (default: false = 320x240 canvas) */
void rgds_set_direct_native_mode(bool enabled);

/* Get direct pointer to the 320x240 XRGB8888 logical canvas for `screen` */
uint32_t *rgds_get_canvas_buffer(rgds_screen_t screen);

/* Get direct pointer to the 1024x768 XRGB8888 native framebuffer for `screen` */
uint32_t *rgds_get_native_buffer(rgds_screen_t screen);

/* Clear the entire screen to `color` (0x00RRGGBB) */
void rgds_clear(rgds_screen_t screen, uint32_t color);

/* Draw a single pixel on `screen` */
void rgds_draw_pixel(rgds_screen_t screen, int x, int y, uint32_t color);

/* Draw a filled rectangle on `screen` */
void rgds_fill_rect(rgds_screen_t screen, int x, int y, int w, int h, uint32_t color);

/* Draw a rectangle outline of `thickness` pixels on `screen` */
void rgds_draw_rect(rgds_screen_t screen, int x, int y, int w, int h, int thickness, uint32_t color);

/* Measure pixel width of an ASCII string at integer `scale` (each char is 6*scale wide) */
int rgds_text_width(const char *text, int scale);

/* Draw an ASCII string at `(x, y)` with integer `scale` (base glyph size is 5x7) */
void rgds_draw_text(rgds_screen_t screen, int x, int y, const char *text, int scale, uint32_t color);

/* Draw an ASCII string horizontally centered at vertical coordinate `y` */
void rgds_draw_text_centered(rgds_screen_t screen, int y, const char *text, int scale, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif /* RGDS_SDK_H */
