/*
 * metronome - Dual-Screen Precision Metronome for Anbernic RG DS Plus (Linux OS) & Linux Host
 *
 * Target Hardware:
 *   Anbernic RG DS Plus (Rockchip RK3568, ARM Cortex-A55 aarch64, Dual 4:3 1024x768 IPS screens)
 *   running Anbernic's Linux-based OS (Weston/Wayland + SDL2 + DRM/KMS), plus x86_64 Linux desktop X11 host.
 *
 * Dual-Screen Layout (Each Screen is 1024x768 on RG DS Plus):
 *   - UPPER SCREEN (1024x768): Active Metronome (BPM, Classical Marking, Time Signature,
 *                              Beat Visualizer Boxes with Beat 1 Accent, Swinging Pendulum)
 *   - LOWER SCREEN (1024x768): Overall & Session Practice Statistics (Session Time, Session
 *                              Beats & Bars, Lifetime Practice Time, Lifetime Beats & Bars,
 *                              Min/Max/Avg BPM, and Per-Time-Signature Practice Breakdown)
 *
 * Controls on Anbernic RG DS Plus & Linux Host:
 *   A Button (Space / 'A')          : Play / Pause metronome
 *   D-Pad UP / DOWN                 : Increase / Decrease Tempo (+/-1 BPM, hold for continuous repeat)
 *   D-Pad LEFT / RIGHT              : Cycle Time Signature (4/4, 3/4, 2/4, 5/4, 7/4)
 *   L1 / R1 / L2 / R2 Shoulders     : Jump Tempo -5 / +5 BPM
 *   B Button ('C' on Linux)         : Reset current session counters (keeps lifetime stats)
 *   MENU (M) / SELECT / START+SELECT: Save stats & Exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

/*
 * Each screen uses a 320x240 logical 4:3 canvas:
 *   - On Anbernic RG DS Plus: scaled 3.2x (exact 1024x768 per screen, 4:3 aspect ratio)
 *   - On Linux X11 desktop  : scaled 2x per screen (640x480 each, stacked 640x968 window)
 */
#define CANVAS_W 320
#define CANVAS_H 240

#define NATIVE_W 1024
#define NATIVE_H 768

/* On Linux X11 desktop, stack Upper and Lower screens with an 8px hinge divider */
#define X11_SCALE 2
#define X11_SCR_W (CANVAS_W * X11_SCALE) /* 640 */
#define X11_SCR_H (CANVAS_H * X11_SCALE) /* 480 */
#define HINGE_H   8
#define X11_WIN_W X11_SCR_W
#define X11_WIN_H (X11_SCR_H * 2 + HINGE_H) /* 968 */

#define SAMPLE_RATE 44100
#define AUDIO_CHANNELS 2

#define MIN_BPM 30
#define MAX_BPM 260
#define DEFAULT_BPM 120

/* Time Signatures: 4/4, 3/4, 2/4, 5/4, 7/4 */
#define NUM_TIME_SIGS 5
static const int k_time_sig_beats[NUM_TIME_SIGS] = { 4, 3, 2, 5, 7 };
static const char *k_time_sig_names[NUM_TIME_SIGS] = { "4/4", "3/4", "2/4", "5/4", "7/4" };

/* Logical input actions */
typedef enum {
    ACT_NONE = 0,
    ACT_PLAY_PAUSE,
    ACT_BPM_UP,
    ACT_BPM_DOWN,
    ACT_BPM_UP_FAST,
    ACT_BPM_DOWN_FAST,
    ACT_SIG_PREV,
    ACT_SIG_NEXT,
    ACT_VOL_UP,
    ACT_VOL_DOWN,
    ACT_RESET_SESSION,
    ACT_QUIT
} InputAction;

/* Continuous auto-repeat state for holding D-Pad UP/DOWN and Volume UP/DOWN */
#define REPEAT_INITIAL_DELAY_MS 320
#define REPEAT_INTERVAL_MS      65
#define VOL_REPEAT_INTERVAL_MS  85

static int      g_dpad_vert_dir = 0; /* -1 = UP (BPM+), +1 = DOWN (BPM-), 0 = neutral */
static uint32_t g_dpad_next_repeat_ms = 0;
static int      g_vol_dir = 0;       /* +1 = VOL+, -1 = VOL-, 0 = neutral */
static uint32_t g_vol_next_repeat_ms = 0;

static uint32_t get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

static void delay_ms(uint32_t ms) {
    usleep(ms * 1000);
}

static inline uint32_t make_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF000000U) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* =========================================================================
 * Maximum-Loudness Click Synthesizer (Full-Scale +/-32767 Saturated Harmonic)
 * ========================================================================= */
#define MIN_VOLUME     0
#define MAX_VOLUME     10
#define DEFAULT_VOLUME 10

static volatile int g_bpm = DEFAULT_BPM;
static volatile int g_time_sig_idx = 0;
static volatile bool g_playing = false;
static volatile int g_volume = DEFAULT_VOLUME; /* 0 (Mute) .. 10 (100% Max Volume) */
static bool g_vol_from_config = false;

static volatile int g_click_samples_left = 0;
static volatile bool g_click_is_accent = false;
static uint32_t g_click_sample_pos = 0;

#define ACCENT_CLICK_MS 75
#define NORMAL_CLICK_MS 55

static void trigger_click(bool accent) {
    g_click_is_accent = accent;
    g_click_sample_pos = 0;
    int duration_ms = accent ? ACCENT_CLICK_MS : NORMAL_CLICK_MS;
    g_click_samples_left = (SAMPLE_RATE * duration_ms) / 1000;
}

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *samples = (int16_t *)stream;
    int frames = len / (2 * AUDIO_CHANNELS);
    int vol = g_volume;
    if (vol < MIN_VOLUME) vol = MIN_VOLUME;
    if (vol > MAX_VOLUME) vol = MAX_VOLUME;
    double vol_gain = (double)vol / (double)MAX_VOLUME;

    for (int i = 0; i < frames; i++) {
        int rem = g_click_samples_left;
        if (rem > 0) {
            bool accent = g_click_is_accent;
            uint32_t pos = g_click_sample_pos++;
            g_click_samples_left = rem - 1;

            if (vol == 0) {
                for (int c = 0; c < AUDIO_CHANNELS; c++) {
                    samples[i * AUDIO_CHANNELS + c] = 0;
                }
                continue;
            }

            int total_click_len = (SAMPLE_RATE * (accent ? ACCENT_CLICK_MS : NORMAL_CLICK_MS)) / 1000;
            double progress = (double)pos / (double)total_click_len;

            double sustain_frac = accent ? 0.70 : 0.65;
            double env = 1.0;
            if (progress > sustain_frac) {
                env = (1.0 - progress) / (1.0 - sustain_frac);
            }

            double freq = accent ? 1760.0 : 1000.0;
            if (pos < (SAMPLE_RATE * 5) / 1000) {
                freq *= 1.35;
            }

            double phase = (2.0 * 3.14159265358979323846 * freq * (double)pos) / (double)SAMPLE_RATE;

            double raw = sin(phase) + 0.65 * sin(phase * 2.0) + 0.45 * sin(phase * 3.0);
            if (accent) {
                raw += 0.50 * sin(phase * 0.5);
            }

            double driven = raw * 3.5;
            if (driven > 1.0)  driven = 1.0;
            if (driven < -1.0) driven = -1.0;

            int32_t val = (int32_t)(driven * 32767.0 * env * vol_gain);
            if (val > 32767)  val = 32767;
            if (val < -32767) val = -32767;

            int16_t s = (int16_t)val;
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                samples[i * AUDIO_CHANNELS + c] = s;
            }
        } else {
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                samples[i * AUDIO_CHANNELS + c] = 0;
            }
        }
    }
}

/* =========================================================================
 * Dual-Screen Platform Backend:
 *   1. HOST_X11   : Linux Desktop (X11 Dual-Screen Window + PulseAudio)
 *   2. RG DS Plus : Anbernic RG DS Plus Linux OS (Dual 1024x768 SDL2 Windows on
 *                   Weston/Wayland Display 0 & Display 1, plus fbdev fallback)
 * ========================================================================= */
#if defined(HOST_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Display *g_x11_dpy = NULL;
static Window g_x11_win = 0;
static GC g_x11_gc = 0;
static XImage *g_x11_img = NULL;
static Atom g_wm_delete_window = 0;
static char *g_scaled_buf = NULL;

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

