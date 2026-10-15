/*
 * rgds_sdk.c - Implementation of the Anbernic RG DS Plus C SDK
 *
 * Supports:
 *   1. Anbernic RG DS Plus Hardware (aarch64 Linux, Weston/Wayland + SDL2 dual 1024x768
 *      windows, /dev/fb0 + /dev/fb1 fallback, and /dev/input/event* "ANBERNIC-rk3568-keys").
 *   2. Linux Desktop Simulator (-DHOST_X11, Xlib dual-screen stacked window).
 */

#include "rgds_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dlfcn.h>

#ifdef HOST_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#else
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#endif

/* ============================================================================
 * INTERNAL STATE & FRAMEBUFFERS
 * ============================================================================ */

/* Logical 320x240 canvases (4:3) */
static uint32_t g_canvas_top[RGDS_CANVAS_W * RGDS_CANVAS_H];
static uint32_t g_canvas_bottom[RGDS_CANVAS_W * RGDS_CANVAS_H];

/* Native 1024x768 hardware framebuffers (4:3, 3.2x logical resolution) */
static uint32_t g_native_top[RGDS_NATIVE_W * RGDS_NATIVE_H];
static uint32_t g_native_bottom[RGDS_NATIVE_W * RGDS_NATIVE_H];

/* Precomputed LUT for fast 320x240 -> 1024x768 scaling */
static int g_lut_x[RGDS_NATIVE_W];
static int g_lut_y[RGDS_NATIVE_H];
static bool g_lut_ready = false;
static bool g_direct_native_mode = false;

static uint32_t g_prev_held = 0;
static bool g_quit_requested = false;
static char g_backend_desc[128] = "Uninitialized";

/*
 * Internal 10-point multi-touch state table (shared by hardware Goodix GT911
 * "goodix-ts" evdev, SDL2 TouchFingerEvent fallback, and X11 simulator).
 */
typedef struct {
    bool          down;
    bool          prev_down;
    bool          pressed_latch;
    bool          released_latch;
    int           raw_x;
    int           raw_y;
    int           prev_x;
    int           prev_y;
    int           prev_native_x;
    int           prev_native_y;
    int           pressure;
    rgds_screen_t screen;
} rgds_internal_touch_slot_t;

static rgds_internal_touch_slot_t g_touch_slots[RGDS_MAX_TOUCH_POINTS];
static rgds_touch_state_t         g_touch_state;
static bool                       g_touch_enabled = true;
static int                        g_lcd_swap = 0;

bool rgds_is_touch_enabled(void) {
    return g_touch_enabled;
}

static inline int clamp_int(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void rgds_reset_touch_slots(void) {
    rgds_screen_t def_scr = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
    memset(g_touch_slots, 0, sizeof(g_touch_slots));
    memset(&g_touch_state, 0, sizeof(g_touch_state));
    g_touch_state.screen = def_scr;
    for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
        g_touch_slots[s].screen = def_scr;
        g_touch_state.points[s].id = s;
        g_touch_state.points[s].screen = def_scr;
    }
}

static void rgds_finalize_touch_frame(rgds_touch_state_t *out_touch) {
    int  active_count      = 0;
    int  primary_active    = -1;
    int  primary_edge      = -1;
    bool any_just_pressed  = false;
    bool any_just_released = false;
    bool prev_touching     = g_touch_state.touching;

    for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
        rgds_internal_touch_slot_t *slot = &g_touch_slots[s];
        rgds_touch_point_t         *pt   = &g_touch_state.points[s];

        bool act = g_touch_enabled && slot->down;
        bool jp  = g_touch_enabled && (slot->pressed_latch || (act && !slot->prev_down));
        bool jr  = slot->released_latch || (!act && slot->prev_down);

        int nx = clamp_int(slot->raw_x, 0, RGDS_NATIVE_W - 1);
        int ny = clamp_int(slot->raw_y, 0, RGDS_NATIVE_H - 1);
        int cx = (nx * RGDS_CANVAS_W) / RGDS_NATIVE_W;
        int cy = (ny * RGDS_CANVAS_H) / RGDS_NATIVE_H;

        int dx = 0, dy = 0, ndx = 0, ndy = 0;
        if (act && !jp && slot->prev_down) {
            dx  = cx - slot->prev_x;
            dy  = cy - slot->prev_y;
            ndx = nx - slot->prev_native_x;
            ndy = ny - slot->prev_native_y;
        }

        pt->active        = act;
        pt->just_pressed  = jp;
        pt->just_released = jr;
        pt->id            = s;
        if (act || jp || jr) {
            pt->x        = cx;
            pt->y        = cy;
            pt->native_x = nx;
            pt->native_y = ny;
            pt->screen   = slot->screen;
        }
        pt->dx        = dx;
        pt->dy        = dy;
        pt->native_x  = clamp_int(pt->native_x, 0, RGDS_NATIVE_W - 1);
        pt->native_y  = clamp_int(pt->native_y, 0, RGDS_NATIVE_H - 1);
        pt->native_dx = ndx;
        pt->native_dy = ndy;
        pt->pressure  = act ? (slot->pressure > 0 ? slot->pressure : 128) : 0;

        slot->prev_down      = act;
        slot->pressed_latch  = false;
        slot->released_latch = false;
        if (act || jp) {
            slot->prev_x        = cx;
            slot->prev_y        = cy;
            slot->prev_native_x = nx;
            slot->prev_native_y = ny;
        }

        if (act) {
            active_count++;
            if (primary_active < 0) primary_active = s;
        }
        if (jp) {
            any_just_pressed = true;
            if (primary_edge < 0) primary_edge = s;
        }
        if (jr) {
            any_just_released = true;
            if (primary_edge < 0) primary_edge = s;
        }
    }

    g_touch_state.touching      = (active_count > 0);
    g_touch_state.count         = active_count;
    g_touch_state.just_pressed  = any_just_pressed || (!prev_touching && active_count > 0);
    g_touch_state.just_released = (any_just_released && active_count == 0) ||
                                  (prev_touching && active_count == 0);

    int ref = (primary_active >= 0) ? primary_active : primary_edge;
    if (ref >= 0) {
        const rgds_touch_point_t *p = &g_touch_state.points[ref];
        g_touch_state.x         = p->x;
        g_touch_state.y         = p->y;
        g_touch_state.dx        = p->dx;
        g_touch_state.dy        = p->dy;
        g_touch_state.native_x  = p->native_x;
        g_touch_state.native_y  = p->native_y;
        g_touch_state.native_dx = p->native_dx;
        g_touch_state.native_dy = p->native_dy;
        g_touch_state.pressure  = p->pressure;
        g_touch_state.screen    = p->screen;
    } else {
        g_touch_state.dx        = 0;
        g_touch_state.dy        = 0;
        g_touch_state.native_dx = 0;
        g_touch_state.native_dy = 0;
        g_touch_state.pressure  = 0;
    }

    if (out_touch) {
        *out_touch = g_touch_state;
    }
}

/*
 * System audio volume step (0..10) & PulseAudio sink percentage lookup table
 * verified from /mnt/vendor/bin/portsCtrl.dge (.rodata 0x5af4).
 */
static const int k_rgds_vol_pct[11] = {
    0, 22, 36, 44, 50, 56, 62, 70, 80, 90, 100
};
static int g_volume_level = 10;
static int g_vol_repeat_dir = 0;
static uint64_t g_vol_next_repeat_ms = 0;
#define RGDS_VOL_REPEAT_DELAY_MS    320ULL
#define RGDS_VOL_REPEAT_INTERVAL_MS  85ULL

int rgds_volume_to_percent(int level_0_to_10) {
    if (level_0_to_10 < 0)  level_0_to_10 = 0;
    if (level_0_to_10 > 10) level_0_to_10 = 10;
    return k_rgds_vol_pct[level_0_to_10];
}

int rgds_get_volume(void) {
    return g_volume_level;
}

static void init_scale_lut(void) {
    if (g_lut_ready) return;
    for (int x = 0; x < RGDS_NATIVE_W; x++) {
        g_lut_x[x] = (x * RGDS_CANVAS_W) / RGDS_NATIVE_W;
    }
    for (int y = 0; y < RGDS_NATIVE_H; y++) {
        g_lut_y[y] = (y * RGDS_CANVAS_H) / RGDS_NATIVE_H;
    }
    g_lut_ready = true;
}

