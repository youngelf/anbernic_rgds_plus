/*
 * hello_rgds.c - Sample Dual-Screen "Hello World", Button & Touchscreen Inspector
 *                for the Anbernic RG DS Plus (1024x768 x 2)
 *
 * Demonstrates:
 *   - Upper Screen (RGDS_SCREEN_TOP):    Displays "HELLO" with an interactive sprite/banner
 *                                        and real-time hardware frame/screen/touch telemetry.
 *   - Lower Screen (RGDS_SCREEN_BOTTOM): Displays "WORLD" with a live visual controller
 *                                        tester showing every RG DS Plus button state
 *                                        (D-Pad, A/B/X/Y, L1/R1/L2/R2, SELECT/START/MENU)
 *                                        AND live 10-point capacitive touchscreen crosshairs
 *                                        & drawing trail ("goodix-ts" GT911).
 */

#include "rgds_sdk.h"
#include <stdio.h>

#define TRAIL_CAPACITY 128

typedef struct {
    int           x;
    int           y;
    uint32_t      color;
    rgds_screen_t screen;
} touch_trail_pt_t;

static void draw_button_badge(rgds_screen_t scr, int x, int y, int w, int h,
                              const char *label, bool held, uint32_t active_color) {
    uint32_t bg = held ? active_color : RGDS_RGB(24, 34, 54);
    uint32_t border = held ? RGDS_COLOR_WHITE : RGDS_COLOR_BORDER;
    uint32_t fg = held ? RGDS_COLOR_BLACK : RGDS_COLOR_WHITE;

    rgds_fill_rect(scr, x, y, w, h, bg);
    rgds_draw_rect(scr, x, y, w, h, 1, border);

    int tw = rgds_text_width(label, 1);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 7) / 2;
    rgds_draw_text(scr, tx, ty, label, 1, fg);
}

static void draw_touch_crosshair(rgds_screen_t scr, int x, int y, int slot_id, uint32_t color) {
    /* Horizontal and vertical crosshair arms */
    rgds_fill_rect(scr, x - 8, y, 17, 1, color);
    rgds_fill_rect(scr, x, y - 8, 1, 17, color);
    /* Outer box around touch contact point */
    rgds_draw_rect(scr, x - 4, y - 4, 9, 9, 1, RGDS_COLOR_WHITE);

    char slot_lbl[16];
    snprintf(slot_lbl, sizeof(slot_lbl), "T%d", slot_id);
    int lx = (x + 6 + 14 < RGDS_CANVAS_W) ? (x + 6) : (x - 16);
    int ly = (y - 10 >= 4) ? (y - 10) : (y + 6);
    rgds_draw_text(scr, lx, ly, slot_lbl, 1, color);
}