typedef struct pa_simple pa_simple;
typedef struct {
    int format;
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;

#define PA_STREAM_PLAYBACK 1
#define PA_SAMPLE_S16LE    3

typedef pa_simple* (*pa_simple_new_fn)(const char*, const char*, int, const char*,
                                       const char*, const pa_sample_spec*,
                                       const void*, const void*, int*);
typedef int (*pa_simple_write_fn)(pa_simple*, const void*, size_t, int*);
typedef void (*pa_simple_free_fn)(pa_simple*);

static void *host_audio_worker(void *arg) {
    (void)arg;
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    pa_simple *pa = NULL;
    pa_simple_new_fn fn_new = NULL;
    pa_simple_write_fn fn_write = NULL;
    pa_simple_free_fn fn_free = NULL;

    if (pa_lib) {
        fn_new = (pa_simple_new_fn)dlsym(pa_lib, "pa_simple_new");
        fn_write = (pa_simple_write_fn)dlsym(pa_lib, "pa_simple_write");
        fn_free = (pa_simple_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_sample_spec ss = { PA_SAMPLE_S16LE, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            pa = fn_new(NULL, "RG DS Plus Metronome", PA_STREAM_PLAYBACK, NULL, "Metronome Click", &ss, NULL, NULL, &err);
        }
    }

    int chunk_frames = 512;
    int chunk_bytes = chunk_frames * AUDIO_CHANNELS * 2;
    int16_t buf[512 * AUDIO_CHANNELS];

    while (g_audio_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        if (pa && fn_write) {
            int err = 0;
            fn_write(pa, buf, chunk_bytes, &err);
        } else {
            usleep((chunk_frames * 1000000ULL) / SAMPLE_RATE);
        }
    }

    if (pa && fn_free) fn_free(pa);
    if (pa_lib) dlclose(pa_lib);
    return NULL;
}

static bool platform_init(void) {
    g_x11_dpy = XOpenDisplay(NULL);
    if (!g_x11_dpy) {
        fprintf(stderr, "Cannot open X11 DISPLAY\n");
        return false;
    }
    int scr = DefaultScreen(g_x11_dpy);
    g_x11_win = XCreateSimpleWindow(
        g_x11_dpy, RootWindow(g_x11_dpy, scr),
        80, 40, X11_WIN_W, X11_WIN_H, 1,
        BlackPixel(g_x11_dpy, scr), BlackPixel(g_x11_dpy, scr)
    );

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = X11_WIN_W;
    hints.min_height = hints.max_height = X11_WIN_H;
    XSetWMNormalHints(g_x11_dpy, g_x11_win, &hints);

    XSelectInput(g_x11_dpy, g_x11_win, KeyPressMask | KeyReleaseMask | ExposureMask | StructureNotifyMask);
    g_wm_delete_window = XInternAtom(g_x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_x11_dpy, g_x11_win, &g_wm_delete_window, 1);
    XStoreName(g_x11_dpy, g_x11_win, "Anbernic RG DS Plus Dual-Screen Metronome (1024x768 x 2 Preview)");
    XMapWindow(g_x11_dpy, g_x11_win);
    g_x11_gc = DefaultGC(g_x11_dpy, scr);

    g_scaled_buf = (char *)calloc(X11_WIN_W * X11_WIN_H, 4);
    g_x11_img = XCreateImage(
        g_x11_dpy, DefaultVisual(g_x11_dpy, scr),
        DefaultDepth(g_x11_dpy, scr), ZPixmap, 0,
        g_scaled_buf, X11_WIN_W, X11_WIN_H, 32, 0
    );

    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, host_audio_worker, NULL);
    return true;
}

static void platform_present_dual(const uint32_t *upper_canvas, const uint32_t *lower_canvas) {
    uint32_t *dst = (uint32_t *)g_scaled_buf;

    /* 1. Upper Screen (y: 0 .. 479) */
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = upper_canvas[y * CANVAS_W + x];
            for (int sy = 0; sy < X11_SCALE; sy++) {
                uint32_t *row = dst + (y * X11_SCALE + sy) * X11_WIN_W + (x * X11_SCALE);
                for (int sx = 0; sx < X11_SCALE; sx++) {
                    row[sx] = c;
                }
            }
        }
    }

    /* 2. Hinge Divider Bar (y: 480 .. 487) */
    for (int y = X11_SCR_H; y < X11_SCR_H + HINGE_H; y++) {
        uint32_t *row = dst + y * X11_WIN_W;
        uint32_t col = (y == X11_SCR_H || y == X11_SCR_H + HINGE_H - 1) ? 0xFF3A4664U : 0xFF080A10U;
        for (int x = 0; x < X11_WIN_W; x++) {
            row[x] = col;
        }
    }

    /* 3. Lower Screen (y: 488 .. 967) */
    int y_off = X11_SCR_H + HINGE_H;
    for (int y = 0; y < CANVAS_H; y++) {
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = lower_canvas[y * CANVAS_W + x];
            for (int sy = 0; sy < X11_SCALE; sy++) {
                uint32_t *row = dst + (y_off + y * X11_SCALE + sy) * X11_WIN_W + (x * X11_SCALE);
                for (int sx = 0; sx < X11_SCALE; sx++) {
                    row[sx] = c;
                }
            }
        }
    }

    XPutImage(g_x11_dpy, g_x11_win, g_x11_gc, g_x11_img, 0, 0, 0, 0, X11_WIN_W, X11_WIN_H);
    XFlush(g_x11_dpy);
}

static InputAction platform_poll_input(void) {
    while (XPending(g_x11_dpy) > 0) {
        XEvent xev;
        XNextEvent(g_x11_dpy, &xev);
        if (xev.type == ClientMessage) {
            if ((Atom)xev.xclient.data.l[0] == g_wm_delete_window) {
                return ACT_QUIT;
            }
        } else if (xev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&xev.xkey, 0);
            if (ks >= XK_A && ks <= XK_Z) ks = ks - XK_A + XK_a;
            switch (ks) {
                case XK_a:
                case XK_space:
                case XK_Return:
                case XK_Alt_L:
                    return ACT_PLAY_PAUSE;
                case XK_b:
                case XK_c:
                    return ACT_RESET_SESSION;
                case XK_Up:
                    if (g_dpad_vert_dir != -1) {
                        g_dpad_vert_dir = -1;
                        g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                        return ACT_BPM_UP;
                    }
                    break;
                case XK_Down:
                    if (g_dpad_vert_dir != 1) {
                        g_dpad_vert_dir = 1;
                        g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                        return ACT_BPM_DOWN;
                    }
                    break;
                case XK_Left:
                    return ACT_SIG_PREV;
                case XK_Right:
                    return ACT_SIG_NEXT;
                case XK_Page_Up:
                case XK_BackSpace:
                case XK_bracketright:
                    return ACT_BPM_UP_FAST;
                case XK_Page_Down:
                case XK_Tab:
                case XK_bracketleft:
                    return ACT_BPM_DOWN_FAST;
                case XK_equal:
                case XK_plus:
                case XK_KP_Add:
                    if (g_vol_dir != 1) {
                        g_vol_dir = 1;
                        g_vol_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                        return ACT_VOL_UP;
                    }
                    break;
                case XK_minus:
                case XK_underscore:
                case XK_KP_Subtract:
                    if (g_vol_dir != -1) {
                        g_vol_dir = -1;
                        g_vol_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                        return ACT_VOL_DOWN;
                    }
                    break;
                case XK_q:
                case XK_Escape:
                    return ACT_QUIT;
                default:
                    break;
            }
        } else if (xev.type == KeyRelease) {
            if (XEventsQueued(g_x11_dpy, QueuedAfterReading)) {
                XEvent nev;
                XPeekEvent(g_x11_dpy, &nev);
                if (nev.type == KeyPress && nev.xkey.time == xev.xkey.time &&
                    nev.xkey.keycode == xev.xkey.keycode) {
                    XNextEvent(g_x11_dpy, &xev);
                    continue;
                }
            }
            KeySym ks = XLookupKeysym(&xev.xkey, 0);
            if ((ks == XK_Up && g_dpad_vert_dir == -1) ||
                (ks == XK_Down && g_dpad_vert_dir == 1)) {
                g_dpad_vert_dir = 0;
            }
            if (((ks == XK_equal || ks == XK_plus || ks == XK_KP_Add) && g_vol_dir == 1) ||
                ((ks == XK_minus || ks == XK_underscore || ks == XK_KP_Subtract) && g_vol_dir == -1)) {
                g_vol_dir = 0;
            }
        }
    }

    if (g_dpad_vert_dir != 0) {
        uint32_t now = get_ticks_ms();
        if ((int32_t)(now - g_dpad_next_repeat_ms) >= 0) {
            g_dpad_next_repeat_ms = now + REPEAT_INTERVAL_MS;
            return (g_dpad_vert_dir < 0) ? ACT_BPM_UP : ACT_BPM_DOWN;
        }
    }
    if (g_vol_dir != 0) {
        uint32_t now = get_ticks_ms();
        if ((int32_t)(now - g_vol_next_repeat_ms) >= 0) {
            g_vol_next_repeat_ms = now + VOL_REPEAT_INTERVAL_MS;
            return (g_vol_dir > 0) ? ACT_VOL_UP : ACT_VOL_DOWN;
        }
    }
    return ACT_NONE;
}

static void platform_set_volume(int vol) {
    if (vol < MIN_VOLUME) vol = MIN_VOLUME;
    if (vol > MAX_VOLUME) vol = MAX_VOLUME;
    g_volume = vol;
}

static void platform_shutdown(void) {
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    if (g_x11_img) {
        XDestroyImage(g_x11_img);
        g_x11_img = NULL;
    }
    if (g_x11_dpy) {
        if (g_x11_win) XDestroyWindow(g_x11_dpy, g_x11_win);
        XCloseDisplay(g_x11_dpy);
        g_x11_dpy = NULL;
    }
}

#else /* Anbernic RG DS Plus Target (Rockchip RK3568 aarch64 Dual 1024x768 Linux OS) */

#include <linux/fb.h>
#include <linux/input.h>
#include <sys/mman.h>

#define SDL2_INIT_TIMER                0x00000001U
#define SDL2_INIT_AUDIO                0x00000010U
#define SDL2_INIT_VIDEO                0x00000020U
#define SDL2_WINDOWPOS_UNDEFINED_MASK  0x1FFF0000U
#define SDL2_WINDOWPOS_CENTERED_MASK   0x2FFF0000U
#define SDL2_WINDOWPOS_DISPLAY(X)      (SDL2_WINDOWPOS_UNDEFINED_MASK | (X))
#define SDL2_WINDOWPOS_CENTERED_DISPLAY(X) (SDL2_WINDOWPOS_CENTERED_MASK | (X))
#define SDL2_WINDOW_FULLSCREEN         0x00000001U
#define SDL2_WINDOW_SHOWN              0x00000004U
#define SDL2_WINDOW_BORDERLESS         0x00000010U
#define SDL2_WINDOW_FULLSCREEN_DESKTOP 0x00001001U
#define SDL2_WINDOW_ALWAYS_ON_TOP      0x00008000U
/* 0x3001 matches subscreen.dge on RG DS Plus */
#define SDL2_RGDS_WIN_FLAGS            0x00003001U
#define SDL2_RENDERER_SOFTWARE         0x00000001U
#define SDL2_RENDERER_ACCELERATED      0x00000002U
#define SDL2_RENDERER_PRESENTVSYNC     0x00000004U
#define SDL2_RENDERER_TARGETTEXTURE    0x00000008U
#define SDL2_PIXELFORMAT_ARGB8888      0x16362004U
#define SDL2_TEXTUREACCESS_STREAMING   1
#define SDL2_AUDIO_S16LSB              0x8010

typedef void (*SDL2_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int x, y, w, h;
} SDL2_Rect;

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    SDL2_AudioCallback callback;
    void *userdata;
} SDL2_AudioSpec;

typedef union {
    uint32_t type;
    uint8_t padding[64];
} SDL2_Event;

#define SDL2_QUIT 0x100

static void *g_sdl2_lib = NULL;
static void *g_sdl2_win_top = NULL;
static void *g_sdl2_ren_top = NULL;
static void *g_sdl2_tex_top = NULL;

static void *g_sdl2_win_bot = NULL;
static void *g_sdl2_ren_bot = NULL;
static void *g_sdl2_tex_bot = NULL;

static bool  g_use_sdl2_video = false;
static bool  g_sdl2_dual_windows = false;
static bool  g_sdl2_stacked_single = false;
static bool  g_use_sdl2_audio = false;