static void upscale_canvas_to_native(const uint32_t *src, uint32_t *dst) {
    init_scale_lut();
    for (int y = 0; y < RGDS_NATIVE_H; y++) {
        const uint32_t *src_row = src + (g_lut_y[y] * RGDS_CANVAS_W);
        uint32_t *dst_row = dst + (y * RGDS_NATIVE_W);
        for (int x = 0; x < RGDS_NATIVE_W; x++) {
            dst_row[x] = src_row[g_lut_x[x]];
        }
    }
}

/* ============================================================================
 * BUILT-IN 5x7 BITMAP FONT (ASCII 32..126)
 * ============================================================================ */

static void get_glyph_row(char c, int row, uint8_t *bits) {
    *bits = 0;
    if (row < 0 || row >= 7) return;
    static const uint8_t font5x7[][7] = {
        /* ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        /* '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
        /* '"' */ {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00},
        /* '#' */ {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},
        /* '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
        /* '%' */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
        /* '&' */ {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
        /* '\''*/ {0x04,0x04,0x08,0x00,0x00,0x00,0x00},
        /* '(' */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
        /* ')' */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
        /* '*' */ {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
        /* '+' */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
        /* ',' */ {0x00,0x00,0x00,0x00,0x04,0x04,0x08},
        /* '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
        /* '.' */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
        /* '/' */ {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
        /* '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
        /* '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        /* '2' */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        /* '3' */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        /* '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
        /* '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        /* '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
        /* '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        /* '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        /* '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
        /* ':' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
        /* ';' */ {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
        /* '<' */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
        /* '=' */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
        /* '>' */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
        /* '?' */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
        /* '@' */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
        /* 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
        /* 'B' */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        /* 'C' */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
        /* 'D' */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
        /* 'E' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
        /* 'F' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        /* 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
        /* 'H' */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        /* 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        /* 'J' */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
        /* 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        /* 'L' */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        /* 'M' */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
        /* 'N' */ {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
        /* 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        /* 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        /* 'Q' */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
        /* 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        /* 'S' */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
        /* 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        /* 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        /* 'V' */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        /* 'W' */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
        /* 'X' */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        /* 'Y' */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
        /* 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
        /* '[' */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
        /* '\\'*/ {0x10,0x10,0x08,0x04,0x02,0x01,0x01},
        /* ']' */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
        /* '^' */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
        /* '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    };
    if (c == '|') {
        *bits = 0x04;
        return;
    }
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 32 && c <= 95) {
        *bits = font5x7[(int)c - 32][row];
    }
}

/* ============================================================================
 * DRAWING FUNCTIONS
 * ============================================================================ */

void rgds_set_direct_native_mode(bool enabled) {
    g_direct_native_mode = enabled;
}

uint32_t *rgds_get_canvas_buffer(rgds_screen_t screen) {
    return (screen == RGDS_SCREEN_TOP) ? g_canvas_top : g_canvas_bottom;
}

uint32_t *rgds_get_native_buffer(rgds_screen_t screen) {
    return (screen == RGDS_SCREEN_TOP) ? g_native_top : g_native_bottom;
}

static inline int active_w(void) {
    return g_direct_native_mode ? RGDS_NATIVE_W : RGDS_CANVAS_W;
}

static inline int active_h(void) {
    return g_direct_native_mode ? RGDS_NATIVE_H : RGDS_CANVAS_H;
}

static inline uint32_t *active_buf(rgds_screen_t screen) {
    if (g_direct_native_mode) {
        return (screen == RGDS_SCREEN_TOP) ? g_native_top : g_native_bottom;
    }
    return (screen == RGDS_SCREEN_TOP) ? g_canvas_top : g_canvas_bottom;
}

void rgds_clear(rgds_screen_t screen, uint32_t color) {
    uint32_t *buf = active_buf(screen);
    int total = active_w() * active_h();
    for (int i = 0; i < total; i++) {
        buf[i] = color;
    }
}

void rgds_draw_pixel(rgds_screen_t screen, int x, int y, uint32_t color) {
    int w = active_w();
    int h = active_h();
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    active_buf(screen)[y * w + x] = color;
}

void rgds_fill_rect(rgds_screen_t screen, int x, int y, int w, int h, uint32_t color) {
    int sw = active_w();
    int sh = active_h();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    uint32_t *buf = active_buf(screen);
    for (int row = y; row < y + h; row++) {
        uint32_t *dst = buf + (row * sw + x);
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

void rgds_draw_rect(rgds_screen_t screen, int x, int y, int w, int h, int thickness, uint32_t color) {
    if (thickness <= 0 || w <= 0 || h <= 0) return;
    rgds_fill_rect(screen, x, y, w, thickness, color);
    rgds_fill_rect(screen, x, y + h - thickness, w, thickness, color);
    rgds_fill_rect(screen, x, y, thickness, h, color);
    rgds_fill_rect(screen, x + w - thickness, y, thickness, h, color);
}

int rgds_text_width(const char *text, int scale) {
    if (!text || !*text || scale <= 0) return 0;
    int len = (int)strlen(text);
    return (len * 6 - 1) * scale;
}

void rgds_draw_text(rgds_screen_t screen, int x, int y, const char *text, int scale, uint32_t color) {
    if (!text || scale <= 0) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        for (int r = 0; r < 7; r++) {
            uint8_t bits = 0;
            get_glyph_row(*p, r, &bits);
            for (int col = 0; col < 5; col++) {
                if (bits & (1u << (4 - col))) {
                    rgds_fill_rect(screen, cx + col * scale, y + r * scale, scale, scale, color);
                }
            }
        }
        cx += 6 * scale;
    }
}

void rgds_draw_text_centered(rgds_screen_t screen, int y, const char *text, int scale, uint32_t color) {
    int tw = rgds_text_width(text, scale);
    int x = (active_w() - tw) / 2;
    rgds_draw_text(screen, x, y, text, scale, color);
}

/* ============================================================================
 * TIMING HELPERS
 * ============================================================================ */

uint64_t rgds_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

void rgds_delay_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

const char *rgds_backend_name(void) {
    return g_backend_desc;
}

/* ============================================================================
 * PLATFORM BACKEND: HOST X11 SIMULATOR (-DHOST_X11) vs RG DS PLUS HARDWARE
 * ============================================================================ */

#ifdef HOST_X11

#define X11_HINGE_H  8

static Display *g_x11_dpy = NULL;
static Window   g_x11_win = 0;
static GC       g_x11_gc  = 0;
static XImage  *g_x11_img = NULL;
static uint32_t *g_x11_buf = NULL;
static Atom     g_wm_delete = 0;
static int      g_sim_w = 640;  /* Default 2x integer scale of 320x240 (640x480 x 2 = 640x968) */
static int      g_sim_h = 480;
static int      g_total_h = 480 * 2 + X11_HINGE_H;
static uint32_t g_x11_held = 0;
static bool     g_x11_mouse_down = false;

static void x11_update_touch_from_mouse(int mx, int my, bool down, bool is_press_edge, bool is_release_edge) {
    if (!g_touch_enabled || g_sim_w <= 0 || g_sim_h <= 0) return;

    rgds_screen_t scr = g_touch_slots[0].screen;
    int local_y = 0;

    if (my < g_sim_h) {
        scr = RGDS_SCREEN_TOP;
        local_y = clamp_int(my, 0, g_sim_h - 1);
    } else if (my >= g_sim_h + X11_HINGE_H) {
        scr = RGDS_SCREEN_BOTTOM;
        local_y = clamp_int(my - (g_sim_h + X11_HINGE_H), 0, g_sim_h - 1);
    } else {
        /* Inside the 8px hinge gap: clamp to whichever screen the drag started on */
        if (scr == RGDS_SCREEN_TOP) {
            local_y = g_sim_h - 1;
        } else {
            local_y = 0;
        }
    }

    int local_x = clamp_int(mx, 0, g_sim_w - 1);
    int nx = clamp_int((local_x * RGDS_NATIVE_W) / g_sim_w, 0, RGDS_NATIVE_W - 1);
    int ny = clamp_int((local_y * RGDS_NATIVE_H) / g_sim_h, 0, RGDS_NATIVE_H - 1);

    g_touch_slots[0].raw_x    = nx;
    g_touch_slots[0].raw_y    = ny;
    g_touch_slots[0].screen   = scr;
    g_touch_slots[0].down     = down;
    g_touch_slots[0].pressure = down ? 200 : 0;
    if (is_press_edge)   g_touch_slots[0].pressed_latch  = true;
    if (is_release_edge) g_touch_slots[0].released_latch = true;
}

void rgds_set_touch_enabled(bool enabled) {
    g_touch_enabled = enabled;
    if (!enabled) {
        g_x11_mouse_down = false;
        rgds_reset_touch_slots();
    }
}

int rgds_init(const char *app_title) {
    init_scale_lut();
    g_touch_enabled = true;
    rgds_reset_touch_slots();

    if (getenv("RGDS_FULL_1024")) {
        g_sim_w = RGDS_NATIVE_W;
        g_sim_h = RGDS_NATIVE_H;
    } else {
        g_sim_w = 640;
        g_sim_h = 480;
    }
    g_total_h = g_sim_h * 2 + X11_HINGE_H;

    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) {
        fprintf(stderr, "[rgds_sdk] ERROR: Cannot open X11 display. Check DISPLAY env var.\n");
        return -1;
    }

    Bool supported = False;
    XkbSetDetectableAutoRepeat(g_x11_dpy, True, &supported);

    int screen = DefaultScreen(g_x11_dpy);
    g_x11_win = XCreateSimpleWindow(
        g_x11_dpy, RootWindow(g_x11_dpy, screen),
        40, 20, (unsigned int)g_sim_w, (unsigned int)g_total_h, 1,
        BlackPixel(g_x11_dpy, screen), BlackPixel(g_x11_dpy, screen)
    );

    char title_buf[256];
    snprintf(title_buf, sizeof(title_buf), "%s [RG DS Plus Dual 1024x768 X11 Simulator]",
             app_title ? app_title : "RG DS Plus App");
    XStoreName(g_x11_dpy, g_x11_win, title_buf);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = g_sim_w;
    hints.min_height = hints.max_height = g_total_h;
    XSetWMNormalHints(g_x11_dpy, g_x11_win, &hints);

    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags = InputHint | StateHint;
    wm_hints.input = True;
    wm_hints.initial_state = NormalState;
    XSetWMHints(g_x11_dpy, g_x11_win, &wm_hints);

    XSelectInput(g_x11_dpy, g_x11_win,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | Button1MotionMask |
                 FocusChangeMask | StructureNotifyMask);
    g_wm_delete = XInternAtom(g_x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11_dpy, g_x11_win, &g_wm_delete, 1);

    XMapRaised(g_x11_dpy, g_x11_win);
    XFlush(g_x11_dpy);
    g_x11_gc = DefaultGC(g_x11_dpy, screen);

    g_x11_buf = (uint32_t *)calloc((size_t)g_sim_w * (size_t)g_total_h, sizeof(uint32_t));
    g_x11_img = XCreateImage(
        g_x11_dpy, DefaultVisual(g_x11_dpy, screen),
        (unsigned int)DefaultDepth(g_x11_dpy, screen), ZPixmap, 0,
        (char *)g_x11_buf, (unsigned int)g_sim_w, (unsigned int)g_total_h, 32, 0
    );

    snprintf(g_backend_desc, sizeof(g_backend_desc),
             "X11 Dual-Screen Simulator (%dx%d x 2 -> 1024x768 x 2)", g_sim_w, g_sim_h);
    return 0;
}

static uint32_t keysym_to_rgds_btn(KeySym sym) {
    switch (sym) {
        case XK_Up:
        case XK_KP_Up:
        case XK_w:
        case XK_W:             return RGDS_BTN_UP;
        case XK_Down:
        case XK_KP_Down:      return RGDS_BTN_DOWN;
        case XK_plus:
        case XK_equal:
        case XK_KP_Add:       return RGDS_BTN_VOL_UP;
        case XK_minus:
        case XK_underscore:
        case XK_KP_Subtract:  return RGDS_BTN_VOL_DOWN;
        case XK_Left:
        case XK_KP_Left:      return RGDS_BTN_LEFT;
        case XK_Right:
        case XK_KP_Right:     return RGDS_BTN_RIGHT;
        case XK_a:
        case XK_A:
        case XK_z:
        case XK_Z:
        case XK_Alt_L:
        case XK_Alt_R:
        case XK_space:
        case XK_KP_Space:     return RGDS_BTN_A;
        case XK_b:
        case XK_B:
        case XK_Control_L:
        case XK_Control_R:
        case XK_BackSpace:    return RGDS_BTN_B;
        case XK_x:
        case XK_X:
        case XK_s:
        case XK_S:
        case XK_Shift_L:      return RGDS_BTN_X;
        case XK_y:
        case XK_Y:
        case XK_r:
        case XK_R:            return RGDS_BTN_Y;
        case XK_q:
        case XK_Q:
        case XK_bracketleft:  return RGDS_BTN_L1;
        case XK_e:
        case XK_E:
        case XK_bracketright: return RGDS_BTN_R1;
        case XK_1:
        case XK_Page_Down:
        case XK_KP_Page_Down: return RGDS_BTN_L2;
        case XK_2:
        case XK_Page_Up:
        case XK_KP_Page_Up:   return RGDS_BTN_R2;
        case XK_Shift_R:
        case XK_Tab:
        case XK_ISO_Left_Tab: return RGDS_BTN_SELECT;
        case XK_Return:
        case XK_KP_Enter:     return RGDS_BTN_START;
        case XK_m:
        case XK_M:            return RGDS_BTN_MENU;
        default:              return 0;
    }
}

static uint32_t xkey_to_rgds_btn(XKeyEvent *xkey, bool *out_escape) {
    char buf[8] = {0};
    KeySym sym = NoSymbol;
    int n = XLookupString(xkey, buf, sizeof(buf) - 1, &sym, NULL);
    if (sym == NoSymbol) {
        sym = XLookupKeysym(xkey, 0);
    }
    if (sym == XK_Escape || (n > 0 && buf[0] == 27)) {
        if (out_escape) *out_escape = true;
    }
    uint32_t btn = keysym_to_rgds_btn(sym);
    if (!btn) {
        KeySym base_sym = XLookupKeysym(xkey, 0);
        if (base_sym != NoSymbol && base_sym != sym) {
            if (base_sym == XK_Escape && out_escape) *out_escape = true;
            btn = keysym_to_rgds_btn(base_sym);
        }
    }
    if (!btn && n > 0) {
        switch (buf[0]) {
            case 'a': case 'A': case ' ': case 'z': case 'Z': btn = RGDS_BTN_A; break;
            case 'b': case 'B': case '\b': case 127:          btn = RGDS_BTN_B; break;
            case 'x': case 'X': case 's': case 'S':           btn = RGDS_BTN_X; break;
            case 'y': case 'Y': case 'r': case 'R':           btn = RGDS_BTN_Y; break;
            case 'q': case 'Q': case '[':                     btn = RGDS_BTN_L1; break;
            case 'e': case 'E': case ']':                     btn = RGDS_BTN_R1; break;
            case '1':                                         btn = RGDS_BTN_L2; break;
            case '2':                                         btn = RGDS_BTN_R2; break;
            case '\t':                                        btn = RGDS_BTN_SELECT; break;
            case '\r': case '\n':                             btn = RGDS_BTN_START; break;
            case 'm': case 'M':                               btn = RGDS_BTN_MENU; break;
            case '+': case '=':                               btn = RGDS_BTN_VOL_UP; break;
            case '-': case '_':                               btn = RGDS_BTN_VOL_DOWN; break;
            default: break;
        }
    }
    return btn;
}

void rgds_set_volume(int level_0_to_10) {
    if (level_0_to_10 < 0)  level_0_to_10 = 0;
    if (level_0_to_10 > 10) level_0_to_10 = 10;
    g_volume_level = level_0_to_10;
}

bool rgds_poll_input(rgds_input_state_t *out_input) {
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;

    while (g_x11_dpy && XPending(g_x11_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_x11_dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g_wm_delete) {
            g_quit_requested = true;
        } else if (ev.type == MapNotify) {
            XSetInputFocus(g_x11_dpy, g_x11_win, RevertToParent, CurrentTime);
        } else if (ev.type == ButtonPress) {
            XSetInputFocus(g_x11_dpy, g_x11_win, RevertToParent, CurrentTime);
            /* Mouse wheel scroll: Button4 = Up/ScrollUp, Button5 = Down/ScrollDown */
            if (ev.xbutton.button == Button4) {
                if (ev.xbutton.y < g_sim_h) pressed_latch |= RGDS_BTN_UP;
                else                        pressed_latch |= RGDS_BTN_L2;
            } else if (ev.xbutton.button == Button5) {
                if (ev.xbutton.y < g_sim_h) pressed_latch |= RGDS_BTN_DOWN;
                else                        pressed_latch |= RGDS_BTN_R2;
            } else if (ev.xbutton.button == Button1) {
                g_x11_mouse_down = true;
                x11_update_touch_from_mouse(ev.xbutton.x, ev.xbutton.y, true, true, false);
            }
        } else if (ev.type == ButtonRelease) {
            if (ev.xbutton.button == Button1) {
                g_x11_mouse_down = false;
                x11_update_touch_from_mouse(ev.xbutton.x, ev.xbutton.y, false, false, true);
            }
        } else if (ev.type == MotionNotify) {
            if (g_x11_mouse_down || (ev.xmotion.state & Button1Mask)) {
                g_x11_mouse_down = true;
                x11_update_touch_from_mouse(ev.xmotion.x, ev.xmotion.y, true, false, false);
            }
        } else if (ev.type == KeyPress) {
            bool is_esc = false;
            uint32_t btn = xkey_to_rgds_btn(&ev.xkey, &is_esc);
            if (is_esc) {
                g_quit_requested = true;
            }
            if (btn) {
                /* Latch rising edge even if KeyRelease arrives in the same XPending batch */
                pressed_latch |= (btn & ~g_x11_held);
                g_x11_held |= btn;
            }
        } else if (ev.type == KeyRelease) {
            /* Filter X11 auto-repeat KeyRelease/KeyPress pairs */
            if (XEventsQueued(g_x11_dpy, QueuedAfterReading)) {
                XEvent nev;
                XPeekEvent(g_x11_dpy, &nev);
                if (nev.type == KeyPress && nev.xkey.time == ev.xkey.time &&
                    nev.xkey.keycode == ev.xkey.keycode) {
                    XNextEvent(g_x11_dpy, &ev);
                    continue;
                }
            }
            uint32_t btn = xkey_to_rgds_btn(&ev.xkey, NULL);
            if (btn) {
                released_latch |= (btn & g_x11_held);
                g_x11_held &= ~btn;
            }
        }
    }

    uint32_t newly_pressed = pressed_latch | (g_x11_held & ~g_prev_held);
    if (newly_pressed & RGDS_BTN_VOL_UP) {
        rgds_set_volume(g_volume_level + 1);
        g_vol_repeat_dir = 1;
        g_vol_next_repeat_ms = rgds_ticks_ms() + RGDS_VOL_REPEAT_DELAY_MS;
    } else if (newly_pressed & RGDS_BTN_VOL_DOWN) {
        rgds_set_volume(g_volume_level - 1);
        g_vol_repeat_dir = -1;
        g_vol_next_repeat_ms = rgds_ticks_ms() + RGDS_VOL_REPEAT_DELAY_MS;
    }
    if (!(g_x11_held & (RGDS_BTN_VOL_UP | RGDS_BTN_VOL_DOWN))) {
        g_vol_repeat_dir = 0;
    } else if (g_vol_repeat_dir != 0) {
        uint64_t now = rgds_ticks_ms();
        if (now >= g_vol_next_repeat_ms) {
            g_vol_next_repeat_ms = now + RGDS_VOL_REPEAT_INTERVAL_MS;
            rgds_set_volume(g_volume_level + g_vol_repeat_dir);
        }
    }

    if (((g_x11_held | pressed_latch) & (RGDS_BTN_START | RGDS_BTN_SELECT)) == (RGDS_BTN_START | RGDS_BTN_SELECT)) {
        g_quit_requested = true;
    }

    rgds_finalize_touch_frame(out_input ? &out_input->touch : NULL);

    if (out_input) {
        out_input->held     = g_x11_held;
        out_input->pressed  = newly_pressed;
        out_input->released = released_latch | (~g_x11_held & g_prev_held);
        out_input->quit_req = g_quit_requested;
    }
    g_prev_held = g_x11_held;
    return !g_quit_requested;
}

void rgds_present(void) {
    if (!g_x11_dpy || !g_x11_img) return;
    if (!g_direct_native_mode) {
        upscale_canvas_to_native(g_canvas_top, g_native_top);
        upscale_canvas_to_native(g_canvas_bottom, g_native_bottom);
    }

    /* Render Upper Screen (1024x768) into top half of X11 window */
    for (int y = 0; y < g_sim_h; y++) {
        int ny = (y * RGDS_NATIVE_H) / g_sim_h;
        const uint32_t *src_row = g_native_top + (ny * RGDS_NATIVE_W);
        uint32_t *dst_row = g_x11_buf + (y * g_sim_w);
        for (int x = 0; x < g_sim_w; x++) {
            int nx = (x * RGDS_NATIVE_W) / g_sim_w;
            dst_row[x] = src_row[nx];
        }
    }

    /* Render Clamshell Hinge Bar */
    for (int y = 0; y < X11_HINGE_H; y++) {
        uint32_t c = (y == 0 || y == X11_HINGE_H - 1) ? 0x00405575 : 0x00121824;
        uint32_t *dst_row = g_x11_buf + ((g_sim_h + y) * g_sim_w);
        for (int x = 0; x < g_sim_w; x++) {
            dst_row[x] = c;
        }
    }

    /* Render Lower Screen (1024x768) into bottom half of X11 window */
    int bot_y0 = g_sim_h + X11_HINGE_H;
    for (int y = 0; y < g_sim_h; y++) {
        int ny = (y * RGDS_NATIVE_H) / g_sim_h;
        const uint32_t *src_row = g_native_bottom + (ny * RGDS_NATIVE_W);
        uint32_t *dst_row = g_x11_buf + ((bot_y0 + y) * g_sim_w);
        for (int x = 0; x < g_sim_w; x++) {
            int nx = (x * RGDS_NATIVE_W) / g_sim_w;
            dst_row[x] = src_row[nx];
        }
    }

    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0,
              (unsigned int)g_sim_w, (unsigned int)g_total_h);
    XFlush(g_x11_dpy);
}

void rgds_shutdown(void) {
    if (g_x11_img) {
        g_x11_img->data = NULL;
        XDestroyImage(g_x11_img);
        g_x11_img = NULL;
    }
    free(g_x11_buf);
    g_x11_buf = NULL;
    if (g_x11_dpy) {
        if (g_x11_win) XDestroyWindow(g_x11_dpy, g_x11_win);
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
}

#else /* !HOST_X11: Target Anbernic RG DS Plus Hardware (aarch64) */

/*
 * Dynamic SDL2 function pointers loaded via dlopen("libSDL2-2.0.so.0")
 * so the cross-compiler only needs standard glibc headers while linking
 * at runtime against the RG DS Plus system /usr/lib/libSDL2-2.0.so.0.
 */
typedef struct { int x, y, w, h; } rgds_sdl_rect_t;

/*
 * 64-bit SDL_TouchFingerEvent layout verified from /mnt/vendor/bin/dmenu.bin
 * (touch_handl_poll @ 0x48b58..0x49d50):
 *   +0:  uint32_t type       (0x700 = SDL_FINGERDOWN, 0x701 = SDL_FINGERUP, 0x702 = SDL_FINGERMOTION)
 *   +4:  uint32_t timestamp
 *   +8:  int64_t  touchId
 *   +16: int64_t  fingerId   (0..9)
 *   +24: float    x          (0.0f .. 1.0f)
 *   +28: float    y          (0.0f .. 1.0f)
 *   +32: float    dx         (-1.0f .. 1.0f)
 *   +36: float    dy         (-1.0f .. 1.0f)
 *   +40: float    pressure   (0.0f .. 1.0f)
 *   +44: uint32_t windowID
 */
typedef struct {
    uint32_t type;
    uint32_t timestamp;
    int64_t  touchId;
    int64_t  fingerId;
    float    x;
    float    y;
    float    dx;
    float    dy;
    float    pressure;
    uint32_t windowID;
} rgds_sdl_touch_finger_event_t;

typedef union {
    uint32_t                      type;
    rgds_sdl_touch_finger_event_t tfinger;
    uint8_t                       padding[56];
} rgds_sdl_event_t;

#define SDL_INIT_VIDEO               0x00000020u
#define SDL_WINDOW_SHOWN             0x00000004u
#define SDL_WINDOW_BORDERLESS        0x00000010u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_WINDOWPOS_CENTERED_DISPLAY(X) (0x2FFF0000u | (X))
#define SDL_RENDERER_ACCELERATED     0x00000002u
#define SDL_RENDERER_PRESENTVSYNC    0x00000004u
#define SDL_PIXELFORMAT_ARGB8888     0x16362004u
#define SDL_TEXTUREACCESS_STREAMING  1
#define SDL_QUIT_EVENT               0x100u
#define SDL_FINGERDOWN_EVENT         0x700u
#define SDL_FINGERUP_EVENT           0x701u
#define SDL_FINGERMOTION_EVENT       0x702u

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT        0x2f
#endif
#ifndef ABS_MT_TOUCH_MAJOR
#define ABS_MT_TOUCH_MAJOR 0x30
#endif
#ifndef ABS_MT_WIDTH_MAJOR
#define ABS_MT_WIDTH_MAJOR 0x32
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X  0x35
#endif
#ifndef ABS_MT_POSITION_Y
#define ABS_MT_POSITION_Y  0x36
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif
#ifndef ABS_MT_PRESSURE
#define ABS_MT_PRESSURE    0x3a
#endif
#ifndef BTN_TOUCH
#define BTN_TOUCH          0x14a
#endif

static void *g_sdl_lib = NULL;
static int  (*pSDL_Init)(uint32_t) = NULL;
static void (*pSDL_Quit)(void) = NULL;
static int  (*pSDL_SetHint)(const char *, const char *) = NULL;
static int  (*pSDL_GetNumVideoDisplays)(void) = NULL;
static int  (*pSDL_GetDisplayBounds)(int, rgds_sdl_rect_t *) = NULL;
static void*(*pSDL_CreateWindow)(const char *, int, int, int, int, uint32_t) = NULL;
static void*(*pSDL_CreateRenderer)(void *, int, uint32_t) = NULL;
static void*(*pSDL_CreateTexture)(void *, uint32_t, int, int, int) = NULL;
static int  (*pSDL_UpdateTexture)(void *, const rgds_sdl_rect_t *, const void *, int) = NULL;
static int  (*pSDL_RenderClear)(void *) = NULL;
static int  (*pSDL_RenderCopy)(void *, void *, const rgds_sdl_rect_t *, const rgds_sdl_rect_t *) = NULL;
static void (*pSDL_RenderPresent)(void *) = NULL;
static int  (*pSDL_PollEvent)(rgds_sdl_event_t *) = NULL;
static int  (*pSDL_ShowCursor)(int) = NULL;
static void (*pSDL_DestroyTexture)(void *) = NULL;
static void (*pSDL_DestroyRenderer)(void *) = NULL;
static void (*pSDL_DestroyWindow)(void *) = NULL;

static void *g_win[2] = {NULL, NULL};
static void *g_ren[2] = {NULL, NULL};
static void *g_tex[2] = {NULL, NULL};
static int   g_saved_lcdoff = 0;

/* Direct evdev input descriptors (/dev/input/event*) */
#define MAX_EVDEV_FDS 16
static int      g_evdev_fds[MAX_EVDEV_FDS];
static bool     g_evdev_is_touch[MAX_EVDEV_FDS];
static bool     g_evdev_uses_mt[MAX_EVDEV_FDS];
static int      g_evdev_cur_slot[MAX_EVDEV_FDS];
static int      g_evdev_max_x[MAX_EVDEV_FDS];
static int      g_evdev_max_y[MAX_EVDEV_FDS];
static int      g_num_evdev_fds = 0;
static bool     g_has_evdev_touch = false;
static uint32_t g_hw_held = 0;
static uint32_t g_tpctrl_guard_frames = 0;

static int sysfs_read_int(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    char buf[32] = {0};
    int val = fallback;
    if (fgets(buf, sizeof(buf), f)) {
        const char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p >= '0' && *p <= '9') {
            val = 0;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
        }
    }
    fclose(f);
    return val;
}

static void sysfs_write_int(const char *path, int val) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", val);
    fclose(f);
}

static void write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

/*
 * Configure the RG DS Plus kernel touchscreen gate (/sys/class/anbernic_misc/tpctrl)
 * and Weston DRM touch calibration (/tmp/.weston_drm.conf), matching:
 *   - /mnt/vendor/bin/dmenu.bin (set_touch_inpt @ 0x15194)
 *   - /mnt/vendor/deep/drastic64/launch.sh
 *   - Kernel gt9xx driver (0xc4931c -> allen_get_tpctrl @ 0xc27d84):
 *       tpctrl == 0 -> touch ENABLED in kernel gt9xx IRQ handler
 *       tpctrl == 1 -> touch DISABLED in kernel gt9xx IRQ handler
 */
static void rgds_apply_hw_touch_config(bool enabled) {
    if (enabled) {
        write_text_file("/tmp/.weston_drm.conf", "calibration:gt9xx-0:0.5 0 0.5 0 1 0\n");
        sysfs_write_int("/sys/class/anbernic_misc/tpctrl", 0);
    } else {
        sysfs_write_int("/sys/class/anbernic_misc/tpctrl", 1);
    }
}

void rgds_set_touch_enabled(bool enabled) {
    g_touch_enabled = enabled;
    rgds_apply_hw_touch_config(enabled);
    if (!enabled) {
        rgds_reset_touch_slots();
    }
}

void rgds_set_volume(int level_0_to_10) {
    if (level_0_to_10 < 0)  level_0_to_10 = 0;
    if (level_0_to_10 > 10) level_0_to_10 = 10;
    g_volume_level = level_0_to_10;

    /* Persist system volume step in /sys/class/anbernic_misc/openbor_volume (0..10) */
    sysfs_write_int("/sys/class/anbernic_misc/openbor_volume", level_0_to_10);

    /* Apply PulseAudio sink volume via /mnt/vendor/ctrl/setPulseaudio.sh (spk & hp sinks) */
    if (access("/mnt/vendor/ctrl/setPulseaudio.sh", X_OK) == 0) {
        int pct = k_rgds_vol_pct[level_0_to_10];
        int spk_state = sysfs_read_int("/sys/class/anbernic_misc/spk_state", 0);
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "(/mnt/vendor/ctrl/setPulseaudio.sh volume %s %d; "
                 "/mnt/vendor/ctrl/setPulseaudio.sh volume %s %d) >/dev/null 2>&1 &",
                 (spk_state == 1) ? "hp" : "spk", pct,
                 (spk_state == 1) ? "spk" : "hp", pct);
        int r = system(cmd);
        (void)r;
    }
}

static void rgds_step_brightness(int delta) {
    int cur = sysfs_read_int("/sys/class/anbernic_misc/brightness", 5);
    int next = cur + delta;
    if (next < 1) next = 1;
    if (next > 10) next = 10;
    if (next != cur) {
        sysfs_write_int("/sys/class/anbernic_misc/brightness", next);
        if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
            int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1 &");
            (void)r;
        }
    }
}

static void rgds_prepare_hardware_screens(void) {
    /*
     * 1. Stop subscreen.dge if running on Display 1:
     *    When dmenu.bin launches an app from Roms/APPS, it writes
     *    `/mnt/vendor/subscreen/launch.sh apk apps 0 &` to /tmp/.next.
     *    Writing 0 to /sys/class/anbernic_misc/runapp and killing subscreen.dge
     *    frees Weston Display 1 for our Lower Screen window, after which we set
     *    runapp = 1 so system audio/power helpers know an application is active.
     */
    sysfs_write_int("/sys/class/anbernic_misc/runapp", 0);
    int r1 = system("killall -9 subscreen.dge >/dev/null 2>&1");
    (void)r1;
    sysfs_write_int("/sys/class/anbernic_misc/runapp", 1);

    /*
     * 2. Enable kernel touchscreen reporting (tpctrl = 0) and Weston dual-display
     *    touchscreen calibration (/tmp/.weston_drm.conf).
     *    dmenu.bin (set_touch_inpt(0)) and /mnt/vendor/subscreen/launch.sh both write 1
     *    to /sys/class/anbernic_misc/tpctrl when launching external apps, which causes
     *    the Goodix GT911 kernel driver (gt9xx) to drop all touch events unless reset to 0.
     */
    rgds_apply_hw_touch_config(true);

    /*
     * 3. Ensure both 1024x768 LCDs are powered on (lcdoff == 0):
     *    If the user previously toggled single-screen mode, lcdoff may be 1 or 2.
     */
    g_saved_lcdoff = sysfs_read_int("/sys/class/anbernic_misc/lcdoff", 0);
    if (g_saved_lcdoff != 0) {
        sysfs_write_int("/sys/class/anbernic_misc/lcdoff", 0);
        int r2 = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1");
        (void)r2;
    }

    /*
     * 4. Synchronize audio volume from /sys/class/anbernic_misc/openbor_volume (0..10).
     */
    int hw_vol = sysfs_read_int("/sys/class/anbernic_misc/openbor_volume", 10);
    if (hw_vol < 0 || hw_vol > 10) hw_vol = 10;
    rgds_set_volume(hw_vol);

    /*
     * 5. Ensure Weston Wayland & SDL2 touch environment variables match
     *    /etc/profile.d/myenv.sh, Roms/APPS/Clock.sh, and /mnt/vendor/bin/dmenu.bin.
     */
    if (!getenv("XDG_RUNTIME_DIR")) {
        if (access("/var/run/0-runtime-dir/wayland-0", F_OK) == 0) {
            setenv("XDG_RUNTIME_DIR", "/var/run/0-runtime-dir", 0);
        } else {
            setenv("XDG_RUNTIME_DIR", "/var/run", 0);
        }
    }
    if (!getenv("WAYLAND_DISPLAY")) {
        setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    }
    setenv("SDL_VIDEO_DOUBLE_BUFFER", "1", 0);
    setenv("SDL_RENDER_VSYNC", "1", 0);
    setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 0);
    setenv("SDL_MOUSE_TOUCH_EVENTS", "0", 0);
}

static void rgds_restore_hardware_screens(void) {
    write_text_file("/tmp/.weston_drm.conf", "calibration:gt9xx-0:clear\n");
    sysfs_write_int("/sys/class/anbernic_misc/tpctrl", 0);
    sysfs_write_int("/sys/class/anbernic_misc/runapp", 0);
    if (g_saved_lcdoff != 0) {
        sysfs_write_int("/sys/class/anbernic_misc/lcdoff", g_saved_lcdoff);
        int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1");
        (void)r;
    }
}

static void init_evdev_input(void) {
    g_num_evdev_fds = 0;
    g_has_evdev_touch = false;

    for (int i = 0; i < MAX_EVDEV_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            int idx = g_num_evdev_fds++;
            g_evdev_fds[idx]      = fd;
            g_evdev_is_touch[idx] = false;
            g_evdev_uses_mt[idx]  = false;
            g_evdev_cur_slot[idx] = 0;
            g_evdev_max_x[idx]    = RGDS_NATIVE_W;
            g_evdev_max_y[idx]    = RGDS_NATIVE_H;

            char name[128] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
                name[0] = '\0';
            }

            struct input_absinfo abs_x, abs_y, abs_slot;
            memset(&abs_x, 0, sizeof(abs_x));
            memset(&abs_y, 0, sizeof(abs_y));
            memset(&abs_slot, 0, sizeof(abs_slot));

            bool has_mt_x = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0);
            bool has_mt_y = (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0);

            if (has_mt_x && has_mt_y) {
                g_evdev_is_touch[idx] = true;
                g_evdev_uses_mt[idx]  = true;
                g_evdev_max_x[idx]    = abs_x.maximum;
                g_evdev_max_y[idx]    = abs_y.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &abs_slot) == 0) {
                    g_evdev_cur_slot[idx] = abs_slot.value;
                }
                g_has_evdev_touch = true;
            } else if (strstr(name, "goodix") || strstr(name, "gt9xx") ||
                       strstr(name, "touch")  || strstr(name, "ts")) {
                g_evdev_is_touch[idx] = true;
                g_evdev_uses_mt[idx]  = true;
                if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > 0) {
                    g_evdev_max_x[idx] = abs_x.maximum;
                }
                if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                    g_evdev_max_y[idx] = abs_y.maximum;
                }
                g_has_evdev_touch = true;
            }
        }
    }
}

