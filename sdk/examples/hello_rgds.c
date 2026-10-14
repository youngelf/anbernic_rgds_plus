/*
 * hello_rgds.c - Sample Dual-Screen "Hello World" & Interactive Button Inspector
 *                for the Anbernic RG DS Plus (1024x768 x 2)
 *
 * Demonstrates:
 *   - Upper Screen (RGDS_SCREEN_TOP):    Displays "HELLO" with an interactive sprite/banner
 *                                        and real-time hardware frame/screen telemetry.
 *   - Lower Screen (RGDS_SCREEN_BOTTOM): Displays "WORLD" with a live visual controller
 *                                        tester showing every RG DS Plus button state
 *                                        (D-Pad, A/B/X/Y, L1/R1/L2/R2, SELECT/START/MENU).
 */

#include "rgds_sdk.h"
#include <stdio.h>

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

int main(void) {
    if (rgds_init("Hello RG DS Plus") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }

    int banner_x = 0;
    int banner_y = 0;
    uint32_t a_press_count = 0;
    uint32_t b_press_count = 0;
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
        int card_y = 58 + banner_y;
        rgds_fill_rect(RGDS_SCREEN_TOP, card_x, card_y, 230, 92, RGDS_RGB(22, 42, 76));
        rgds_draw_rect(RGDS_SCREEN_TOP, card_x, card_y, 230, 92, 2, RGDS_COLOR_YELLOW);

        rgds_draw_text(RGDS_SCREEN_TOP, card_x + 28, card_y + 16, "HELLO", 6, RGDS_COLOR_YELLOW);
        rgds_draw_text(RGDS_SCREEN_TOP, card_x + 26, card_y + 68, "FROM TOP 1024X768 IPS", 1, RGDS_COLOR_WHITE);

        /* Telemetry footer on Upper Screen */
        char info_line[64];
        snprintf(info_line, sizeof(info_line), "UPTIME: %lluS   A:%u   B:%u   VOL:%d/10",
                 (unsigned long long)elapsed_sec, a_press_count, b_press_count, rgds_get_volume());
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 192, info_line, 1, RGDS_COLOR_GREEN);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 210, "DPAD: MOVE HELLO   X: CENTER   VOL-+/-=: VOLUME", 1, RGDS_COLOR_GRAY);

        /* ====================================================================
         * 2. RENDER LOWER SCREEN (RGDS_SCREEN_BOTTOM - 1024x768) -> "WORLD"
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(16, 18, 30));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 4, 4, RGDS_CANVAS_W - 8, RGDS_CANVAS_H - 8, 2, RGDS_COLOR_GREEN);

        /* Header bar */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 10, RGDS_CANVAS_W - 20, 22, RGDS_COLOR_SLATE);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 17, "LOWER SCREEN (DISPLAY 1 - 1024X768)", 1, RGDS_COLOR_GREEN);

        /* Large "WORLD" banner */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 45, 40, 230, 68, RGDS_RGB(20, 48, 42));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 45, 40, 230, 68, 2, RGDS_COLOR_GREEN);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 50, "WORLD", 6, RGDS_COLOR_GREEN);

        /* Live Hardware Button Tester Panel */
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 118, "LIVE ANBERNIC-RK3568-KEYS TESTER", 1, RGDS_COLOR_CYAN);

        /* Shoulder & Volume buttons row */
        draw_button_badge(RGDS_SCREEN_BOTTOM,  18, 132, 36, 16, "L2",   (input.held & RGDS_BTN_L2) != 0,       RGDS_COLOR_ORANGE);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  58, 132, 36, 16, "L1",   (input.held & RGDS_BTN_L1) != 0,       RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 116, 132, 42, 16, "VOL-", (input.held & RGDS_BTN_VOL_DOWN) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 162, 132, 42, 16, "VOL+", (input.held & RGDS_BTN_VOL_UP) != 0,   RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 226, 132, 36, 16, "R1",   (input.held & RGDS_BTN_R1) != 0,       RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 266, 132, 36, 16, "R2",   (input.held & RGDS_BTN_R2) != 0,       RGDS_COLOR_ORANGE);

        /* D-Pad cross */
        draw_button_badge(RGDS_SCREEN_BOTTOM,  44, 154, 24, 18, "UP", (input.held & RGDS_BTN_UP) != 0,    RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  18, 174, 24, 18, "LF", (input.held & RGDS_BTN_LEFT) != 0,  RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  70, 174, 24, 18, "RT", (input.held & RGDS_BTN_RIGHT) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM,  44, 194, 24, 18, "DN", (input.held & RGDS_BTN_DOWN) != 0,  RGDS_COLOR_YELLOW);

        /* Center system buttons (SELECT / MENU / START) */
        draw_button_badge(RGDS_SCREEN_BOTTOM, 112, 174, 30, 16, "SEL", (input.held & RGDS_BTN_SELECT) != 0, RGDS_COLOR_ORANGE);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 145, 174, 30, 16, "MEN", (input.held & RGDS_BTN_MENU) != 0,   RGDS_COLOR_RED);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 178, 174, 30, 16, "STA", (input.held & RGDS_BTN_START) != 0,  RGDS_COLOR_ORANGE);

        /* ABXY Diamond (Nintendo/Anbernic layout: X=North, Y=West, A=East, B=South) */
        draw_button_badge(RGDS_SCREEN_BOTTOM, 248, 154, 24, 18, "X", (input.held & RGDS_BTN_X) != 0, RGDS_COLOR_CYAN);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 222, 174, 24, 18, "Y", (input.held & RGDS_BTN_Y) != 0, RGDS_COLOR_YELLOW);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 274, 174, 24, 18, "A", (input.held & RGDS_BTN_A) != 0, RGDS_COLOR_RED);
        draw_button_badge(RGDS_SCREEN_BOTTOM, 248, 194, 24, 18, "B", (input.held & RGDS_BTN_B) != 0, RGDS_COLOR_GREEN);

        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 220, "EXIT: MENU BUTTON OR START+SELECT (ESC ON PC)", 1, RGDS_COLOR_GRAY);

        /* Present both 1024x768 screens */
        rgds_present();
        rgds_delay_ms(16);
    }

    rgds_shutdown();
    return 0;
}