static int  (*fn_SDL_Init)(uint32_t) = NULL;
static void (*fn_SDL_Quit)(void) = NULL;
static int  (*fn_SDL_SetHint)(const char*, const char*) = NULL;
static int  (*fn_SDL_GetNumVideoDisplays)(void) = NULL;
static int  (*fn_SDL_GetDisplayBounds)(int, SDL2_Rect*) = NULL;
static void*(*fn_SDL_CreateWindow)(const char*, int, int, int, int, uint32_t) = NULL;
static void (*fn_SDL_DestroyWindow)(void*) = NULL;
static void*(*fn_SDL_CreateRenderer)(void*, int, uint32_t) = NULL;
static void (*fn_SDL_DestroyRenderer)(void*) = NULL;
static void*(*fn_SDL_CreateTexture)(void*, uint32_t, int, int, int) = NULL;
static void (*fn_SDL_DestroyTexture)(void*) = NULL;
static int  (*fn_SDL_UpdateTexture)(void*, const void*, const void*, int) = NULL;
static int  (*fn_SDL_SetRenderDrawColor)(void*, uint8_t, uint8_t, uint8_t, uint8_t) = NULL;
static int  (*fn_SDL_RenderClear)(void*) = NULL;
static int  (*fn_SDL_RenderCopy)(void*, void*, const void*, const void*) = NULL;
static void (*fn_SDL_RenderPresent)(void*) = NULL;
static int  (*fn_SDL_PollEvent)(SDL2_Event*) = NULL;
static int  (*fn_SDL_OpenAudio)(SDL2_AudioSpec*, SDL2_AudioSpec*) = NULL;
static void (*fn_SDL_PauseAudio)(int) = NULL;
static void (*fn_SDL_CloseAudio)(void) = NULL;
static int  (*fn_SDL_ShowCursor)(int) = NULL;

/* --- Framebuffer State (/dev/fb0 for Upper/Stacked, /dev/fb1 for Lower) --- */
typedef struct {
    int fd;
    uint8_t *mem;
    size_t map_size;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
} FbDevice;

static FbDevice g_fb0 = { .fd = -1 };
static FbDevice g_fb1 = { .fd = -1 };

/* Native 1024x768 per-screen buffers for RG DS Plus */
static uint32_t g_scaled_upper[NATIVE_W * NATIVE_H];
static uint32_t g_scaled_lower[NATIVE_W * NATIVE_H];
static uint32_t g_scaled_stacked[NATIVE_W * NATIVE_H * 2];

/* Precomputed X and Y lookup tables for fast 320x240 -> 1024x768 scaling */
static int g_lut_x[NATIVE_W];
static int g_lut_y[NATIVE_H];

static void init_scale_lut(void) {
    for (int x = 0; x < NATIVE_W; x++) {
        g_lut_x[x] = (x * CANVAS_W) / NATIVE_W;
    }
    for (int y = 0; y < NATIVE_H; y++) {
        g_lut_y[y] = (y * CANVAS_H) / NATIVE_H;
    }
}

static void scale_canvas_to_native(const uint32_t *canvas, uint32_t *dst_1024x768) {
    for (int y = 0; y < NATIVE_H; y++) {
        const uint32_t *src_row = canvas + g_lut_y[y] * CANVAS_W;
        uint32_t *dst_row = dst_1024x768 + y * NATIVE_W;
        for (int x = 0; x < NATIVE_W; x++) {
            dst_row[x] = src_row[g_lut_x[x]];
        }
    }
}

/* --- Direct Linux evdev (/dev/input/event*) Polling State --- */
#define MAX_EVDEV_FDS 16
static int g_evdev_fds[MAX_EVDEV_FDS];
static int g_num_evdev_fds = 0;
static bool g_btn_select_held = false;
static bool g_btn_start_held = false;
static int g_hat_x = 0;
static int g_hat_y = 0;

/* --- Fallback ALSA / PulseAudio Thread --- */
static pthread_t g_alsa_thread;
static volatile bool g_alsa_running = false;

typedef int (*snd_pcm_open_fn)(void**, const char*, int, int);
typedef int (*snd_pcm_set_params_fn)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*snd_pcm_writei_fn)(void*, const void*, unsigned long);
typedef int (*snd_pcm_recover_fn)(void*, int, int);
typedef int (*snd_pcm_close_fn)(void*);

static void *alsa_audio_worker(void *arg) {
    (void)arg;
    void *asound = dlopen("libasound.so.2", RTLD_LAZY);
    if (!asound) return NULL;

    snd_pcm_open_fn fn_open = (snd_pcm_open_fn)dlsym(asound, "snd_pcm_open");
    snd_pcm_set_params_fn fn_params = (snd_pcm_set_params_fn)dlsym(asound, "snd_pcm_set_params");
    snd_pcm_writei_fn fn_writei = (snd_pcm_writei_fn)dlsym(asound, "snd_pcm_writei");
    snd_pcm_recover_fn fn_recover = (snd_pcm_recover_fn)dlsym(asound, "snd_pcm_recover");
    snd_pcm_close_fn fn_close = (snd_pcm_close_fn)dlsym(asound, "snd_pcm_close");

    void *pcm = NULL;
    if (!fn_open || !fn_params || !fn_writei || !fn_close) {
        dlclose(asound);
        return NULL;
    }

    if (fn_open(&pcm, "default", 0, 0) < 0) {
        if (fn_open(&pcm, "plughw:0,0", 0, 0) < 0) {
            dlclose(asound);
            return NULL;
        }
    }
    fn_params(pcm, 2, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);

    int chunk_frames = 512;
    int chunk_bytes = chunk_frames * AUDIO_CHANNELS * 2;
    int16_t buf[512 * AUDIO_CHANNELS];

    while (g_alsa_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        long written = fn_writei(pcm, buf, chunk_frames);
        if (written < 0 && fn_recover) {
            fn_recover(pcm, (int)written, 1);
        }
    }

    fn_close(pcm);
    dlclose(asound);
    return NULL;
}

static int read_sysfs_int(const char *path, int default_val) {
    FILE *f = fopen(path, "r");
    if (!f) return default_val;
    char buf[64];
    int val = default_val;
    if (fgets(buf, sizeof(buf), f)) {
        sscanf(buf, "%d", &val);
    }
    fclose(f);
    return val;
}

static void write_sysfs_int(const char *path, int val) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", val);
    fclose(f);
}

static int g_saved_lcdoff = -1;

/*
 * RG DS Plus stock firmware volume percentage table from /mnt/vendor/bin/portsCtrl.dge (.rodata 0x5af4).
 * Maps /sys/class/anbernic_misc/openbor_volume (0..10) to PulseAudio sink volume percentage (0..100%).
 */
static const int k_rgds_vol_pct[11] = {
    0, 22, 36, 44, 50, 56, 62, 70, 80, 90, 100
};

static void platform_set_volume(int vol) {
    if (vol < MIN_VOLUME) vol = MIN_VOLUME;
    if (vol > MAX_VOLUME) vol = MAX_VOLUME;
    g_volume = vol;

    /* Persist volume step in /sys/class/anbernic_misc/openbor_volume (0..10) */
    write_sysfs_int("/sys/class/anbernic_misc/openbor_volume", vol);

    /* Apply PulseAudio sink volume via /mnt/vendor/ctrl/setPulseaudio.sh (spk & hp sinks) */
    if (access("/mnt/vendor/ctrl/setPulseaudio.sh", X_OK) == 0) {
        int pct = k_rgds_vol_pct[vol];
        int spk_state = read_sysfs_int("/sys/class/anbernic_misc/spk_state", 0);
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

static void rgds_plus_step_brightness(int delta) {
    int cur = read_sysfs_int("/sys/class/anbernic_misc/brightness", 5);
    int next = cur + delta;
    if (next < 1) next = 1;
    if (next > 10) next = 10;
    if (next != cur) {
        write_sysfs_int("/sys/class/anbernic_misc/brightness", next);
        if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
            int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1 &");
            (void)r;
        }
    }
}

/* Ensure both 1024x768 screens on RG DS Plus are powered on and subscreen.dge yields Display 1 */
static void rgds_plus_prepare_dual_screens(void) {
    /* Ensure Wayland environment variables are set for RG DS Plus Weston compositor */
    if (!getenv("WAYLAND_DISPLAY")) {
        if (access("/var/run/0-runtime-dir/wayland-0", F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-0", 0);
            setenv("XDG_RUNTIME_DIR", "/var/run/0-runtime-dir", 0);
        } else if (access("/var/run/wayland-0", F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-0", 0);
            setenv("XDG_RUNTIME_DIR", "/var/run", 0);
        } else if (access("/run/user/0/wayland-1", F_OK) == 0) {
            setenv("WAYLAND_DISPLAY", "wayland-1", 0);
            setenv("XDG_RUNTIME_DIR", "/run/user/0", 0);
        }
    }

    /*
     * Stop subscreen.dge so our Lower Screen window owns Display 1, then restore
     * /sys/class/anbernic_misc/runapp = 1 (subscreen/launch.sh is not a loop, so
     * subscreen.dge will not respawn, while system daemons know an app is running).
     */
    write_sysfs_int("/sys/class/anbernic_misc/runapp", 0);
    int r1 = system("killall -9 subscreen.dge 2>/dev/null");
    (void)r1;
    write_sysfs_int("/sys/class/anbernic_misc/runapp", 1);

    /* Wake both LCDs if the user had single-screen mode (lcdoff != 0) active */
    g_saved_lcdoff = read_sysfs_int("/sys/class/anbernic_misc/lcdoff", 0);
    if (g_saved_lcdoff != 0) {
        write_sysfs_int("/sys/class/anbernic_misc/lcdoff", 0);
        if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
            int r2 = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1");
            (void)r2;
        }
    }

    /* Initialize and sync audio volume with /sys/class/anbernic_misc/openbor_volume */
    if (!g_vol_from_config) {
        int hw_vol = read_sysfs_int("/sys/class/anbernic_misc/openbor_volume", DEFAULT_VOLUME);
        if (hw_vol > 0 && hw_vol <= MAX_VOLUME) {
            g_volume = hw_vol;
        }
    }
    platform_set_volume(g_volume);
}

static void rgds_plus_restore_screens(void) {
    write_sysfs_int("/sys/class/anbernic_misc/runapp", 0);
    if (g_saved_lcdoff > 0) {
        write_sysfs_int("/sys/class/anbernic_misc/lcdoff", g_saved_lcdoff);
        if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
            int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1");
            (void)r;
        }
    }
}

static void open_evdev_devices(void) {
    g_num_evdev_fds = 0;
    for (int i = 0; i < 16 && g_num_evdev_fds < MAX_EVDEV_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_evdev_fds[g_num_evdev_fds++] = fd;
        }
    }
}

static bool open_fb_device(const char *path, FbDevice *fb) {
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) return false;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) < 0) {
        close(fb->fd);
        fb->fd = -1;
        return false;
    }
    fb->map_size = fb->finfo.smem_len ? fb->finfo.smem_len : (fb->vinfo.yres_virtual * fb->finfo.line_length);
    fb->mem = (uint8_t *)mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        fb->mem = NULL;
        close(fb->fd);
        fb->fd = -1;
        return false;
    }
    return true;
}