int rgds_init(const char *app_title) {
    init_scale_lut();
    g_touch_enabled = true;
    g_tpctrl_guard_frames = 0;

    int lcd_swap = sysfs_read_int("/sys/class/anbernic_misc/lcdswap", 0);
    const char *env_swap = getenv("SDL2_SWAP_LCD");
    if (env_swap && env_swap[0] == '1') lcd_swap = 1;
    g_lcd_swap = lcd_swap;

    rgds_reset_touch_slots();
    rgds_prepare_hardware_screens();
    init_evdev_input();

    const char *sdl_candidates[] = {
        "libSDL2-2.0.so.0",
        "/usr/lib/libSDL2-2.0.so.0",
        "/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0",
        "/mnt/ports/libs/SDL64/libSDL2-2.0.so.0",
        NULL
    };
    for (int i = 0; sdl_candidates[i]; i++) {
        g_sdl_lib = dlopen(sdl_candidates[i], RTLD_LAZY | RTLD_GLOBAL);
        if (g_sdl_lib) break;
    }
    if (!g_sdl_lib) {
        fprintf(stderr, "[rgds_sdk] ERROR: Could not dlopen libSDL2-2.0.so.0\n");
        return -1;
    }

    pSDL_Init                 = dlsym(g_sdl_lib, "SDL_Init");
    pSDL_Quit                 = dlsym(g_sdl_lib, "SDL_Quit");
    pSDL_SetHint              = dlsym(g_sdl_lib, "SDL_SetHint");
    pSDL_GetNumVideoDisplays  = dlsym(g_sdl_lib, "SDL_GetNumVideoDisplays");
    pSDL_GetDisplayBounds     = dlsym(g_sdl_lib, "SDL_GetDisplayBounds");
    pSDL_CreateWindow         = dlsym(g_sdl_lib, "SDL_CreateWindow");
    pSDL_CreateRenderer       = dlsym(g_sdl_lib, "SDL_CreateRenderer");
    pSDL_CreateTexture        = dlsym(g_sdl_lib, "SDL_CreateTexture");
    pSDL_UpdateTexture        = dlsym(g_sdl_lib, "SDL_UpdateTexture");
    pSDL_RenderClear          = dlsym(g_sdl_lib, "SDL_RenderClear");
    pSDL_RenderCopy           = dlsym(g_sdl_lib, "SDL_RenderCopy");
    pSDL_RenderPresent        = dlsym(g_sdl_lib, "SDL_RenderPresent");
    pSDL_PollEvent            = dlsym(g_sdl_lib, "SDL_PollEvent");
    pSDL_ShowCursor           = dlsym(g_sdl_lib, "SDL_ShowCursor");
    pSDL_DestroyTexture       = dlsym(g_sdl_lib, "SDL_DestroyTexture");
    pSDL_DestroyRenderer      = dlsym(g_sdl_lib, "SDL_DestroyRenderer");
    pSDL_DestroyWindow        = dlsym(g_sdl_lib, "SDL_DestroyWindow");

    /*
     * Set SDL2 touch & Wayland hints before SDL_Init, matching /mnt/vendor/bin/dmenu.bin
     * (0x13434..0x13468) so touch events are delivered cleanly without synthetic mouse clutter.
     */
    if (pSDL_SetHint) {
        pSDL_SetHint("SDL_ACCELEROMETER_AS_JOYSTICK", "0");
        pSDL_SetHint("SDL_TOUCH_MOUSE_EVENTS", "0");
        pSDL_SetHint("SDL_MOUSE_TOUCH_EVENTS", "0");
        pSDL_SetHint("SDL_VIDEO_WAYLAND_MODE_EMULATION", "0");
    }

    if (!pSDL_Init || pSDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[rgds_sdk] ERROR: SDL_Init(SDL_INIT_VIDEO) failed\n");
        return -1;
    }
    if (pSDL_ShowCursor) pSDL_ShowCursor(0);

    int num_displays = pSDL_GetNumVideoDisplays ? pSDL_GetNumVideoDisplays() : 1;
    int top_disp = (num_displays >= 2) ? (lcd_swap ? 1 : 0) : 0;
    int bot_disp = (num_displays >= 2) ? (lcd_swap ? 0 : 1) : 0;
    int disp_idx[2] = { top_disp, bot_disp };

    for (int i = 0; i < 2; i++) {
        if (i == 1 && num_displays < 2) break;

        rgds_sdl_rect_t bounds = {0, 0, RGDS_NATIVE_W, RGDS_NATIVE_H};
        if (pSDL_GetDisplayBounds) {
            pSDL_GetDisplayBounds(disp_idx[i], &bounds);
        }
        int w = (bounds.w > 0) ? bounds.w : RGDS_NATIVE_W;
        int h = (bounds.h > 0) ? bounds.h : RGDS_NATIVE_H;

        char win_title[128];
        snprintf(win_title, sizeof(win_title), "%s - %s",
                 app_title ? app_title : "RGDSPlus", (i == 0) ? "Top" : "Bottom");

        g_win[i] = pSDL_CreateWindow(
            win_title,
            (int)SDL_WINDOWPOS_CENTERED_DISPLAY(disp_idx[i]),
            (int)SDL_WINDOWPOS_CENTERED_DISPLAY(disp_idx[i]),
            w, h,
            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN
        );
        if (!g_win[i]) {
            g_win[i] = pSDL_CreateWindow(
                win_title, bounds.x, bounds.y, w, h,
                SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN
            );
        }
        if (g_win[i]) {
            g_ren[i] = pSDL_CreateRenderer(g_win[i], -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!g_ren[i]) g_ren[i] = pSDL_CreateRenderer(g_win[i], -1, 0);
            if (g_ren[i]) {
                g_tex[i] = pSDL_CreateTexture(g_ren[i], SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, RGDS_NATIVE_W, RGDS_NATIVE_H);
            }
        }
    }

    /* Re-assert tpctrl = 0 after window creation in case background subscreen/launch.sh ran */
    rgds_apply_hw_touch_config(true);

    snprintf(g_backend_desc, sizeof(g_backend_desc),
             "RG DS Plus Weston/SDL2 (%d displays, 1024x768 x 2, swap=%d, touch=%s)",
             num_displays, lcd_swap, g_has_evdev_touch ? "goodix-ts" : "sdl2");
    return 0;
}

