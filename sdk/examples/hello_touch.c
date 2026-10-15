/*
 * hello_touch.c - Starter Template for Touchscreen & 10-Point Multi-Touch Apps
 *                 on the Anbernic RG DS Plus (Dual 1024x768 IPS Displays)
 *
 * ============================================================================
 * OVERVIEW FOR DEVELOPERS
 * ============================================================================
 * This example is designed as a clean, well-documented starting point for
 * building touch-driven utilities and games with the RG DS Plus C SDK.
 *
 * It demonstrates:
 *   1. Single-Touch UI Controls (`input.touch`):
 *      - How to define clickable on-screen buttons (color swatches, brush size
 *        toggle, and a CLEAR button) on the Lower Touchscreen and test for taps
 *        using `input.touch.just_pressed` and `(input.touch.x, input.touch.y)`.
 *
 *   2. 10-Point Multi-Touch Input (`input.touch.points[0..9]`):
 *      - How to iterate over all 10 hardware finger slots reported by the
 *        Goodix GT911 ("goodix-ts") capacitive controller so multiple fingers
 *        can draw on the canvas at the same time.
 *      - How to use per-finger frame deltas (`pt->dx`, `pt->dy`) to draw smooth,
 *        continuous strokes even when a finger moves quickly.
 *      - How to detect a 2-finger gesture (pinch/spread distance & midpoint)
 *        when two or more fingers touch the screen simultaneously.
 *
 *   3. Dual-Screen Layout (`RGDS_SCREEN_TOP` + `RGDS_SCREEN_BOTTOM`):
 *      - Lower Screen (`RGDS_SCREEN_BOTTOM`): Interactive touch toolbar,
 *        multi-finger drawing canvas, live finger crosshairs, and 2-finger
 *        chord visualizer.
 *      - Upper Screen (`RGDS_SCREEN_TOP`): "HELLO TOUCH" header, live 10-slot
 *        multi-touch telemetry table (logical 320x240 & native 1024x768 coords),
 *        and a 2-finger distance gauge.
 *
 * Controls:
 *   - Touch / Mouse Drag (Lower Screen): Tap toolbar buttons or draw on canvas
 *   - A Button: Cycle active brush color
 *   - B Button: Cycle brush size (Small / Medium / Large)
 *   - X / Y Button: Clear the drawing canvas
 *   - MENU ('M') or START + SELECT (or ESC on PC): Exit application
 * ============================================================================
 */

#include "rgds_sdk.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * 1. CONSTANTS & SIMPLE UI LAYOUT DEFINITIONS (320x240 Logical Canvas Space)
 * ============================================================================ */

/* Interactive drawing area bounds on the Lower Screen (RGDS_SCREEN_BOTTOM) */
#define PAD_X       10
#define PAD_Y       62
#define PAD_W      300
#define PAD_H      152

/* Number of selectable color swatches in the top toolbar */
#define NUM_COLORS   6

/* Simple rectangular UI button definition */
typedef struct {
    int         x;
    int         y;
    int         w;
    int         h;
    const char *label;
    uint32_t    color;
} ui_button_t;

/* Palette of brush colors selectable via touch toolbar or 'A' button */
static const ui_button_t k_color_buttons[NUM_COLORS] = {
    {  10, 34, 30, 22, "CYN", RGDS_COLOR_CYAN   },
    {  44, 34, 30, 22, "YEL", RGDS_COLOR_YELLOW },
    {  78, 34, 30, 22, "GRN", RGDS_COLOR_GREEN  },
    { 112, 34, 30, 22, "ORG", RGDS_COLOR_ORANGE },
    { 146, 34, 30, 22, "RED", RGDS_COLOR_RED    },
    { 180, 34, 30, 22, "WHT", RGDS_COLOR_WHITE  }
};

/* Brush size toggle button & Clear canvas button */
static const ui_button_t k_size_button  = { 216, 34, 44, 22, "SZ:2",  RGDS_COLOR_SLATE };
static const ui_button_t k_clear_button = { 264, 34, 46, 22, "CLEAR", RGDS_RGB(140, 40, 55) };

/* Off-screen pixel buffer storing the user's persistent drawing on the pad */
static uint32_t g_pad_pixels[PAD_W * PAD_H];