static void close_fb_device(FbDevice *fb) {
    if (fb->mem) {
        munmap(fb->mem, fb->map_size);
        fb->mem = NULL;
    }
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

/* Blit a 1024x768 ARGB8888 buffer into a Linux framebuffer at vertical offset y_dst_offset */
static void blit_to_fb(FbDevice *fb, const uint32_t *src_1024x768, int y_dst_offset, int target_h) {
    if (!fb->mem || fb->fd < 0) return;
    int fb_w = (int)fb->vinfo.xres;
    int fb_h = (int)fb->vinfo.yres;
    int bpp  = (int)fb->vinfo.bits_per_pixel;
    int stride = (int)fb->finfo.line_length;
    uint8_t *base = fb->mem + fb->vinfo.yoffset * stride;

    if (y_dst_offset >= fb_h) return;
    int avail_h = fb_h - y_dst_offset;
    if (target_h > avail_h) target_h = avail_h;

    for (int y = 0; y < target_h; y++) {
        int src_y = (y * NATIVE_H) / target_h;
        const uint32_t *src_row = src_1024x768 + src_y * NATIVE_W;
        uint8_t *dst_line = base + (y_dst_offset + y) * stride;

        if (bpp == 32) {
            uint32_t *dst = (uint32_t *)dst_line;
            if (fb_w == NATIVE_W) {
                memcpy(dst, src_row, NATIVE_W * 4);
            } else {
                for (int x = 0; x < fb_w; x++) {
                    dst[x] = src_row[(x * NATIVE_W) / fb_w];
                }
            }
        } else if (bpp == 16) {
            uint16_t *dst = (uint16_t *)dst_line;
            for (int x = 0; x < fb_w; x++) {
                uint32_t c = src_row[(x * NATIVE_W) / fb_w];
                uint16_t r = (uint16_t)((c >> 19) & 0x1F);
                uint16_t g = (uint16_t)((c >> 10) & 0x3F);
                uint16_t b = (uint16_t)((c >> 3)  & 0x1F);
                dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->vinfo);
}

static bool platform_init(void) {
    init_scale_lut();
    rgds_plus_prepare_dual_screens();
    open_evdev_devices();

    /* Also check if a secondary framebuffer /dev/fb1 exists */
    open_fb_device("/dev/fb1", &g_fb1);

    g_sdl2_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (g_sdl2_lib) {
        fn_SDL_Init                = dlsym(g_sdl2_lib, "SDL_Init");
        fn_SDL_Quit                = dlsym(g_sdl2_lib, "SDL_Quit");
        fn_SDL_SetHint             = dlsym(g_sdl2_lib, "SDL_SetHint");
        fn_SDL_GetNumVideoDisplays = dlsym(g_sdl2_lib, "SDL_GetNumVideoDisplays");
        fn_SDL_GetDisplayBounds    = dlsym(g_sdl2_lib, "SDL_GetDisplayBounds");
        fn_SDL_CreateWindow        = dlsym(g_sdl2_lib, "SDL_CreateWindow");
        fn_SDL_DestroyWindow       = dlsym(g_sdl2_lib, "SDL_DestroyWindow");
        fn_SDL_CreateRenderer      = dlsym(g_sdl2_lib, "SDL_CreateRenderer");
        fn_SDL_DestroyRenderer     = dlsym(g_sdl2_lib, "SDL_DestroyRenderer");
        fn_SDL_CreateTexture       = dlsym(g_sdl2_lib, "SDL_CreateTexture");
        fn_SDL_DestroyTexture      = dlsym(g_sdl2_lib, "SDL_DestroyTexture");
        fn_SDL_UpdateTexture       = dlsym(g_sdl2_lib, "SDL_UpdateTexture");
        fn_SDL_SetRenderDrawColor  = dlsym(g_sdl2_lib, "SDL_SetRenderDrawColor");
        fn_SDL_RenderClear         = dlsym(g_sdl2_lib, "SDL_RenderClear");
        fn_SDL_RenderCopy          = dlsym(g_sdl2_lib, "SDL_RenderCopy");
        fn_SDL_RenderPresent       = dlsym(g_sdl2_lib, "SDL_RenderPresent");
        fn_SDL_PollEvent           = dlsym(g_sdl2_lib, "SDL_PollEvent");
        fn_SDL_OpenAudio           = dlsym(g_sdl2_lib, "SDL_OpenAudio");
        fn_SDL_PauseAudio          = dlsym(g_sdl2_lib, "SDL_PauseAudio");
        fn_SDL_CloseAudio          = dlsym(g_sdl2_lib, "SDL_CloseAudio");
        fn_SDL_ShowCursor          = dlsym(g_sdl2_lib, "SDL_ShowCursor");

        if (fn_SDL_Init && fn_SDL_CreateWindow && fn_SDL_CreateRenderer && fn_SDL_CreateTexture) {
            if (fn_SDL_SetHint) {
                fn_SDL_SetHint("SDL_TOUCH_MOUSE_EVENTS", "0");
                fn_SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "0");
            }
            if (fn_SDL_Init(SDL2_INIT_VIDEO | SDL2_INIT_AUDIO) == 0 ||
                fn_SDL_Init(SDL2_INIT_VIDEO) == 0) {
                if (fn_SDL_ShowCursor) fn_SDL_ShowCursor(0);

                int num_displays = fn_SDL_GetNumVideoDisplays ? fn_SDL_GetNumVideoDisplays() : 1;
                SDL2_Rect bounds0 = { 0, 0, NATIVE_W, NATIVE_H };
                if (fn_SDL_GetDisplayBounds) {
                    fn_SDL_GetDisplayBounds(0, &bounds0);
                }

                /* Check if user swapped Upper and Lower screens via /sys/class/anbernic_misc/lcdswap */
                int lcdswap = read_sysfs_int("/sys/class/anbernic_misc/lcdswap", 0);
                int top_disp_idx = (lcdswap == 1 && num_displays >= 2) ? 1 : 0;
                int bot_disp_idx = (lcdswap == 1 && num_displays >= 2) ? 0 : 1;

                if (num_displays >= 2) {
                    /*
                     * RG DS Plus Dual-Display Weston/SDL2 Mode:
                     * Display 0 (MIPI-1, 1024x768) and Display 1 (MIPI-2, 1024x768).
                     * Create one fullscreen window per display using SDL_WINDOWPOS_CENTERED_DISPLAY(i)
                     * and bounds (1024x768), matching mod_set/graphic.py and subscreen.dge.
                     */
                    SDL2_Rect bounds_top = { 0, 0, NATIVE_W, NATIVE_H };
                    SDL2_Rect bounds_bot = { 0, NATIVE_H, NATIVE_W, NATIVE_H };
                    if (fn_SDL_GetDisplayBounds) {
                        fn_SDL_GetDisplayBounds(top_disp_idx, &bounds_top);
                        fn_SDL_GetDisplayBounds(bot_disp_idx, &bounds_bot);
                    }

                    g_sdl2_win_top = fn_SDL_CreateWindow(
                        "MIPI-1",
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(top_disp_idx),
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(top_disp_idx),
                        bounds_top.w > 0 ? bounds_top.w : NATIVE_W,
                        bounds_top.h > 0 ? bounds_top.h : NATIVE_H,
                        SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
                    );
                    if (!g_sdl2_win_top) {
                        g_sdl2_win_top = fn_SDL_CreateWindow(
                            "MIPI-1",
                            bounds_top.x, bounds_top.y,
                            bounds_top.w > 0 ? bounds_top.w : NATIVE_W,
                            bounds_top.h > 0 ? bounds_top.h : NATIVE_H,
                            SDL2_RGDS_WIN_FLAGS
                        );
                    }

                    g_sdl2_win_bot = fn_SDL_CreateWindow(
                        "MIPI-2",
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(bot_disp_idx),
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(bot_disp_idx),
                        bounds_bot.w > 0 ? bounds_bot.w : NATIVE_W,
                        bounds_bot.h > 0 ? bounds_bot.h : NATIVE_H,
                        SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
                    );
                    if (!g_sdl2_win_bot) {
                        g_sdl2_win_bot = fn_SDL_CreateWindow(
                            "MIPI-2",
                            bounds_bot.x, bounds_bot.y,
                            bounds_bot.w > 0 ? bounds_bot.w : NATIVE_W,
                            bounds_bot.h > 0 ? bounds_bot.h : NATIVE_H,
                            SDL2_RGDS_WIN_FLAGS
                        );
                    }

                    if (g_sdl2_win_top) {
                        g_sdl2_ren_top = fn_SDL_CreateRenderer(g_sdl2_win_top, -1, SDL2_RENDERER_ACCELERATED | SDL2_RENDERER_PRESENTVSYNC);
                        if (!g_sdl2_ren_top) g_sdl2_ren_top = fn_SDL_CreateRenderer(g_sdl2_win_top, -1, SDL2_RENDERER_SOFTWARE);
                        if (g_sdl2_ren_top) {
                            g_sdl2_tex_top = fn_SDL_CreateTexture(g_sdl2_ren_top, SDL2_PIXELFORMAT_ARGB8888, SDL2_TEXTUREACCESS_STREAMING, NATIVE_W, NATIVE_H);
                        }
                    }
                    if (g_sdl2_win_bot) {
                        g_sdl2_ren_bot = fn_SDL_CreateRenderer(g_sdl2_win_bot, -1, SDL2_RENDERER_ACCELERATED);
                        if (!g_sdl2_ren_bot) g_sdl2_ren_bot = fn_SDL_CreateRenderer(g_sdl2_win_bot, -1, SDL2_RENDERER_SOFTWARE);
                        if (g_sdl2_ren_bot) {
                            g_sdl2_tex_bot = fn_SDL_CreateTexture(g_sdl2_ren_bot, SDL2_PIXELFORMAT_ARGB8888, SDL2_TEXTUREACCESS_STREAMING, NATIVE_W, NATIVE_H);
                        }
                    }

                    if (g_sdl2_tex_top) {
                        g_use_sdl2_video = true;
                        g_sdl2_dual_windows = (g_sdl2_tex_bot != NULL);
                    }
                } else {
                    /* Single SDL2 display: check if Weston/DRM stacks both 1024x768 panels vertically (1024x1536) */
                    bool is_stacked = (bounds0.h >= bounds0.w);
                    int tex_h = is_stacked ? (NATIVE_H * 2) : NATIVE_H;

                    g_sdl2_win_top = fn_SDL_CreateWindow(
                        "RG DS Plus Metronome",
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(0),
                        SDL2_WINDOWPOS_CENTERED_DISPLAY(0),
                        bounds0.w > 0 ? bounds0.w : NATIVE_W,
                        bounds0.h > 0 ? bounds0.h : NATIVE_H,
                        SDL2_WINDOW_FULLSCREEN_DESKTOP | SDL2_WINDOW_SHOWN
                    );
                    if (g_sdl2_win_top) {
                        g_sdl2_ren_top = fn_SDL_CreateRenderer(g_sdl2_win_top, -1, SDL2_RENDERER_ACCELERATED | SDL2_RENDERER_PRESENTVSYNC);
                        if (!g_sdl2_ren_top) g_sdl2_ren_top = fn_SDL_CreateRenderer(g_sdl2_win_top, -1, SDL2_RENDERER_SOFTWARE);
                        if (g_sdl2_ren_top) {
                            g_sdl2_tex_top = fn_SDL_CreateTexture(g_sdl2_ren_top, SDL2_PIXELFORMAT_ARGB8888, SDL2_TEXTUREACCESS_STREAMING, NATIVE_W, tex_h);
                            if (g_sdl2_tex_top) {
                                g_use_sdl2_video = true;
                                g_sdl2_stacked_single = is_stacked;
                            }
                        }
                    }
                }

                if (fn_SDL_OpenAudio && fn_SDL_PauseAudio) {
                    SDL2_AudioSpec desired;
                    memset(&desired, 0, sizeof(desired));
                    desired.freq = SAMPLE_RATE;
                    desired.format = SDL2_AUDIO_S16LSB;
                    desired.channels = AUDIO_CHANNELS;
                    desired.samples = 512;
                    desired.callback = audio_callback;
                    if (fn_SDL_OpenAudio(&desired, NULL) == 0) {
                        fn_SDL_PauseAudio(0);
                        g_use_sdl2_audio = true;
                    }
                }
            }
        }
    }

    if (!g_use_sdl2_video) {
        if (!open_fb_device("/dev/fb0", &g_fb0)) {
            fprintf(stderr, "Failed to initialize both SDL2 video and /dev/fb0\n");
            return false;
        }
    }

    if (!g_use_sdl2_audio) {
        g_alsa_running = true;
        pthread_create(&g_alsa_thread, NULL, alsa_audio_worker, NULL);
    }

    return true;
}

static void platform_present_dual(const uint32_t *upper_canvas, const uint32_t *lower_canvas) {
    scale_canvas_to_native(upper_canvas, g_scaled_upper);
    scale_canvas_to_native(lower_canvas, g_scaled_lower);

    if (g_use_sdl2_video) {
        if (g_sdl2_dual_windows) {
            /* Render Upper Screen (1024x768) to Display 0 and Lower Screen (1024x768) to Display 1 */
            fn_SDL_UpdateTexture(g_sdl2_tex_top, NULL, g_scaled_upper, NATIVE_W * 4);
            if (fn_SDL_SetRenderDrawColor) fn_SDL_SetRenderDrawColor(g_sdl2_ren_top, 0, 0, 0, 255);
            fn_SDL_RenderClear(g_sdl2_ren_top);
            fn_SDL_RenderCopy(g_sdl2_ren_top, g_sdl2_tex_top, NULL, NULL);
            fn_SDL_RenderPresent(g_sdl2_ren_top);

            fn_SDL_UpdateTexture(g_sdl2_tex_bot, NULL, g_scaled_lower, NATIVE_W * 4);
            if (fn_SDL_SetRenderDrawColor) fn_SDL_SetRenderDrawColor(g_sdl2_ren_bot, 0, 0, 0, 255);
            fn_SDL_RenderClear(g_sdl2_ren_bot);
            fn_SDL_RenderCopy(g_sdl2_ren_bot, g_sdl2_tex_bot, NULL, NULL);
            fn_SDL_RenderPresent(g_sdl2_ren_bot);
        } else if (g_sdl2_stacked_single) {
            /* Combined 1024x1536 stacked texture: Upper on top half, Lower on bottom half */
            memcpy(g_scaled_stacked, g_scaled_upper, NATIVE_W * NATIVE_H * 4);
            memcpy(g_scaled_stacked + NATIVE_W * NATIVE_H, g_scaled_lower, NATIVE_W * NATIVE_H * 4);
            fn_SDL_UpdateTexture(g_sdl2_tex_top, NULL, g_scaled_stacked, NATIVE_W * 4);
            fn_SDL_RenderClear(g_sdl2_ren_top);
            fn_SDL_RenderCopy(g_sdl2_ren_top, g_sdl2_tex_top, NULL, NULL);
            fn_SDL_RenderPresent(g_sdl2_ren_top);
        } else {
            /* SDL2 drives Upper Screen on Display 0; also push Lower Screen to /dev/fb1 if present */
            fn_SDL_UpdateTexture(g_sdl2_tex_top, NULL, g_scaled_upper, NATIVE_W * 4);
            fn_SDL_RenderClear(g_sdl2_ren_top);
            fn_SDL_RenderCopy(g_sdl2_ren_top, g_sdl2_tex_top, NULL, NULL);
            fn_SDL_RenderPresent(g_sdl2_ren_top);

            if (g_fb1.mem) {
                blit_to_fb(&g_fb1, g_scaled_lower, 0, (int)g_fb1.vinfo.yres);
            }
        }
    } else if (g_fb0.mem) {
        /* Pure framebuffer mode: check if /dev/fb0 is vertically stacked or paired with /dev/fb1 */
        if (g_fb0.vinfo.yres >= g_fb0.vinfo.xres) {
            int half_h = (int)g_fb0.vinfo.yres / 2;
            blit_to_fb(&g_fb0, g_scaled_upper, 0, half_h);
            blit_to_fb(&g_fb0, g_scaled_lower, half_h, half_h);
        } else {
            blit_to_fb(&g_fb0, g_scaled_upper, 0, (int)g_fb0.vinfo.yres);
            if (g_fb1.mem) {
                blit_to_fb(&g_fb1, g_scaled_lower, 0, (int)g_fb1.vinfo.yres);
            }
        }
    }
}

static InputAction platform_poll_input(void) {
    if (g_use_sdl2_video && fn_SDL_PollEvent) {
        SDL2_Event ev;
        while (fn_SDL_PollEvent(&ev)) {
            if (ev.type == SDL2_QUIT) {
                return ACT_QUIT;
            }
        }
    }

    /*
     * ANBERNIC-rk3568-keys (/dev/input/event*) keycode mapping from RG DS Plus firmware:
     *   304 (0x130 / BTN_SOUTH)   = A Button
     *   305 (0x131 / BTN_EAST)    = B Button
     *   306 (0x132 / BTN_C)       = Y Button
     *   307 (0x133 / BTN_NORTH)   = X Button
     *   308 (0x134 / BTN_WEST)    = L1 Shoulder
     *   309 (0x135 / BTN_Z)       = R1 Shoulder
     *   310 (0x136 / BTN_TL)      = SELECT
     *   311 (0x137 / BTN_TR)      = START
     *   312 (0x138 / BTN_TR2)     = MENU / Function
     *   314 (0x13a / BTN_SELECT)  = L2 Trigger
     *   315 (0x13b / BTN_START)   = R2 Trigger
     *   316 (0x13c / BTN_MODE)    = MENU
     *   114 (0x72  / KEY_VOLUMEDOWN) = Physical Volume Down Button
     *   115 (0x73  / KEY_VOLUMEUP)   = Physical Volume Up Button
     *   ABS_HAT0X (16) / ABS_HAT0Y (17) and KEY_UP/DOWN/LEFT/RIGHT = D-Pad
     */
    for (int i = 0; i < g_num_evdev_fds; i++) {
        struct input_event ie;
        while (read(g_evdev_fds[i], &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (ie.type == EV_KEY) {
                bool pressed = (ie.value == 1);
                if (ie.code == 310 || ie.code == KEY_ESC) {
                    g_btn_select_held = (ie.value != 0);
                    if (pressed && g_btn_start_held) return ACT_QUIT;
                }
                if (ie.code == 311 || ie.code == KEY_ENTER) {
                    g_btn_start_held = (ie.value != 0);
                    if (pressed && g_btn_select_held) return ACT_QUIT;
                }

                /* Physical Volume Up (115) & Volume Down (114) on ANBERNIC-rk3568-keys */
                if (ie.code == KEY_VOLUMEUP) {
                    if (ie.value == 1) {
                        if (g_btn_select_held || g_btn_start_held) {
                            rgds_plus_step_brightness(+1);
                        } else if (g_vol_dir != 1) {
                            g_vol_dir = 1;
                            g_vol_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_VOL_UP;
                        }
                    } else if (ie.value == 0 && g_vol_dir == 1) {
                        g_vol_dir = 0;
                    }
                    continue;
                }
                if (ie.code == KEY_VOLUMEDOWN) {
                    if (ie.value == 1) {
                        if (g_btn_select_held || g_btn_start_held) {
                            rgds_plus_step_brightness(-1);
                        } else if (g_vol_dir != -1) {
                            g_vol_dir = -1;
                            g_vol_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_VOL_DOWN;
                        }
                    } else if (ie.value == 0 && g_vol_dir == -1) {
                        g_vol_dir = 0;
                    }
                    continue;
                }

                /* Handle D-Pad UP/DOWN key press AND release for continuous auto-repeat */
                if (ie.code == BTN_DPAD_UP || ie.code == KEY_UP) {
                    if (ie.value != 0) {
                        if (g_dpad_vert_dir != -1) {
                            g_dpad_vert_dir = -1;
                            g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_BPM_UP;
                        }
                    } else if (g_dpad_vert_dir == -1) {
                        g_dpad_vert_dir = 0;
                    }
                    continue;
                }
                if (ie.code == BTN_DPAD_DOWN || ie.code == KEY_DOWN) {
                    if (ie.value != 0) {
                        if (g_dpad_vert_dir != 1) {
                            g_dpad_vert_dir = 1;
                            g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_BPM_DOWN;
                        }
                    } else if (g_dpad_vert_dir == 1) {
                        g_dpad_vert_dir = 0;
                    }
                    continue;
                }

                if (!pressed) continue;

                switch (ie.code) {
                    /* A Button (304 on ANBERNIC-rk3568-keys) -> Play / Pause */
                    case BTN_SOUTH:  /* 304 = A on RG DS Plus */
                    case KEY_SPACE:
                    case KEY_LEFTALT:
                        return ACT_PLAY_PAUSE;

                    /* B Button (305 on ANBERNIC-rk3568-keys) -> Reset Current Session Stats (or Pause if playing) */
                    case BTN_EAST:   /* 305 = B on RG DS Plus */
                        return g_playing ? ACT_PLAY_PAUSE : ACT_RESET_SESSION;

                    /* D-Pad LEFT / RIGHT -> Cycle Time Signature */
                    case BTN_DPAD_LEFT:
                    case KEY_LEFT:
                        return ACT_SIG_PREV;
                    case BTN_DPAD_RIGHT:
                    case KEY_RIGHT:
                        return ACT_SIG_NEXT;

                    /* L1 (308) / L2 (314) & R1 (309) / R2 (315) Shoulder Buttons -> Tempo -5 / +5 BPM */
                    case BTN_WEST:   /* 308 = L1 on RG DS Plus */
                    case BTN_SELECT: /* 314 = L2 on RG DS Plus */
                    case KEY_TAB:
                        return ACT_BPM_DOWN_FAST;
                    case BTN_Z:      /* 309 = R1 on RG DS Plus */
                    case BTN_START:  /* 315 = R2 on RG DS Plus */
                    case KEY_BACKSPACE:
                        return ACT_BPM_UP_FAST;

                    /* SELECT (310) / MENU (312, 316) -> Save & Exit (matching Clock/Settings apps) */
                    case BTN_TL:     /* 310 = SELECT on RG DS Plus */
                    case BTN_TL2:    /* 312 = MENUF on RG DS Plus */
                    case BTN_MODE:   /* 316 */
                    case KEY_ESC:
                        return ACT_QUIT;

                    default:
                        break;
                }
            } else if (ie.type == EV_ABS) {
                /* D-Pad on ANBERNIC-rk3568-keys emits ABS_HAT0X (16) and ABS_HAT0Y (17) */
                if (ie.code == ABS_HAT0X) {
                    int prev = g_hat_x;
                    g_hat_x = ie.value;
                    if (g_hat_x < 0 && prev >= 0) return ACT_SIG_PREV;
                    if (g_hat_x > 0 && prev <= 0) return ACT_SIG_NEXT;
                } else if (ie.code == ABS_HAT0Y) {
                    int prev = g_hat_y;
                    g_hat_y = ie.value;
                    if (g_hat_y < 0) {
                        g_dpad_vert_dir = -1;
                        if (prev >= 0) {
                            g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_BPM_UP;
                        }
                    } else if (g_hat_y > 0) {
                        g_dpad_vert_dir = 1;
                        if (prev <= 0) {
                            g_dpad_next_repeat_ms = get_ticks_ms() + REPEAT_INITIAL_DELAY_MS;
                            return ACT_BPM_DOWN;
                        }
                    } else {
                        g_dpad_vert_dir = 0;
                    }
                }
            }
        }
    }

    /* Fire continuous tempo change while D-Pad UP or DOWN is held */
    if (g_dpad_vert_dir != 0) {
        uint32_t now = get_ticks_ms();
        if ((int32_t)(now - g_dpad_next_repeat_ms) >= 0) {
            g_dpad_next_repeat_ms = now + REPEAT_INTERVAL_MS;
            return (g_dpad_vert_dir < 0) ? ACT_BPM_UP : ACT_BPM_DOWN;
        }
    }

    /* Fire continuous volume change while Volume UP or DOWN is held */
    if (g_vol_dir != 0) {
        uint32_t now = get_ticks_ms();
        if ((int32_t)(now - g_vol_next_repeat_ms) >= 0) {
            g_vol_next_repeat_ms = now + VOL_REPEAT_INTERVAL_MS;
            return (g_vol_dir > 0) ? ACT_VOL_UP : ACT_VOL_DOWN;
        }
    }
    return ACT_NONE;
}

static void platform_shutdown(void) {
    if (g_use_sdl2_audio && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
    }
    if (g_alsa_running) {
        g_alsa_running = false;
        pthread_join(g_alsa_thread, NULL);
    }
    if (g_use_sdl2_video) {
        if (g_sdl2_tex_bot && fn_SDL_DestroyTexture) fn_SDL_DestroyTexture(g_sdl2_tex_bot);
        if (g_sdl2_ren_bot && fn_SDL_DestroyRenderer) fn_SDL_DestroyRenderer(g_sdl2_ren_bot);
        if (g_sdl2_win_bot && fn_SDL_DestroyWindow) fn_SDL_DestroyWindow(g_sdl2_win_bot);

        if (g_sdl2_tex_top && fn_SDL_DestroyTexture) fn_SDL_DestroyTexture(g_sdl2_tex_top);
        if (g_sdl2_ren_top && fn_SDL_DestroyRenderer) fn_SDL_DestroyRenderer(g_sdl2_ren_top);
        if (g_sdl2_win_top && fn_SDL_DestroyWindow) fn_SDL_DestroyWindow(g_sdl2_win_top);
        if (fn_SDL_Quit) fn_SDL_Quit();
    }
    if (g_sdl2_lib) {
        dlclose(g_sdl2_lib);
        g_sdl2_lib = NULL;
    }
    close_fb_device(&g_fb0);
    close_fb_device(&g_fb1);
    for (int i = 0; i < g_num_evdev_fds; i++) {
        close(g_evdev_fds[i]);
    }
    g_num_evdev_fds = 0;
    rgds_plus_restore_screens();
}

#endif /* HOST_X11 vs Anbernic RG DS Plus */

/* =========================================================================
 * Built-in 5x7 ASCII Bitmap Font & Canvas Drawing Primitives
 * ========================================================================= */
static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 32 ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 33 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 34 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 35 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 36 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 37 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 38 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 39 ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 40 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 41 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* 42 '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 43 '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 44 ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 45 '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 46 '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 47 '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 48 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 49 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 50 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 51 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 52 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 53 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 54 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 55 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 56 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 57 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 58 ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 59 ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 60 '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 61 '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 62 '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 63 '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 64 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 65 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 66 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 67 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 68 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 69 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 70 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 71 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 72 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 73 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 74 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 75 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 76 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 77 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 78 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 79 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 80 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 81 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 82 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 83 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 84 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 85 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 86 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 87 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 88 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 89 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 90 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 91 '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 92 '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 93 ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 94 '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 95 '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 96 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 97 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 98 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 99 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 100 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 101 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 102 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 103 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 104 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 105 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 106 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 107 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 108 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 109 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 110 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 111 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 112 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 113 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 114 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 115 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 116 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 117 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 118 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 119 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 120 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 121 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 122 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 123 '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 124 '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 125 '}' */
    {0x08, 0x04, 0x08, 0x10, 0x08}  /* 126 '~' */
};

/* =========================================================================
 * Overall & Session Practice Statistics State (Displayed on Lower Screen)
 * ========================================================================= */
static char g_config_path[512];

/* Session stats (reset when app launches or when B/C is pressed while paused) */
static uint64_t g_session_ms = 0;
static uint64_t g_session_beats = 0;
static uint64_t g_session_bars = 0;

/* Lifetime persisted stats */
static uint64_t g_total_ms = 0;
static uint64_t g_total_beats = 0;
static uint64_t g_total_bars = 0;
static uint64_t g_total_sessions = 0;
static int      g_min_bpm_used = 0;
static int      g_max_bpm_used = 0;
static uint64_t g_bpm_weighted_sum = 0; /* sum of (bpm * beats) for average BPM */
static uint64_t g_sig_ms[NUM_TIME_SIGS] = {0};
static uint64_t g_sig_bars[NUM_TIME_SIGS] = {0};

static const char *get_tempo_marking(int bpm) {
    if (bpm < 45)  return "GRAVE";
    if (bpm < 60)  return "LARGO";
    if (bpm < 66)  return "LARGHETTO";
    if (bpm < 76)  return "ADAGIO";
    if (bpm < 108) return "ANDANTE";
    if (bpm < 120) return "MODERATO";
    if (bpm < 156) return "ALLEGRO";
    if (bpm < 176) return "VIVACE";
    if (bpm < 200) return "PRESTO";
    return "PRESTISSIMO";
}

static void resolve_config_path(void) {
    struct stat st;
    if (stat("/mnt/sdcard", &st) == 0 && S_ISDIR(st.st_mode) && access("/mnt/sdcard", W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "/mnt/sdcard/.metronome_rgds.cfg");
    } else if (stat("/mnt/mmc", &st) == 0 && S_ISDIR(st.st_mode) && access("/mnt/mmc", W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "/mnt/mmc/.metronome_rgds.cfg");
    } else if (stat("/userdata/system", &st) == 0 && S_ISDIR(st.st_mode) && access("/userdata/system", W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "/userdata/system/.metronome_rgds.cfg");
    } else {
        const char *home = getenv("HOME");
        if (home && access(home, W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.metronome_rgds.cfg", home);
        } else {
            snprintf(g_config_path, sizeof(g_config_path), "./.metronome_rgds.cfg");
        }
    }
}

static uint64_t parse_u64(const char *s) {
    uint64_t val = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10ULL + (uint64_t)(*s - '0');
        s++;
    }
    return val;
}

static void load_config(void) {
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "bpm=", 4) == 0) {
            int val = (int)parse_u64(line + 4);
            if (val >= MIN_BPM && val <= MAX_BPM) g_bpm = val;
        } else if (strncmp(line, "time_sig_idx=", 13) == 0) {
            int val = (int)parse_u64(line + 13);
            if (val >= 0 && val < NUM_TIME_SIGS) g_time_sig_idx = val;
        } else if (strncmp(line, "volume=", 7) == 0) {
            int val = (int)parse_u64(line + 7);
            if (val >= MIN_VOLUME && val <= MAX_VOLUME) {
                g_volume = val;
                g_vol_from_config = true;
            }
        } else if (strncmp(line, "total_ms=", 9) == 0) {
            g_total_ms = parse_u64(line + 9);
        } else if (strncmp(line, "total_beats=", 12) == 0) {
            g_total_beats = parse_u64(line + 12);
        } else if (strncmp(line, "total_bars=", 11) == 0) {
            g_total_bars = parse_u64(line + 11);
        } else if (strncmp(line, "total_sessions=", 15) == 0) {
            g_total_sessions = parse_u64(line + 15);
        } else if (strncmp(line, "min_bpm=", 8) == 0) {
            g_min_bpm_used = (int)parse_u64(line + 8);
        } else if (strncmp(line, "max_bpm=", 8) == 0) {
            g_max_bpm_used = (int)parse_u64(line + 8);
        } else if (strncmp(line, "bpm_weighted_sum=", 17) == 0) {
            g_bpm_weighted_sum = parse_u64(line + 17);
        } else if (strncmp(line, "sig_ms_", 7) == 0 && line[7] >= '0' && line[7] < ('0' + NUM_TIME_SIGS) && line[8] == '=') {
            g_sig_ms[line[7] - '0'] = parse_u64(line + 9);
        } else if (strncmp(line, "sig_bars_", 9) == 0 && line[9] >= '0' && line[9] < ('0' + NUM_TIME_SIGS) && line[10] == '=') {
            g_sig_bars[line[9] - '0'] = parse_u64(line + 11);
        }
    }
    fclose(f);
}