bool rgds_poll_input(rgds_input_state_t *out_input) {
    uint32_t pressed_latch  = 0;
    uint32_t released_latch = 0;

    /*
     * Guard against asynchronous `/mnt/vendor/subscreen/launch.sh apk apps 0 &` writing
     * `echo 1 > /sys/class/anbernic_misc/tpctrl` shortly after process launch.
     */
    if (g_touch_enabled && g_tpctrl_guard_frames < 180) {
        if ((g_tpctrl_guard_frames % 15) == 0) {
            if (sysfs_read_int("/sys/class/anbernic_misc/tpctrl", 0) != 0) {
                rgds_apply_hw_touch_config(true);
            }
        }
        g_tpctrl_guard_frames++;
    }

    /*
     * 1. Drain SDL2 window events (and process SDL_FINGERDOWN/UP/MOTION if raw
     *    goodix-ts evdev is not directly open, matching dmenu.bin touch_handl_poll @ 0x48b58).
     */
    if (pSDL_PollEvent) {
        rgds_sdl_event_t ev;
        while (pSDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT_EVENT) {
                g_quit_requested = true;
            } else if (!g_has_evdev_touch && g_touch_enabled &&
                       (ev.type == SDL_FINGERDOWN_EVENT ||
                        ev.type == SDL_FINGERUP_EVENT ||
                        ev.type == SDL_FINGERMOTION_EVENT)) {
                int slot = (int)ev.tfinger.fingerId;
                if (slot >= 0 && slot < RGDS_MAX_TOUCH_POINTS) {
                    int nx = clamp_int((int)(ev.tfinger.x * (float)RGDS_NATIVE_W), 0, RGDS_NATIVE_W - 1);
                    int ny = clamp_int((int)(ev.tfinger.y * (float)RGDS_NATIVE_H), 0, RGDS_NATIVE_H - 1);
                    int pr = clamp_int((int)(ev.tfinger.pressure * 255.0f), 1, 255);
                    g_touch_slots[slot].raw_x  = nx;
                    g_touch_slots[slot].raw_y  = ny;
                    g_touch_slots[slot].screen = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
                    if (ev.type == SDL_FINGERDOWN_EVENT) {
                        if (!g_touch_slots[slot].down) {
                            g_touch_slots[slot].pressed_latch = true;
                        }
                        g_touch_slots[slot].down     = true;
                        g_touch_slots[slot].pressure = pr;
                    } else if (ev.type == SDL_FINGERMOTION_EVENT) {
                        g_touch_slots[slot].down     = true;
                        g_touch_slots[slot].pressure = pr;
                    } else if (ev.type == SDL_FINGERUP_EVENT) {
                        if (g_touch_slots[slot].down || g_touch_slots[slot].pressed_latch) {
                            g_touch_slots[slot].released_latch = true;
                        }
                        g_touch_slots[slot].down     = false;
                        g_touch_slots[slot].pressure = 0;
                    }
                }
            }
        }
    }

    /*
     * 2. Read raw Linux input events from /dev/input/event*:
     *    - "ANBERNIC-rk3568-keys": buttons (EV_KEY) and D-Pad (EV_ABS ABS_HAT0X/Y)
     *    - "goodix-ts" (Goodix GT911 @ I2C5 0x14): 10-slot Multi-Touch Protocol B
     *      (ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X/Y, ABS_MT_TOUCH_MAJOR)
     */
    for (int i = 0; i < g_num_evdev_fds; i++) {
        struct input_event ev;
        while (read(g_evdev_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                if (ev.code == BTN_TOUCH && g_evdev_is_touch[i] && !g_evdev_uses_mt[i] && g_touch_enabled) {
                    if (ev.value == 1) {
                        if (!g_touch_slots[0].down) g_touch_slots[0].pressed_latch = true;
                        g_touch_slots[0].down     = true;
                        g_touch_slots[0].pressure = 128;
                        g_touch_slots[0].screen   = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
                    } else if (ev.value == 0) {
                        if (g_touch_slots[0].down || g_touch_slots[0].pressed_latch) {
                            g_touch_slots[0].released_latch = true;
                        }
                        g_touch_slots[0].down     = false;
                        g_touch_slots[0].pressure = 0;
                    }
                    continue;
                }

                uint32_t mask = 0;
                switch (ev.code) {
                    case 304: case KEY_SPACE:     mask = RGDS_BTN_A;        break;
                    case 305: case KEY_BACKSPACE: mask = RGDS_BTN_B;        break;
                    case 306:                     mask = RGDS_BTN_Y;        break;
                    case 307:                     mask = RGDS_BTN_X;        break;
                    case 308:                     mask = RGDS_BTN_L1;       break;
                    case 309:                     mask = RGDS_BTN_R1;       break;
                    case 310:                     mask = RGDS_BTN_SELECT;   break;
                    case 311: case KEY_ENTER:     mask = RGDS_BTN_START;    break;
                    case 312: case KEY_ESC:       mask = RGDS_BTN_MENU;     break;
                    case 314:                     mask = RGDS_BTN_L2;       break;
                    case 315:                     mask = RGDS_BTN_R2;       break;
                    case 317:                     mask = RGDS_BTN_L3;       break;
                    case 318:                     mask = RGDS_BTN_R3;       break;
                    case KEY_VOLUMEUP:            mask = RGDS_BTN_VOL_UP;   break;
                    case KEY_VOLUMEDOWN:          mask = RGDS_BTN_VOL_DOWN; break;
                    case KEY_UP:                  mask = RGDS_BTN_UP;       break;
                    case KEY_DOWN:                mask = RGDS_BTN_DOWN;     break;
                    case KEY_LEFT:                mask = RGDS_BTN_LEFT;     break;
                    case KEY_RIGHT:               mask = RGDS_BTN_RIGHT;    break;
                    default: break;
                }
                if (mask) {
                    if (ev.value == 1) {
                        pressed_latch |= (mask & ~g_hw_held);
                        g_hw_held |= mask;
                    } else if (ev.value == 0) {
                        released_latch |= (mask & g_hw_held);
                        g_hw_held &= ~mask;
                    }
                }
            } else if (ev.type == EV_ABS) {
                /* D-Pad on ANBERNIC-rk3568-keys reports via ABS_HAT0X (16) and ABS_HAT0Y (17) */
                if (ev.code == ABS_HAT0X) {
                    uint32_t old_lr = g_hw_held & (RGDS_BTN_LEFT | RGDS_BTN_RIGHT);
                    uint32_t new_lr = (ev.value < 0) ? RGDS_BTN_LEFT : (ev.value > 0) ? RGDS_BTN_RIGHT : 0;
                    pressed_latch  |= (new_lr & ~old_lr);
                    released_latch |= (old_lr & ~new_lr);
                    g_hw_held = (g_hw_held & ~(RGDS_BTN_LEFT | RGDS_BTN_RIGHT)) | new_lr;
                } else if (ev.code == ABS_HAT0Y) {
                    uint32_t old_ud = g_hw_held & (RGDS_BTN_UP | RGDS_BTN_DOWN);
                    uint32_t new_ud = (ev.value < 0) ? RGDS_BTN_UP : (ev.value > 0) ? RGDS_BTN_DOWN : 0;
                    pressed_latch  |= (new_ud & ~old_ud);
                    released_latch |= (old_ud & ~new_ud);
                    g_hw_held = (g_hw_held & ~(RGDS_BTN_UP | RGDS_BTN_DOWN)) | new_ud;
                } else if (g_evdev_is_touch[i] && g_touch_enabled) {
                    int slot = g_evdev_cur_slot[i];
                    int max_x = (g_evdev_max_x[i] > 0) ? g_evdev_max_x[i] : RGDS_NATIVE_W;
                    int max_y = (g_evdev_max_y[i] > 0) ? g_evdev_max_y[i] : RGDS_NATIVE_H;

                    if (ev.code == ABS_MT_SLOT) {
                        g_evdev_uses_mt[i]  = true;
                        g_evdev_cur_slot[i] = ev.value;
                    } else if (ev.code == ABS_MT_TRACKING_ID) {
                        g_evdev_uses_mt[i] = true;
                        if (slot >= 0 && slot < RGDS_MAX_TOUCH_POINTS) {
                            if (ev.value >= 0) {
                                if (!g_touch_slots[slot].down) {
                                    g_touch_slots[slot].pressed_latch = true;
                                }
                                g_touch_slots[slot].down   = true;
                                g_touch_slots[slot].screen = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
                                if (g_touch_slots[slot].pressure <= 0) {
                                    g_touch_slots[slot].pressure = 128;
                                }
                            } else {
                                if (g_touch_slots[slot].down || g_touch_slots[slot].pressed_latch) {
                                    g_touch_slots[slot].released_latch = true;
                                }
                                g_touch_slots[slot].down     = false;
                                g_touch_slots[slot].pressure = 0;
                            }
                        }
                    } else if (ev.code == ABS_MT_POSITION_X) {
                        g_evdev_uses_mt[i] = true;
                        if (slot >= 0 && slot < RGDS_MAX_TOUCH_POINTS) {
                            int nx = (max_x == RGDS_NATIVE_W) ? ev.value : (ev.value * RGDS_NATIVE_W) / max_x;
                            g_touch_slots[slot].raw_x  = clamp_int(nx, 0, RGDS_NATIVE_W - 1);
                            g_touch_slots[slot].screen = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
                        }
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        g_evdev_uses_mt[i] = true;
                        if (slot >= 0 && slot < RGDS_MAX_TOUCH_POINTS) {
                            int ny = (max_y == RGDS_NATIVE_H) ? ev.value : (ev.value * RGDS_NATIVE_H) / max_y;
                            g_touch_slots[slot].raw_y  = clamp_int(ny, 0, RGDS_NATIVE_H - 1);
                            g_touch_slots[slot].screen = g_lcd_swap ? RGDS_SCREEN_TOP : RGDS_SCREEN_BOTTOM;
                        }
                    } else if (ev.code == ABS_MT_TOUCH_MAJOR ||
                               ev.code == ABS_MT_WIDTH_MAJOR ||
                               ev.code == ABS_MT_PRESSURE) {
                        if (slot >= 0 && slot < RGDS_MAX_TOUCH_POINTS) {
                            g_touch_slots[slot].pressure = clamp_int(ev.value, 1, 255);
                        }
                    } else if (ev.code == ABS_X && !g_evdev_uses_mt[i]) {
                        int nx = (max_x == RGDS_NATIVE_W) ? ev.value : (ev.value * RGDS_NATIVE_W) / max_x;
                        g_touch_slots[0].raw_x = clamp_int(nx, 0, RGDS_NATIVE_W - 1);
                    } else if (ev.code == ABS_Y && !g_evdev_uses_mt[i]) {
                        int ny = (max_y == RGDS_NATIVE_H) ? ev.value : (ev.value * RGDS_NATIVE_H) / max_y;
                        g_touch_slots[0].raw_y = clamp_int(ny, 0, RGDS_NATIVE_H - 1);
                    }
                }
            }
        }
    }

    /* Handle physical Volume Up (115) / Volume Down (114) with hold auto-repeat (or brightness with SELECT/START) */
    uint32_t newly_pressed = pressed_latch | (g_hw_held & ~g_prev_held);
    bool mod_held = (g_hw_held & (RGDS_BTN_SELECT | RGDS_BTN_START)) != 0;
    if (newly_pressed & RGDS_BTN_VOL_UP) {
        if (mod_held) {
            rgds_step_brightness(+1);
        } else {
            rgds_set_volume(g_volume_level + 1);
            g_vol_repeat_dir = 1;
            g_vol_next_repeat_ms = rgds_ticks_ms() + RGDS_VOL_REPEAT_DELAY_MS;
        }
    } else if (newly_pressed & RGDS_BTN_VOL_DOWN) {
        if (mod_held) {
            rgds_step_brightness(-1);
        } else {
            rgds_set_volume(g_volume_level - 1);
            g_vol_repeat_dir = -1;
            g_vol_next_repeat_ms = rgds_ticks_ms() + RGDS_VOL_REPEAT_DELAY_MS;
        }
    }
    if (!(g_hw_held & (RGDS_BTN_VOL_UP | RGDS_BTN_VOL_DOWN)) || mod_held) {
        g_vol_repeat_dir = 0;
    } else if (g_vol_repeat_dir != 0) {
        uint64_t now = rgds_ticks_ms();
        if (now >= g_vol_next_repeat_ms) {
            g_vol_next_repeat_ms = now + RGDS_VOL_REPEAT_INTERVAL_MS;
            rgds_set_volume(g_volume_level + g_vol_repeat_dir);
        }
    }

    if (((g_hw_held | pressed_latch) & RGDS_BTN_MENU) ||
        (((g_hw_held | pressed_latch) & (RGDS_BTN_START | RGDS_BTN_SELECT)) == (RGDS_BTN_START | RGDS_BTN_SELECT))) {
        g_quit_requested = true;
    }

    rgds_finalize_touch_frame(out_input ? &out_input->touch : NULL);

    if (out_input) {
        out_input->held     = g_hw_held;
        out_input->pressed  = newly_pressed;
        out_input->released = released_latch | (~g_hw_held & g_prev_held);
        out_input->quit_req = g_quit_requested;
    }
    g_prev_held = g_hw_held;
    return !g_quit_requested;
}