/* ============================================================================
 * 2. RUDIMENTARY GRAPHICS & HIT-TESTING HELPERS
 * ============================================================================ */

/*
 * Check whether a touch coordinate `(tx, ty)` lies inside a rectangle.
 */
static bool point_in_rect(int tx, int ty, int rx, int ry, int rw, int rh) {
    return (tx >= rx && tx < (rx + rw) && ty >= ry && ty < (ry + rh));
}

/*
 * Clear the persistent drawing pad buffer to a dark background grid.
 */
static void pad_clear(void) {
    for (int y = 0; y < PAD_H; y++) {
        for (int x = 0; x < PAD_W; x++) {
            bool grid = ((x % 20) == 0) || ((y % 20) == 0);
            g_pad_pixels[y * PAD_W + x] = grid ? RGDS_RGB(24, 34, 52) : RGDS_RGB(16, 22, 36);
        }
    }
}

/*
 * Stamp a square brush dab of radius `radius` at `(px, py)` in pad-local coords.
 */
static void pad_stamp_brush(int px, int py, int radius, uint32_t color) {
    for (int dy = -radius; dy <= radius; dy++) {
        int y = py + dy;
        if (y < 0 || y >= PAD_H) continue;
        for (int dx = -radius; dx <= radius; dx++) {
            int x = px + dx;
            if (x < 0 || x >= PAD_W) continue;
            g_pad_pixels[y * PAD_W + x] = color;
        }
    }
}

/*
 * Draw a continuous line on the persistent pad using Bresenham's algorithm.
 * Connecting `(x0, y0)` to `(x1, y1)` ensures fast finger movements produce
 * solid strokes rather than disconnected dots.
 */