static void save_config(void) {
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "bpm=%d\n", g_bpm);
    fprintf(f, "time_sig_idx=%d\n", g_time_sig_idx);
    fprintf(f, "volume=%d\n", g_volume);
    fprintf(f, "total_ms=%llu\n", (unsigned long long)g_total_ms);
    fprintf(f, "total_beats=%llu\n", (unsigned long long)g_total_beats);
    fprintf(f, "total_bars=%llu\n", (unsigned long long)g_total_bars);
    fprintf(f, "total_sessions=%llu\n", (unsigned long long)g_total_sessions);
    fprintf(f, "min_bpm=%d\n", g_min_bpm_used);
    fprintf(f, "max_bpm=%d\n", g_max_bpm_used);
    fprintf(f, "bpm_weighted_sum=%llu\n", (unsigned long long)g_bpm_weighted_sum);
    for (int i = 0; i < NUM_TIME_SIGS; i++) {
        fprintf(f, "sig_ms_%d=%llu\n", i, (unsigned long long)g_sig_ms[i]);
        fprintf(f, "sig_bars_%d=%llu\n", i, (unsigned long long)g_sig_bars[i]);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void format_duration(uint64_t total_ms, char *buf, size_t buf_sz) {
    uint64_t total_sec = total_ms / 1000ULL;
    unsigned int hrs  = (unsigned int)(total_sec / 3600ULL);
    unsigned int mins = (unsigned int)((total_sec % 3600ULL) / 60ULL);
    unsigned int secs = (unsigned int)(total_sec % 60ULL);
    if (hrs > 0) {
        snprintf(buf, buf_sz, "%uh %02um %02us", hrs, mins, secs);
    } else {
        snprintf(buf, buf_sz, "%02um %02us", mins, secs);
    }
}

/* Canvas Drawing Helpers */
static void draw_rect(uint32_t *canvas, int x, int y, int w, int h, uint32_t color) {
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > CANVAS_W) ? CANVAS_W : (x + w);
    int y1 = (y + h > CANVAS_H) ? CANVAS_H : (y + h);
    for (int ry = y0; ry < y1; ry++) {
        uint32_t *row = canvas + ry * CANVAS_W;
        for (int rx = x0; rx < x1; rx++) {
            row[rx] = color;
        }
    }
}