int main(void) {
    if (rgds_init("Hello RG DS Plus") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }

    static const uint32_t k_slot_colors[RGDS_MAX_TOUCH_POINTS] = {
        RGDS_COLOR_CYAN,
        RGDS_COLOR_YELLOW,
        RGDS_COLOR_ORANGE,
        RGDS_COLOR_GREEN,
        RGDS_COLOR_RED,
        RGDS_RGB(210, 120, 255),
        RGDS_RGB(80,  255, 210),
        RGDS_RGB(255, 130, 190),
        RGDS_RGB(190, 245, 90),
        RGDS_COLOR_WHITE
    };

    touch_trail_pt_t trail[TRAIL_CAPACITY];
    int trail_head  = 0;
    int trail_count = 0;

    int banner_x = 0;
    int banner_y = 0;
    uint32_t a_press_count = 0;
    uint32_t b_press_count = 0;
    uint32_t touch_tap_count = 0;
    uint64_t start_ms = rgds_ticks_ms();

    rgds_input_state_t input;
    while (rgds_poll_input(&input)) {
        /* Update interactive state from D-Pad and face buttons */
        if (input.held & RGDS_BTN_LEFT)  banner_x -= 2;
        if (input.held & RGDS_BTN_RIGHT) banner_x += 2;
        if (input.held & RGDS_BTN_UP)    banner_y -= 2;
        if (input.held & RGDS_BTN_DOWN)  banner_y += 2;
        if (banner_x < -70) banner_x = -70;
        if (banner_x >  70) banner_x =  70;
        if (banner_y < -25) banner_y = -25;
        if (banner_y >  25) banner_y =  25;

        if (input.pressed & RGDS_BTN_A) a_press_count++;
        if (input.pressed & RGDS_BTN_B) b_press_count++;
        if (input.pressed & RGDS_BTN_X) {
            banner_x = 0;
            banner_y = 0;
        }
        if (input.pressed & RGDS_BTN_Y) {
            trail_head  = 0;
            trail_count = 0;
        }

        if (input.touch.just_pressed) {
            touch_tap_count++;
        }

        /* Record active multi-touch points into the visual trail buffer */
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            const rgds_touch_point_t *pt = &input.touch.points[s];
            if (pt->active) {
                trail[trail_head].x      = pt->x;
                trail[trail_head].y      = pt->y;
                trail[trail_head].color  = k_slot_colors[s % RGDS_MAX_TOUCH_POINTS];
                trail[trail_head].screen = pt->screen;
                trail_head = (trail_head + 1) % TRAIL_CAPACITY;
                if (trail_count < TRAIL_CAPACITY) trail_count++;
            }
        }

        uint64_t elapsed_sec = (rgds_ticks_ms() - start_ms) / 1000ULL;

        /* ====================================================================
         * 1. RENDER UPPER SCREEN (RGDS_SCREEN_TOP - 1024x768) -> "HELLO"
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_TOP, RGDS_COLOR_NAVY);
        rgds_draw_rect(RGDS_SCREEN_TOP, 4, 4, RGDS_CANVAS_W - 8, RGDS_CANVAS_H - 8, 2, RGDS_COLOR_CYAN);

        /* Header bar */
        rgds_fill_rect(RGDS_SCREEN_TOP, 10, 10, RGDS_CANVAS_W - 20, 22, RGDS_COLOR_SLATE);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 17, "UPPER SCREEN (DISPLAY 0 - 1024X768)", 1, RGDS_COLOR_CYAN);

        /* Movable "HELLO" hero card */
        int card_x = 45 + banner_x;
        int card_y = 54 + banner_y;
        rgds_fill_rect(RGDS_SCREEN_TOP, card_x, card_y, 230, 90, RGDS_RGB(22, 42, 76));
        rgds_draw_rect(RGDS_SCREEN_TOP, card_x, card_y, 230, 90, 2, RGDS_COLOR_YELLOW);

        rgds_draw_text(RGDS_SCREEN_TOP, card_x + 28, card_y + 15, "HELLO", 6, RGDS_COLOR_YELLOW);
        rgds_draw_text(RGDS_SCREEN_TOP, card_x + 26, card_y + 66, "FROM TOP 1024X768 IPS", 1, RGDS_COLOR_WHITE);

        /* Telemetry footer on Upper Screen */
        char info_line[64];
        snprintf(info_line, sizeof(info_line), "UP:%lluS  A:%u  B:%u  TAPS:%u  VOL:%d/10",
                 (unsigned long long)elapsed_sec, a_press_count, b_press_count,
                 touch_tap_count, rgds_get_volume());
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 184, info_line, 1, RGDS_COLOR_GREEN);

        char touch_line[64];
        if (input.touch.touching) {
            snprintf(touch_line, sizeof(touch_line),
                     "TOUCH [%d]: (%3d,%3d) NAT:(%4d,%3d)",
                     input.touch.count, input.touch.x, input.touch.y,
                     input.touch.native_x, input.touch.native_y);
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 198, touch_line, 1, RGDS_COLOR_YELLOW);
        } else {
            snprintf(touch_line, sizeof(touch_line),
                     "TOUCH [0]: LAST (%3d,%3d) NAT:(%4d,%3d)",
                     input.touch.x, input.touch.y,
                     input.touch.native_x, input.touch.native_y);
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 198, touch_line, 1, RGDS_COLOR_CYAN);
        }
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 214, "DPAD:MOVE  X:CENTER  Y:CLR TRAIL  VOL-+/-=:VOL", 1, RGDS_COLOR_GRAY);

        /* ====================================================================
         * 2. RENDER LOWER SCREEN (RGDS_SCREEN_BOTTOM - 1024x768) -> "WORLD"
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(16, 18, 30));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 4, 4, RGDS_CANVAS_W - 8, RGDS_CANVAS_H - 8, 2, RGDS_COLOR_GREEN);

        /* Header bar */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 10, RGDS_CANVAS_W - 20, 22, RGDS_COLOR_SLATE);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 17, "LOWER TOUCH SCREEN (DISPLAY 1 - 1024X768)", 1, RGDS_COLOR_GREEN);

        /* Large "WORLD" banner */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 45, 38, 230, 64, RGDS_RGB(20, 48, 42));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 45, 38, 230, 64, 2, RGDS_COLOR_GREEN);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 47, "WORLD", 6, RGDS_COLOR_GREEN);

        /* Live Hardware Button & Touchscreen Tester Panel */
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 110, "KEYS + GOODIX GT911 10-PT TOUCH TESTER", 1, RGDS_COLOR_CYAN);

        /* Shoulder & Volume buttons row */
        draw_button_badge(RGDS_SCREEN_BOTTOM,  18, 124, 36, 16, "L2",   (input.held & RGDS_BTN_L2) != 0,       RGDS_COLOR_ORANGE);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  58, 124, 36, 16, "L1",   (input.held & RGDS_BTN_L1) != 0,       RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 116, 124, 42, 16, "VOL-", (input.held & RGDS_BTN_VOL_DOWN) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 162, 124, 42, 16, "VOL+", (input.held & RGDS_BTN_VOL_UP) != 0,   RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 226, 124, 36, 16, "R1",   (input.held & RGDS_BTN_R1) != 0,       RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 266, 124, 36, 16, "R2",   (input.held & RGDS_BTN_R2) != 0,       RGDS_COLOR_ORANGE);

        /* D-Pad cross */
        draw_button_badge(RGDS_SCREEN_BOTTOM,  44, 146, 24, 18, "UP", (input.held & RGDS_BTN_UP) != 0,    RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  18, 166, 24, 18, "LF", (input.held & RGDS_BTN_LEFT) != 0,  RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  70, 166, 24, 18, "RT", (input.held & RGDS_BTN_RIGHT) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  44, 186, 24, 18, "DN", (input.held & RGDS_BTN_DOWN) != 0,  RGDS_COLOR_YELLOW);

        /* Center system buttons (SELECT / MENU / START) + TOUCH indicator badge */
        draw_button_badge(RGDS_SCREEN_BOTTOM, 112, 156, 30, 16, "SEL", (input.held & RGDS_BTN_SELECT) != 0, RGDS_COLOR_ORANGE);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 145, 156, 30, 16, "MEN", (input.held & RGDS_BTN_MENU) != 0,   RGDS_COLOR_RED);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 178, 156, 30, 16, "STA", (input.held & RGDS_BTN_START) != 0,  RGDS_COLOR_ORANGE);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 112, 178, 96, 16,
                          input.touch.touching ? "TOUCH ACTIVE" : "TOUCH READY",
                          input.touch.touching, RGDS_COLOR_CYAN);

        /* ABXY Diamond (Nintendo/Anbernic layout: X=North, Y=West, A=East, B=South) */
        draw_button_badge(RGDS_SCREEN_BOTTOM, 248, 146, 24, 18, "X", (input.held & RGDS_BTN_X) != 0, RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 222, 166, 24, 18, "Y", (input.held & RGDS_BTN_Y) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 274, 166, 24, 18, "A", (input.held & RGDS_BTN_A) != 0, RGDS_COLOR_RED);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 248, 186, 24, 18, "B", (input.held & RGDS_BTN_B) != 0, RGDS_COLOR_GREEN);

        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 212, "TOUCH/DRAG SCREEN TO DRAW   Y: CLEAR TRAIL", 1, RGDS_COLOR_YELLOW);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 224, "EXIT: MENU BUTTON OR START+SELECT (ESC ON PC)", 1, RGDS_COLOR_GRAY);

        /* Render touch trail points on whichever screen they were drawn on */
        for (int i = 0; i < trail_count; i++) {
            rgds_fill_rect(trail[i].screen, trail[i].x - 1, trail[i].y - 1, 3, 3, trail[i].color);
        }

        /* Render live multi-touch crosshairs for all active touch slots (0..9) */
        for (int s = 0; s < RGDS_MAX_TOUCH_POINTS; s++) {
            const rgds_touch_point_t *pt = &input.touch.points[s];
            if (pt->active) {
                draw_touch_crosshair(pt->screen, pt->x, pt->y, s, k_slot_colors[s % RGDS_MAX_TOUCH_POINTS]);
            }
        }

        /* Present both 1024x768 screens */
        rgds_present();
        rgds_delay_ms(16);
    }

    rgds_shutdown();
    return 0;
}