static void pad_draw_stroke(int x0, int y0, int x1, int y1, int radius, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        pad_stamp_brush(x0, y0, radius, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/*
 * Draw a direct line on an SDK screen (`RGDS_SCREEN_TOP` or `RGDS_SCREEN_BOTTOM`)
 * for transient UI overlays such as the 2-finger pinch/spread chord line.
 */
static void screen_draw_line(rgds_screen_t scr, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        rgds_draw_pixel(scr, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/*
 * Blit the persistent drawing pad onto the Lower Screen canvas.
 */
static void pad_render_to_screen(rgds_screen_t scr) {
    uint32_t *canvas = rgds_get_canvas_buffer(scr);
    for (int y = 0; y < PAD_H; y++) {
        memcpy(&canvas[(PAD_Y + y) * RGDS_CANVAS_W + PAD_X],
               &g_pad_pixels[y * PAD_W],
               PAD_W * sizeof(uint32_t));
    }
    rgds_draw_rect(scr, PAD_X - 1, PAD_Y - 1, PAD_W + 2, PAD_H + 2, 1, RGDS_COLOR_BORDER);
}

/*
 * Draw a live crosshair reticle around an active finger slot.
 */
static void draw_finger_reticle(rgds_screen_t scr, const rgds_touch_point_t *pt, uint32_t color) {
    int x = pt->x;
    int y = pt->y;
    int box_r = 4 + (pt->pressure / 64); /* Expand reticle slightly with finger pressure */

    rgds_draw_rect(scr, x - box_r, y - box_r, box_r * 2 + 1, box_r * 2 + 1, 1, RGDS_COLOR_WHITE);
    rgds_fill_rect(scr, x - box_r - 4, y, box_r * 2 + 9, 1, color);
    rgds_fill_rect(scr, x, y - box_r - 4, 1, box_r * 2 + 9, color);

    char tag[12];
    snprintf(tag, sizeof(tag), "F%d", pt->id);
    int tx = (x + box_r + 16 < RGDS_CANVAS_W) ? (x + box_r + 3) : (x - box_r - 14);
    int ty = (y - box_r - 9 >= 4) ? (y - box_r - 9) : (y + box_r + 3);
    rgds_draw_text(scr, tx, ty, tag, 1, color);
}

/* ============================================================================
 * 3. MAIN APPLICATION LOOP
 * ============================================================================ */

int main(void) {
    if (rgds_init("Hello Touch - RG DS Plus") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }

    /* Distinct colors for visualizing each of the 10 multi-touch finger slots */
    static const uint32_t k_finger_colors[RGDS_MAX_TOUCH_POINTS] = {
        RGDS_COLOR_CYAN,
        RGDS_COLOR_YELLOW,
        RGDS_COLOR_GREEN,
        RGDS_COLOR_ORANGE,
        RGDS_COLOR_RED,
        RGDS_RGB(210, 120, 255),
        RGDS_RGB(80,  255, 210),
        RGDS_RGB(255, 130, 190),
        RGDS_RGB(190, 245, 90),
        RGDS_COLOR_WHITE
    };

    int selected_color_idx = 0;
    int brush_radius       = 2; /* 1 = Small, 2 = Medium, 3 = Large */
    uint32_t stroke_count  = 0;

    pad_clear();

    rgds_input_state_t input;
    while (rgds_poll_input(&input)) {
        /* --------------------------------------------------------------------
         * A. Physical Button Shortcuts (for convenience alongside touch)
         * -------------------------------------------------------------------- */
        if (input.pressed & RGDS_BTN_A) {
            selected_color_idx = (selected_color_idx + 1) % NUM_COLORS;
        }
        if (input.pressed & RGDS_BTN_B) {
            brush_radius = (brush_radius % 3) + 1;
        }
        if (input.pressed & (RGDS_BTN_X | RGDS_BTN_Y)) {
            pad_clear();
        }

        /* --------------------------------------------------------------------
         * B. Single-Touch Toolbar Interaction (`input.touch.just_pressed`)
         *    When a finger taps inside the toolbar row (y: 34..56), check
         *    which on-screen UI button was hit.
         * -------------------------------------------------------------------- */
        if (input.touch.just_pressed) {
            int tx = input.touch.x;
            int ty = input.touch.y;

            /* 1. Check color swatch buttons */
            for (int c = 0; c < NUM_COLORS; c++) {
                if (point_in_rect(tx, ty,
                                  k_color_buttons[c].x, k_color_buttons[c].y,
                                  k_color_buttons[c].w, k_color_buttons[c].h)) {
                    selected_color_idx = c;
                }
            }
            /* 2. Check brush size toggle button */
            if (point_in_rect(tx, ty,
                              k_size_button.x, k_size_button.y,
                              k_size_button.w, k_size_button.h)) {
                brush_radius = (brush_radius % 3) + 1;
            }
            /* 3. Check CLEAR button */
            if (point_in_rect(tx, ty,
                              k_clear_button.x, k_clear_button.y,
                              k_clear_button.w, k_clear_button.h)) {
                pad_clear();
            }
        }

        /* --------------------------------------------------------------------
         * C. 10-Point Multi-Touch Canvas Painting (`input.touch.points[0..9]`)
         *    Every active finger inside the pad draws simultaneously!
         *    - Slot 0 uses the user-selected toolbar color.
         *    - Additional simultaneous fingers (Slots 1..9) use their distinct
         *      slot color so multi-finger drawing is immediately visible.
         * -------------------------------------------------------------------- */
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            const rgds_touch_point_t *pt = &input.touch.points[s];
            if (!pt->active) continue;

            if (pt->just_pressed &&
                point_in_rect(pt->x, pt->y, PAD_X, PAD_Y, PAD_W, PAD_H)) {
                stroke_count++;
            }

            if (point_in_rect(pt->x, pt->y, PAD_X, PAD_Y, PAD_W, PAD_H)) {
                int cur_px  = pt->x - PAD_X;
                int cur_py  = pt->y - PAD_Y;
                int prev_px = (pt->x - pt->dx) - PAD_X;
                int prev_py = (pt->y - pt->dy) - PAD_Y;

                uint32_t ink_color = (s == 0)
                    ? k_color_buttons[selected_color_idx].color
                    : k_finger_colors[s % RGDS_MAX_TOUCH_POINTS];

                pad_draw_stroke(prev_px, prev_py, cur_px, cur_py, brush_radius, ink_color);
            }
        }

        /* ====================================================================
         * 4. RENDER UPPER SCREEN (RGDS_SCREEN_TOP): Multi-Touch Telemetry UI
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_TOP, RGDS_COLOR_NAVY);
        rgds_draw_rect(RGDS_SCREEN_TOP, 4, 4, RGDS_CANVAS_W - 8, RGDS_CANVAS_H - 8, 2, RGDS_COLOR_CYAN);

        /* Header */
        rgds_fill_rect(RGDS_SCREEN_TOP, 10, 10, RGDS_CANVAS_W - 20, 22, RGDS_COLOR_SLATE);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 14, "HELLO TOUCH - MULTI-TOUCH INSPECTOR", 1, RGDS_COLOR_CYAN);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 23, "UPPER SCREEN (DISPLAY 0 - 1024X768)", 1, RGDS_COLOR_GRAY);

        /* Summary status bar */
        char summary_buf[64];
        snprintf(summary_buf, sizeof(summary_buf),
                 "ACTIVE FINGERS: %d / %d   STROKES: %u",
                 input.touch.count, RGDS_MAX_TOUCH_POINTS, stroke_count);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 38, summary_buf, 1,
                                input.touch.touching ? RGDS_COLOR_YELLOW : RGDS_COLOR_WHITE);

        /* 10-Slot Multi-Touch Table (2 columns x 5 rows) */
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            const rgds_touch_point_t *pt = &input.touch.points[s];
            int col = s / 5;
            int row = s % 5;
            int bx  = 12 + col * 150;
            int by  = 52 + row * 24;

            uint32_t bg     = pt->active ? RGDS_RGB(26, 48, 78) : RGDS_RGB(18, 26, 42);
            uint32_t border = pt->active ? k_finger_colors[s]   : RGDS_COLOR_BORDER;
            rgds_fill_rect(RGDS_SCREEN_TOP, bx, by, 146, 20, bg);
            rgds_draw_rect(RGDS_SCREEN_TOP, bx, by, 146, 20, 1, border);

            char slot_line[40];
            if (pt->active) {
                snprintf(slot_line, sizeof(slot_line), "F%d:%3d,%3d (%4d,%3d)",
                         s, pt->x, pt->y, pt->native_x, pt->native_y);
                rgds_draw_text(RGDS_SCREEN_TOP, bx + 4, by + 6, slot_line, 1, k_finger_colors[s]);
            } else {
                snprintf(slot_line, sizeof(slot_line), "F%d: IDLE   LAST %3d,%3d",
                         s, pt->x, pt->y);
                rgds_draw_text(RGDS_SCREEN_TOP, bx + 4, by + 6, slot_line, 1, RGDS_COLOR_GRAY);
            }
        }

        /* Two-Finger Pinch / Spread Distance Gauge (active when >= 2 fingers touch) */
        rgds_fill_rect(RGDS_SCREEN_TOP, 12, 176, 296, 36, RGDS_RGB(20, 30, 50));
        rgds_draw_rect(RGDS_SCREEN_TOP, 12, 176, 296, 36, 1, RGDS_COLOR_BORDER);

        int f0 = -1, f1 = -1;
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            if (input.touch.points[s].active) {
                if (f0 < 0)      f0 = s;
                else if (f1 < 0) f1 = s;
            }
        }
        if (f0 >= 0 && f1 >= 0) {
            const rgds_touch_point_t *p0 = &input.touch.points[f0];
            const rgds_touch_point_t *p1 = &input.touch.points[f1];
            int ddx = p1->x - p0->x;
            int ddy = p1->y - p0->y;
            int dist_px = (int)sqrtf((float)(ddx * ddx + ddy * ddy));

            char pinch_txt[64];
            snprintf(pinch_txt, sizeof(pinch_txt),
                     "2-FINGER CHORD (F%d..F%d): DIST = %d PX", f0, f1, dist_px);
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 181, pinch_txt, 1, RGDS_COLOR_GREEN);

            /* Visual bar proportional to finger separation (max ~360px diagonal) */
            int bar_w = (dist_px * 276) / 360;
            if (bar_w > 276) bar_w = 276;
            rgds_fill_rect(RGDS_SCREEN_TOP, 22, 195, 276, 10, RGDS_RGB(12, 18, 30));
            rgds_fill_rect(RGDS_SCREEN_TOP, 22, 195, bar_w, 10, RGDS_COLOR_GREEN);
            rgds_draw_rect(RGDS_SCREEN_TOP, 22, 195, 276, 10, 1, RGDS_COLOR_WHITE);
        } else {
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 184,
                                    "PLACE 2+ FINGERS ON LOWER SCREEN", 1, RGDS_COLOR_CYAN);
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 196,
                                    "TO MEASURE PINCH / SPREAD DISTANCE", 1, RGDS_COLOR_GRAY);
        }

        rgds_draw_text_centered(RGDS_SCREEN_TOP, 220,
                                "A:COLOR  B:SIZE  X/Y:CLEAR  MENU:EXIT", 1, RGDS_COLOR_GRAY);

        /* ====================================================================
         * 5. RENDER LOWER SCREEN (RGDS_SCREEN_BOTTOM): Touch Toolbar & Canvas
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(14, 18, 28));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 4, 4, RGDS_CANVAS_W - 8, RGDS_CANVAS_H - 8, 2, RGDS_COLOR_GREEN);

        /* Header */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 10, RGDS_CANVAS_W - 20, 20, RGDS_COLOR_SLATE);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 16,
                                "LOWER TOUCHSCREEN: TAP UI OR DRAW", 1, RGDS_COLOR_GREEN);

        /* Render Color Swatch Buttons */
        for (int c = 0; c < NUM_COLORS; c++) {
            const ui_button_t *b = &k_color_buttons[c];
            bool selected = (c == selected_color_idx);
            rgds_fill_rect(RGDS_SCREEN_BOTTOM, b->x, b->y, b->w, b->h, b->color);
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, b->x, b->y, b->w, b->h,
                           selected ? 2 : 1,
                           selected ? RGDS_COLOR_WHITE : RGDS_COLOR_BLACK);
            int tw = rgds_text_width(b->label, 1);
            rgds_draw_text(RGDS_SCREEN_BOTTOM,
                           b->x + (b->w - tw) / 2, b->y + 8,
                           b->label, 1, RGDS_COLOR_BLACK);
        }

        /* Render Brush Size Button */
        char sz_label[16];
        snprintf(sz_label, sizeof(sz_label), "SZ:%d", brush_radius);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, k_size_button.x, k_size_button.y,
                       k_size_button.w, k_size_button.h, k_size_button.color);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, k_size_button.x, k_size_button.y,
                       k_size_button.w, k_size_button.h, 1, RGDS_COLOR_CYAN);
        rgds_draw_text(RGDS_SCREEN_BOTTOM,
                       k_size_button.x + 10, k_size_button.y + 8,
                       sz_label, 1, RGDS_COLOR_WHITE);

        /* Render CLEAR Button */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, k_clear_button.x, k_clear_button.y,
                       k_clear_button.w, k_clear_button.h, k_clear_button.color);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, k_clear_button.x, k_clear_button.y,
                       k_clear_button.w, k_clear_button.h, 1, RGDS_COLOR_WHITE);
        rgds_draw_text(RGDS_SCREEN_BOTTOM,
                       k_clear_button.x + 8, k_clear_button.y + 8,
                       k_clear_button.label, 1, RGDS_COLOR_WHITE);

        /* Blit the persistent multi-touch drawing pad */
        pad_render_to_screen(RGDS_SCREEN_BOTTOM);

        /* If 2+ fingers are touching, draw a live connecting chord & midpoint marker */
        if (f0 >= 0 && f1 >= 0) {
            const rgds_touch_point_t *p0 = &input.touch.points[f0];
            const rgds_touch_point_t *p1 = &input.touch.points[f1];
            if (p0->screen == RGDS_SCREEN_BOTTOM && p1->screen == RGDS_SCREEN_BOTTOM) {
                screen_draw_line(RGDS_SCREEN_BOTTOM, p0->x, p0->y, p1->x, p1->y, RGDS_COLOR_WHITE);
                int mx = (p0->x + p1->x) / 2;
                int my = (p0->y + p1->y) / 2;
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, mx - 2, my - 2, 5, 5, RGDS_COLOR_YELLOW);
            }
        }

        /* Draw live reticles for all active touch points (0..9) */
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            const rgds_touch_point_t *pt = &input.touch.points[s];
            if (pt->active) {
                draw_finger_reticle(pt->screen, pt, k_finger_colors[s]);
            }
        }

        /* Footer status on Lower Screen */
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 220,
                                "DRAW WITH 1..10 FINGERS (CLICK+DRAG ON PC)", 1, RGDS_COLOR_GRAY);

        /* Present both 1024x768 screens and pace at ~60 FPS */
        rgds_present();
        rgds_delay_ms(16);
    }

    rgds_shutdown();
    return 0;
}