static void draw_border(uint32_t *canvas, int x, int y, int w, int h, int thickness, uint32_t color) {
    draw_rect(canvas, x, y, w, thickness, color);
    draw_rect(canvas, x, y + h - thickness, w, thickness, color);
    draw_rect(canvas, x, y, thickness, h, color);
    draw_rect(canvas, x + w - thickness, y, thickness, h, color);
}

static void draw_char(uint32_t *canvas, int x, int y, char c, int scale, uint32_t color) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                draw_rect(canvas, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(uint32_t *canvas, int x, int y, const char *str, int scale, uint32_t color) {
    int cur_x = x;
    while (*str) {
        draw_char(canvas, cur_x, y, *str, scale, color);
        cur_x += 6 * scale;
        str++;
    }
}

static void draw_text_centered(uint32_t *canvas, int cx, int y, const char *str, int scale, uint32_t color) {
    int len = (int)strlen(str);
    if (len == 0) return;
    int width = len * 6 * scale - scale;
    draw_text(canvas, cx - width / 2, y, str, scale, color);
}

static void record_beat_stat(int bpm, int sig_idx, bool is_bar_start) {
    g_session_beats++;
    g_total_beats++;
    if (is_bar_start) {
        g_session_bars++;
        g_total_bars++;
        g_sig_bars[sig_idx]++;
    }
    if (g_min_bpm_used == 0 || bpm < g_min_bpm_used) g_min_bpm_used = bpm;
    if (bpm > g_max_bpm_used) g_max_bpm_used = bpm;
    g_bpm_weighted_sum += (uint64_t)bpm;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    resolve_config_path();
    load_config();

    if (!platform_init()) {
        return 1;
    }

    uint32_t *upper_canvas = (uint32_t *)calloc(CANVAS_W * CANVAS_H, sizeof(uint32_t));
    uint32_t *lower_canvas = (uint32_t *)calloc(CANVAS_W * CANVAS_H, sizeof(uint32_t));

    /* Color Palette */
    uint32_t col_bg         = make_rgb(14, 18, 28);
    uint32_t col_panel      = make_rgb(26, 33, 50);
    uint32_t col_panel_dark = make_rgb(20, 25, 38);
    uint32_t col_border     = make_rgb(65, 82, 120);
    uint32_t col_white      = make_rgb(245, 247, 252);
    uint32_t col_muted      = make_rgb(145, 160, 190);
    uint32_t col_accent     = make_rgb(255, 70, 70);   /* Beat 1 Accent Red */
    uint32_t col_beat       = make_rgb(46, 213, 115);  /* Normal Beat Green */
    uint32_t col_gold       = make_rgb(255, 200, 60);  /* Time Signature Gold */
    uint32_t col_cyan       = make_rgb(70, 205, 255);  /* Tempo / UI Highlights */
    uint32_t col_purple     = make_rgb(175, 120, 255); /* Lower Screen Stats Accent */
    uint32_t col_box_idle   = make_rgb(36, 44, 66);

    bool running = true;
    bool counted_this_session = false;
    int current_beat = 0;
    double next_beat_time_ms = 0.0;
    uint32_t last_beat_trigger_ms = 0;
    uint32_t last_vol_change_ms = 0;
    uint32_t last_frame_ms = get_ticks_ms();
    uint32_t last_autosave_ms = last_frame_ms;

    while (running) {
        uint32_t now_ms = get_ticks_ms();
        uint32_t dt_ms = now_ms - last_frame_ms;
        last_frame_ms = now_ms;

        if (g_playing && dt_ms > 0 && dt_ms < 1000) {
            g_session_ms += dt_ms;
            g_total_ms += dt_ms;
            g_sig_ms[g_time_sig_idx] += dt_ms;
        }

        /* Periodic autosave every 15 seconds while playing */
        if (g_playing && (now_ms - last_autosave_ms) >= 15000) {
            save_config();
            last_autosave_ms = now_ms;
        }

        InputAction act;
        while ((act = platform_poll_input()) != ACT_NONE) {
            switch (act) {
                case ACT_PLAY_PAUSE:
                    g_playing = !g_playing;
                    if (g_playing) {
                        if (!counted_this_session) {
                            g_total_sessions++;
                            counted_this_session = true;
                        }
                        current_beat = 0;
                        trigger_click(true);
                        record_beat_stat(g_bpm, g_time_sig_idx, true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    } else {
                        g_click_samples_left = 0;
                    }
                    save_config();
                    break;

                case ACT_RESET_SESSION:
                    g_session_ms = 0;
                    g_session_beats = 0;
                    g_session_bars = 0;
                    break;

                case ACT_BPM_UP:
                    if (g_bpm < MAX_BPM) {
                        g_bpm++;
                        save_config();
                    }
                    break;

                case ACT_BPM_DOWN:
                    if (g_bpm > MIN_BPM) {
                        g_bpm--;
                        save_config();
                    }
                    break;

                case ACT_BPM_UP_FAST:
                    g_bpm += 5;
                    if (g_bpm > MAX_BPM) g_bpm = MAX_BPM;
                    save_config();
                    break;

                case ACT_BPM_DOWN_FAST:
                    g_bpm -= 5;
                    if (g_bpm < MIN_BPM) g_bpm = MIN_BPM;
                    save_config();
                    break;

                case ACT_SIG_PREV:
                    g_time_sig_idx = (g_time_sig_idx + NUM_TIME_SIGS - 1) % NUM_TIME_SIGS;
                    current_beat = 0;
                    if (g_playing) {
                        trigger_click(true);
                        record_beat_stat(g_bpm, g_time_sig_idx, true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    }
                    save_config();
                    break;

                case ACT_SIG_NEXT:
                    g_time_sig_idx = (g_time_sig_idx + 1) % NUM_TIME_SIGS;
                    current_beat = 0;
                    if (g_playing) {
                        trigger_click(true);
                        record_beat_stat(g_bpm, g_time_sig_idx, true);
                        last_beat_trigger_ms = now_ms;
                        next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
                    }
                    save_config();
                    break;

                case ACT_VOL_UP:
                    if (g_volume < MAX_VOLUME) {
                        platform_set_volume(g_volume + 1);
                        save_config();
                    }
                    last_vol_change_ms = now_ms;
                    break;

                case ACT_VOL_DOWN:
                    if (g_volume > MIN_VOLUME) {
                        platform_set_volume(g_volume - 1);
                        save_config();
                    }
                    last_vol_change_ms = now_ms;
                    break;

                case ACT_QUIT:
                    running = false;
                    break;

                default:
                    break;
            }
        }

        /* Metronome Beat Scheduler */
        now_ms = get_ticks_ms();
        int beats_per_bar = k_time_sig_beats[g_time_sig_idx];
        double interval_ms = 60000.0 / (double)g_bpm;

        if (g_playing) {
            if ((double)now_ms >= next_beat_time_ms) {
                current_beat = (current_beat + 1) % beats_per_bar;
                bool is_accent = (current_beat == 0);
                trigger_click(is_accent);
                record_beat_stat(g_bpm, g_time_sig_idx, is_accent);
                last_beat_trigger_ms = now_ms;
                next_beat_time_ms += interval_ms;
                if ((double)now_ms - next_beat_time_ms > interval_ms) {
                    next_beat_time_ms = (double)now_ms + interval_ms;
                }
            }
        }

        /* =================================================================
         * UPPER SCREEN: METRONOME DISPLAY (320x240 logical -> 1024x768)
         * ================================================================= */
        draw_rect(upper_canvas, 0, 0, CANVAS_W, CANVAS_H, col_bg);

        /* 1. Top Status Bar (y: 0..26) */
        bool vol_toast = (last_vol_change_ms != 0 && (now_ms - last_vol_change_ms) < 1400);
        uint32_t top_bar_col = g_playing
            ? ((current_beat == 0 && (now_ms - last_beat_trigger_ms) < 120) ? col_accent : col_panel)
            : col_panel_dark;
        draw_rect(upper_canvas, 0, 0, CANVAS_W, 26, top_bar_col);
        draw_rect(upper_canvas, 0, 26, CANVAS_W, 2, col_border);

        if (vol_toast) {
            char vol_banner[40];
            if (g_volume == 0) {
                snprintf(vol_banner, sizeof(vol_banner), "VOLUME: MUTE (0/10)");
            } else {
                snprintf(vol_banner, sizeof(vol_banner), "VOLUME: %d%% (%d/10)", g_volume * 10, g_volume);
            }
            draw_text_centered(upper_canvas, CANVAS_W / 2, 6, vol_banner, 2,
                               (g_volume == 0) ? col_accent : col_cyan);
        } else {
            if (g_playing) {
                draw_text_centered(upper_canvas, 134, 6, "METRONOME PLAYING", 2, col_white);
            } else {
                draw_text_centered(upper_canvas, 134, 6, "METRONOME PAUSED", 2, col_gold);
            }
            char vol_badge[16];
            if (g_volume == 0) {
                snprintf(vol_badge, sizeof(vol_badge), "MUTE");
            } else {
                snprintf(vol_badge, sizeof(vol_badge), "V:%d0%%", g_volume);
            }
            draw_rect(upper_canvas, 272, 4, 42, 18, col_bg);
            draw_border(upper_canvas, 272, 4, 42, 18, 1, (g_volume == 0) ? col_accent : col_border);
            draw_text_centered(upper_canvas, 293, 10, vol_badge, 1,
                               (g_volume == 0) ? col_accent : col_cyan);
        }

        /* 2. BPM Card (Left) & Time Signature Card (Right) */
        draw_rect(upper_canvas, 12, 36, 184, 92, col_panel);
        draw_border(upper_canvas, 12, 36, 184, 92, 2, col_border);

        draw_text_centered(upper_canvas, 104, 43, "TEMPO (UP/DOWN)", 1, col_muted);

        char bpm_str[16];
        snprintf(bpm_str, sizeof(bpm_str), "%d", g_bpm);
        draw_text_centered(upper_canvas, 88, 58, bpm_str, 5, col_white);
        draw_text(upper_canvas, 150, 79, "BPM", 2, col_cyan);

        draw_text_centered(upper_canvas, 104, 108, get_tempo_marking(g_bpm), 2, col_cyan);

        /* Time Signature Card */
        draw_rect(upper_canvas, 204, 36, 104, 92, col_panel);
        draw_border(upper_canvas, 204, 36, 104, 92, 2, col_border);

        draw_text_centered(upper_canvas, 256, 43, "TIME SIG", 1, col_muted);
        draw_text_centered(upper_canvas, 256, 62, k_time_sig_names[g_time_sig_idx], 4, col_gold);
        draw_text_centered(upper_canvas, 256, 108, "< LEFT/RIGHT >", 1, col_muted);

        /* 3. Beat Visualizer Panel */
        draw_rect(upper_canvas, 12, 136, 296, 72, col_panel);
        draw_border(upper_canvas, 12, 136, 296, 72, 2, col_border);

        int margin_x = 22;
        int avail_w = 296 - 2 * margin_x;
        int gap = (beats_per_bar <= 4) ? 10 : 6;
        int box_w = (avail_w - gap * (beats_per_bar - 1)) / beats_per_bar;
        if (box_w > 48) box_w = 48;
        int total_boxes_w = box_w * beats_per_bar + gap * (beats_per_bar - 1);
        int start_x = 12 + (296 - total_boxes_w) / 2;
        int box_y = 146;
        int box_h = 42;

        bool beat_flash = g_playing && ((now_ms - last_beat_trigger_ms) < (uint32_t)(interval_ms * 0.55));

        for (int b = 0; b < beats_per_bar; b++) {
            int bx = start_x + b * (box_w + gap);
            bool is_active = g_playing && (b == current_beat);

            uint32_t fill_col = col_box_idle;
            uint32_t bdr_col  = (b == 0) ? col_accent : col_border;

            if (is_active) {
                fill_col = (b == 0) ? col_accent : col_beat;
                bdr_col  = col_white;
            }

            draw_rect(upper_canvas, bx, box_y, box_w, box_h, fill_col);
            draw_border(upper_canvas, bx, box_y, box_w, box_h, (b == 0) ? 2 : 1, bdr_col);

            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%d", b + 1);
            uint32_t num_col = is_active ? col_bg : ((b == 0) ? col_accent : col_white);
            draw_text_centered(upper_canvas, bx + box_w / 2, box_y + 10, num_str, 2, num_col);

            if (b == 0) {
                draw_text_centered(upper_canvas, bx + box_w / 2, box_y + 29, "ACC", 1,
                                   is_active ? col_bg : col_accent);
            }
        }

        /* Pendulum / Phase Progress Bar inside Beat Panel */
        int pbar_x = 24, pbar_y = 194, pbar_w = 272, pbar_h = 6;
        draw_rect(upper_canvas, pbar_x, pbar_y, pbar_w, pbar_h, col_bg);
        if (g_playing && interval_ms > 0.0) {
            double elapsed = (double)(now_ms - last_beat_trigger_ms);
            double phase = elapsed / interval_ms;
            if (phase < 0.0) phase = 0.0;
            if (phase > 1.0) phase = 1.0;
            double pos_norm = (current_beat % 2 == 0) ? phase : (1.0 - phase);
            int knob_w = 20;
            int knob_x = pbar_x + (int)(pos_norm * (double)(pbar_w - knob_w));
            uint32_t knob_col = (current_beat == 0 && beat_flash) ? col_accent : col_cyan;
            draw_rect(upper_canvas, knob_x, pbar_y, knob_w, pbar_h, knob_col);
        }

        /* 4. Upper Screen Bottom Bar */
        draw_rect(upper_canvas, 0, 214, CANVAS_W, 26, col_panel_dark);
        draw_rect(upper_canvas, 0, 214, CANVAS_W, 1, col_border);
        draw_text_centered(upper_canvas, CANVAS_W / 2, 223,
                           "A:PLAY/PAUSE  UP/DN:BPM  L/R:SIG  L1/R1:+/-5", 1, col_muted);

        /* =================================================================
         * LOWER SCREEN: OVERALL STATISTICS (320x240 logical -> 1024x768)
         * ================================================================= */
        draw_rect(lower_canvas, 0, 0, CANVAS_W, CANVAS_H, col_bg);

        /* 1. Lower Screen Header Bar (y: 0..24) */
        draw_rect(lower_canvas, 0, 0, CANVAS_W, 24, col_panel_dark);
        draw_rect(lower_canvas, 0, 24, CANVAS_W, 2, col_border);
        draw_text_centered(lower_canvas, CANVAS_W / 2, 5, "OVERALL PRACTICE STATS", 2, col_purple);

        /* 2. Top Row Cards: Current Session (Left) & Lifetime Totals (Right) */
        draw_rect(lower_canvas, 10, 32, 146, 76, col_panel);
        draw_border(lower_canvas, 10, 32, 146, 76, 1, col_border);
        draw_text_centered(lower_canvas, 83, 38, "CURRENT SESSION", 1, col_cyan);

        char sess_time_str[32];
        format_duration(g_session_ms, sess_time_str, sizeof(sess_time_str));
        draw_text_centered(lower_canvas, 83, 52, sess_time_str, 2, col_white);

        char sess_detail[64];
        snprintf(sess_detail, sizeof(sess_detail), "BEATS: %llu", (unsigned long long)g_session_beats);
        draw_text_centered(lower_canvas, 83, 74, sess_detail, 1, col_beat);
        snprintf(sess_detail, sizeof(sess_detail), "BARS : %llu", (unsigned long long)g_session_bars);
        draw_text_centered(lower_canvas, 83, 88, sess_detail, 1, col_gold);

        /* Lifetime Totals Card (Right) */
        draw_rect(lower_canvas, 164, 32, 146, 76, col_panel);
        draw_border(lower_canvas, 164, 32, 146, 76, 1, col_border);
        draw_text_centered(lower_canvas, 237, 38, "ALL-TIME TOTALS", 1, col_gold);

        char total_time_str[32];
        format_duration(g_total_ms, total_time_str, sizeof(total_time_str));
        draw_text_centered(lower_canvas, 237, 52, total_time_str, 2, col_white);

        char tot_detail[64];
        snprintf(tot_detail, sizeof(tot_detail), "BEATS: %llu", (unsigned long long)g_total_beats);
        draw_text_centered(lower_canvas, 237, 74, tot_detail, 1, col_beat);
        snprintf(tot_detail, sizeof(tot_detail), "BARS : %llu  SESS: %llu",
                 (unsigned long long)g_total_bars, (unsigned long long)g_total_sessions);
        draw_text_centered(lower_canvas, 237, 88, tot_detail, 1, col_muted);

        /* 3. Tempo Summary Strip (y: 112..130) */
        draw_rect(lower_canvas, 10, 112, 300, 18, col_panel_dark);
        draw_border(lower_canvas, 10, 112, 300, 18, 1, col_border);
        int avg_bpm = (g_total_beats > 0) ? (int)(g_bpm_weighted_sum / g_total_beats) : g_bpm;
        int min_b   = (g_min_bpm_used > 0) ? g_min_bpm_used : g_bpm;
        int max_b   = (g_max_bpm_used > 0) ? g_max_bpm_used : g_bpm;
        char bpm_stat_line[80];
        snprintf(bpm_stat_line, sizeof(bpm_stat_line),
                 "BPM STATS   AVG: %d   MIN: %d   MAX: %d", avg_bpm, min_b, max_b);
        draw_text_centered(lower_canvas, CANVAS_W / 2, 118, bpm_stat_line, 1, col_cyan);

        /* 4. Time Signature Breakdown Panel (y: 134..212) */
        draw_rect(lower_canvas, 10, 134, 300, 78, col_panel);
        draw_border(lower_canvas, 10, 134, 300, 78, 1, col_border);
        draw_text_centered(lower_canvas, CANVAS_W / 2, 139, "TIME SIGNATURE BREAKDOWN", 1, col_muted);

        uint64_t max_sig_ms = 1;
        for (int s = 0; s < NUM_TIME_SIGS; s++) {
            if (g_sig_ms[s] > max_sig_ms) max_sig_ms = g_sig_ms[s];
        }

        for (int s = 0; s < NUM_TIME_SIGS; s++) {
            int row_y = 151 + s * 12;
            bool is_cur_sig = (s == g_time_sig_idx);
            uint32_t lbl_col = is_cur_sig ? col_gold : col_white;

            draw_text(lower_canvas, 18, row_y, k_time_sig_names[s], 1, lbl_col);

            /* Horizontal progress bar (x: 46..182) */
            int bar_x = 46, bar_w = 136, bar_h = 7;
            draw_rect(lower_canvas, bar_x, row_y, bar_w, bar_h, col_bg);
            int fill_w = (int)((g_sig_ms[s] * (uint64_t)bar_w) / max_sig_ms);
            if (g_sig_ms[s] > 0 && fill_w < 3) fill_w = 3;
            if (fill_w > 0) {
                draw_rect(lower_canvas, bar_x, row_y, fill_w, bar_h, is_cur_sig ? col_gold : col_cyan);
            }

            /* Time & Bars for this signature */
            char sig_stat[48];
            uint64_t sec = g_sig_ms[s] / 1000ULL;
            snprintf(sig_stat, sizeof(sig_stat), "%llum%02llus (%lluB)",
                     (unsigned long long)(sec / 60ULL),
                     (unsigned long long)(sec % 60ULL),
                     (unsigned long long)g_sig_bars[s]);
            draw_text(lower_canvas, 190, row_y, sig_stat, 1, is_cur_sig ? col_gold : col_muted);
        }

        /* 5. Lower Screen Bottom Bar (y: 216..240) */
        draw_rect(lower_canvas, 0, 216, CANVAS_W, 24, col_panel_dark);
        draw_rect(lower_canvas, 0, 216, CANVAS_W, 1, col_border);
        draw_text_centered(lower_canvas, CANVAS_W / 2, 224,
                           "B:CLR SESSION   SELECT/MENU:EXIT", 1, col_muted);

        platform_present_dual(upper_canvas, lower_canvas);
        delay_ms(8);
    }

    save_config();
    free(upper_canvas);
    free(lower_canvas);
    platform_shutdown();
    return 0;
}