void rgds_present(void) {
    if (!g_direct_native_mode) {
        upscale_canvas_to_native(g_canvas_top, g_native_top);
        upscale_canvas_to_native(g_canvas_bottom, g_native_bottom);
    }

    const uint32_t *bufs[2] = { g_native_top, g_native_bottom };
    for (int i = 0; i < 2; i++) {
        if (g_ren[i] && g_tex[i]) {
            pSDL_UpdateTexture(g_tex[i], NULL, bufs[i], RGDS_NATIVE_W * (int)sizeof(uint32_t));
            pSDL_RenderClear(g_ren[i]);
            pSDL_RenderCopy(g_ren[i], g_tex[i], NULL, NULL);
            pSDL_RenderPresent(g_ren[i]);
        }
    }
}

void rgds_shutdown(void) {
    for (int i = 0; i < 2; i++) {
        if (g_tex[i] && pSDL_DestroyTexture)  pSDL_DestroyTexture(g_tex[i]);
        if (g_ren[i] && pSDL_DestroyRenderer) pSDL_DestroyRenderer(g_ren[i]);
        if (g_win[i] && pSDL_DestroyWindow)   pSDL_DestroyWindow(g_win[i]);
        g_tex[i] = g_ren[i] = g_win[i] = NULL;
    }
    if (pSDL_Quit) pSDL_Quit();
    if (g_sdl_lib) {
        dlclose(g_sdl_lib);
        g_sdl_lib = NULL;
    }
    for (int i = 0; i < g_num_evdev_fds; i++) {
        close(g_evdev_fds[i]);
    }
    g_num_evdev_fds = 0;
    rgds_restore_hardware_screens();
}

#endif /* HOST_X11 */
