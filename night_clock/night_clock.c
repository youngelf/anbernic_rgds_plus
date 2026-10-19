/*
 * night_clock.c - Dual-Screen Bedside & Desk Night Clock for Anbernic RG DS Plus
 *
 * Built using the Anbernic RG DS Plus Native C SDK (../sdk):
 *   (0) Local Timezone Shift (D-Pad LEFT / RIGHT):
 *       - Shifts displayed clock & date in 30-minute increments (-24:00 to +24:00)
 *         relative to the device clock, without altering the OS/device timezone.
 *       - Persisted across boots in `.night_clock.cfg`.
 *   (1) Digital / Analog Display (X Key) & 10-Minute IPS Anti-Burn-In Orbit:
 *       - Top Screen (1024x768) shows the time centrally in 24-hour format.
 *       - Digital mode uses a custom 7x11 Retro LED Dot-Matrix bit-font.
 *       - Analog mode uses a minimalist high-contrast baton dial for distance readability.
 *       - Shifts position every 10 minutes along a bounded 9-point orbit to prevent
 *         IPS image retention / burn-in.
 *   (2) Bottom Screen Date Display, 5-Second Auto-Sleep, & Courtesy Wake:
 *       - Shows the full date (in shifted timezone), timezone shift, alarm status,
 *         brightness, and critical button mappings for 5 seconds, then turns off
 *         (powering off the lower LCD backlight on hardware, or showing a "SCREEN OFF"
 *         overlay on the Linux X11 desktop simulator).
 *       - Pressing ANY button while the bottom screen is asleep immediately performs
 *         that button's action AND wakes the bottom screen for 5 seconds.
 *       - Right Shoulder (R1) toggles a full Help screen listing all button mappings.
 *   (3) Brightness Control (D-Pad UP / DOWN) & Single Alarm (A Key):
 *       - UP / DOWN adjusts brightness (1..10 hardware backlight + software dimming).
 *       - Pressing A opens the Alarm Editor (evaluated in the shifted timezone).
 *       - Confirming the alarm with A saves it and plays a loud confirmation alarm sound.
 */

#include "rgds_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * 1. CONSTANTS & APPLICATION STATE
 * ============================================================================ */

#define NATIVE_W  RGDS_NATIVE_W   /* 1024 */
#define NATIVE_H  RGDS_NATIVE_H   /* 768  */

#define BOTTOM_WAKE_DURATION_MS   5000ULL
#define BURN_IN_INTERVAL_SEC      600ULL   /* 10 minutes */

#define REPEAT_INITIAL_DELAY_MS   320ULL
#define REPEAT_INTERVAL_MS         90ULL

#define AUDIO_SAMPLE_RATE         44100
#define AUDIO_CHUNK_FRAMES        512

typedef enum {
    UI_MODE_NORMAL    = 0,
    UI_MODE_ALARM_SET = 1,
    UI_MODE_HELP      = 2
} ui_mode_t;

typedef enum {
    ALARM_FIELD_HOUR   = 0,
    ALARM_FIELD_MINUTE = 1,
    ALARM_FIELD_STATE  = 2
} alarm_field_t;

/* Persisted configuration */
static int       g_tz_shift_mins = 0;      /* -1440 .. +1440 in steps of 30 mins */
static bool      g_is_analog     = false;  /* false = Retro LED Digital, true = Minimal Analog */
static int       g_brightness    = 7;      /* 1 .. 10 */
static bool      g_alarm_enabled = false;
static int       g_alarm_hour    = 7;      /* 0 .. 23 (in shifted timezone) */
static int       g_alarm_min     = 0;      /* 0 .. 59 (in shifted timezone) */

/* Runtime UI state */
static ui_mode_t     g_ui_mode            = UI_MODE_NORMAL;
static ui_mode_t     g_mode_before_help   = UI_MODE_NORMAL;
static alarm_field_t g_alarm_field        = ALARM_FIELD_HOUR;
static int           g_edit_alarm_hour    = 7;
static int           g_edit_alarm_min     = 0;
static bool          g_edit_alarm_enabled = true;

static bool      g_bottom_awake          = true;
static uint64_t  g_bottom_wake_until_ms  = 0;
#ifndef HOST_X11
static int       g_hw_bottom_lcdoff_cur  = 0;
static int       g_lcd_swap_flag         = 0;
#endif

/* Top-screen transient status toast (shown briefly when adjusting TZ/brightness/alarm) */
static char      g_toast_text[96]        = {0};
static uint64_t  g_toast_until_ms        = 0;

/* Alarm trigger & confirmation audio state */
static bool      g_alarm_ringing         = false;
static uint64_t  g_alarm_ring_start_ms   = 0;
static int       g_last_alarm_min_key    = -1;
static volatile int g_confirm_samples_rem = 0;
static int       g_saved_vol_before_alrm = -1;

/* 10-minute anti-burn-in smooth orbit state */
static uint64_t  g_boot_mono_ms          = 0;
static float     g_orbit_cur_x           = 0.0f;
static float     g_orbit_cur_y           = 0.0f;

static char      g_config_path[512]      = {0};
static volatile sig_atomic_t g_sig_quit  = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_sig_quit = 1;
}

/* ============================================================================
 * 2. HARDWARE BACKLIGHT & BOTTOM-SCREEN POWER CONTROL
 * ============================================================================ */

#ifndef HOST_X11
static int sysfs_read_int(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    char buf[32] = {0};
    int val = fallback;
    if (fgets(buf, sizeof(buf), f)) {
        val = atoi(buf);
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
#endif

static void apply_hardware_brightness(int level_1_to_10) {
    if (level_1_to_10 < 1)  level_1_to_10 = 1;
    if (level_1_to_10 > 10) level_1_to_10 = 10;
#ifndef HOST_X11
    sysfs_write_int("/sys/class/anbernic_misc/brightness", level_1_to_10);
    if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
        int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1 &");
        (void)r;
    }
#endif
}

static void set_bottom_screen_hw_power(bool awake) {
#ifndef HOST_X11
    /*
     * On the Anbernic RG DS Plus:
     *   /sys/class/anbernic_misc/lcdoff == 0 -> Both Upper & Lower screens ON
     *   /sys/class/anbernic_misc/lcdoff == 1 -> Lower screen (Display 1) backlight OFF
     *   /sys/class/anbernic_misc/lcdoff == 2 -> Upper screen (Display 0) backlight OFF
     */
    int target_lcdoff = awake ? 0 : (g_lcd_swap_flag ? 2 : 1);
    if (target_lcdoff != g_hw_bottom_lcdoff_cur) {
        g_hw_bottom_lcdoff_cur = target_lcdoff;
        sysfs_write_int("/sys/class/anbernic_misc/lcdoff", target_lcdoff);
        if (access("/mnt/vendor/bin/bright.dge", X_OK) == 0) {
            int r = system("/mnt/vendor/bin/bright.dge >/dev/null 2>&1 &");
            (void)r;
        }
    }
#else
    (void)awake;
#endif
}

static void wake_bottom_screen(uint64_t now_ms) {
    g_bottom_awake = true;
    g_bottom_wake_until_ms = now_ms + BOTTOM_WAKE_DURATION_MS;
    set_bottom_screen_hw_power(true);
}

static void show_toast(const char *msg, uint64_t now_ms) {
    snprintf(g_toast_text, sizeof(g_toast_text), "%s", msg);
    g_toast_until_ms = now_ms + 2200ULL;
}

/* ============================================================================
 * 3. PERSISTENT CONFIGURATION (.night_clock.cfg)
 * ============================================================================ */

static void resolve_config_path(void) {
    struct stat st;
    const char *dirs[] = {
        "/mnt/sdcard/Roms/APPS/night_clock",
        "/mnt/mmc/Roms/APPS/night_clock",
        "/mnt/sdcard/Ports/Night_Clock",
        "/mnt/mmc/Ports/Night_Clock",
        "/mnt/sdcard",
        "/mnt/mmc",
        "/userdata",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(dirs[i], W_OK) == 0) {
            snprintf(g_config_path, sizeof(g_config_path), "%s/.night_clock.cfg", dirs[i]);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s/.night_clock.cfg", home);
    } else {
        snprintf(g_config_path, sizeof(g_config_path), "/tmp/.night_clock.cfg");
    }
}

static void load_config(void) {
    resolve_config_path();
    FILE *f = fopen(g_config_path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val = 0;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if (strcmp(key, "tz_shift_mins") == 0) {
                if (val >= -1440 && val <= 1440) {
                    /* Snap to nearest 30-minute increment */
                    g_tz_shift_mins = (val / 30) * 30;
                }
            } else if (strcmp(key, "is_analog") == 0) {
                g_is_analog = (val != 0);
            } else if (strcmp(key, "brightness") == 0) {
                if (val >= 1 && val <= 10) g_brightness = val;
            } else if (strcmp(key, "alarm_enabled") == 0) {
                g_alarm_enabled = (val != 0);
            } else if (strcmp(key, "alarm_hour") == 0) {
                if (val >= 0 && val <= 23) g_alarm_hour = val;
            } else if (strcmp(key, "alarm_min") == 0) {
                if (val >= 0 && val <= 59) g_alarm_min = val;
            }
        }
    }
    fclose(f);
}

static void save_config(void) {
    if (!g_config_path[0]) resolve_config_path();
    FILE *f = fopen(g_config_path, "w");
    if (!f) return;
    fprintf(f, "tz_shift_mins=%d\n", g_tz_shift_mins);
    fprintf(f, "is_analog=%d\n",     g_is_analog ? 1 : 0);
    fprintf(f, "brightness=%d\n",    g_brightness);
    fprintf(f, "alarm_enabled=%d\n", g_alarm_enabled ? 1 : 0);
    fprintf(f, "alarm_hour=%d\n",    g_alarm_hour);
    fprintf(f, "alarm_min=%d\n",     g_alarm_min);
    fclose(f);
    sync();
}

/* ============================================================================
 * 4. ALARM & CONFIRMATION AUDIO ENGINE (SDL2 / PulseAudio / ALSA)
 * ============================================================================ */

static volatile bool g_audio_running = false;
static pthread_t     g_audio_thread;
static bool          g_use_sdl_audio = false;
static uint32_t      g_tone_sample_idx = 0;

#ifndef HOST_X11
typedef struct {
    int      freq;
    uint16_t format;
    uint8_t  channels;
    uint8_t  silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void   (*callback)(void *userdata, uint8_t *stream, int len);
    void    *userdata;
} sdl_audio_spec_t;

#define SDL_INIT_AUDIO_SUBSYS 0x00000010u
#define SDL_AUDIO_S16SYS_FMT  0x8010u

static void *g_sdl_audio_handle = NULL;
#endif
static void (*pSDL_CloseAudio)(void) = NULL;

/*
 * Synthesize loud, unmistakable dual-harmonic bedside alarm pulses:
 *   - Used both when the user confirms an alarm with A (~1.6s confirmation burst)
 *     and when the alarm time is reached (continuous repeating klaxon pattern).
 */
static void render_alarm_samples(int16_t *out, int num_samples) {
    for (int i = 0; i < num_samples; i++) {
        int rem_confirm = g_confirm_samples_rem;
        bool active_confirm = (rem_confirm > 0);
        if (active_confirm) {
            g_confirm_samples_rem = rem_confirm - 1;
        }

        if (!active_confirm && !g_alarm_ringing) {
            out[i] = 0;
            continue;
        }

        uint32_t idx = g_tone_sample_idx++;
        /*
         * 4-pulse alarm cadence:
         * Each cycle is 0.80s (35280 samples @ 44100 Hz) split into 4 fast pulses
         * of 0.14s (6174 samples) with a brief pause at the end of the group.
         */
        uint32_t pos_in_group = idx % 35280u;
        uint32_t pulse_idx    = pos_in_group / 6615u;
        uint32_t pos_in_pulse = pos_in_group % 6615u;

        bool gate_on = (pulse_idx < 4) && (pos_in_pulse < 4600u);
        if (!gate_on) {
            out[i] = 0;
            continue;
        }

        /* Alternating high/higher alarm pitch (880 Hz A5 / 1174.66 Hz D6) + harmonics */
        float base_freq = (pulse_idx % 2 == 0) ? 880.0f : 1174.66f;
        float t = (float)idx / (float)AUDIO_SAMPLE_RATE;
        float s1 = sinf(2.0f * (float)M_PI * base_freq * t);
        float s2 = 0.55f * sinf(2.0f * (float)M_PI * (base_freq * 2.0f) * t);
        float s3 = 0.30f * sinf(2.0f * (float)M_PI * (base_freq * 3.0f) * t);

        /* Soft attack/decay envelope on each pulse to prevent speaker pop */
        float env = 1.0f;
        if (pos_in_pulse < 220u) {
            env = (float)pos_in_pulse / 220.0f;
        } else if (pos_in_pulse > 4100u) {
            env = (float)(4600u - pos_in_pulse) / 500.0f;
        }

        float wave = (s1 + s2 + s3) * (1.0f / 1.85f) * env;
        int32_t sample = (int32_t)(wave * 28000.0f);
        if (sample >  32000) sample =  32000;
        if (sample < -32000) sample = -32000;
        out[i] = (int16_t)sample;
    }
}

#ifndef HOST_X11
static void sdl_audio_cb(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    render_alarm_samples((int16_t *)stream, len / (int)sizeof(int16_t));
}
#endif

static void *audio_thread_fn(void *arg) {
    (void)arg;
    int16_t buf[AUDIO_CHUNK_FRAMES];

    /* 1. Try PulseAudio Simple (libpulse-simple.so.0) */
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (pa_lib) {
        typedef struct { int format; uint32_t rate; uint8_t channels; } pa_ss_t;
        typedef void *(*pa_new_fn)(const char *, const char *, int, const char *,
                                   const char *, const pa_ss_t *, const void *,
                                   const void *, int *);
        typedef int   (*pa_write_fn)(void *, const void *, size_t, int *);
        typedef void  (*pa_free_fn)(void *);

        pa_new_fn   fn_new   = (pa_new_fn)dlsym(pa_lib, "pa_simple_new");
        pa_write_fn fn_write = (pa_write_fn)dlsym(pa_lib, "pa_simple_write");
        pa_free_fn  fn_free  = (pa_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_new && fn_write && fn_free) {
            pa_ss_t ss = { 3 /* PA_SAMPLE_S16LE */, AUDIO_SAMPLE_RATE, 1 };
            int err = 0;
            void *pa = fn_new(NULL, "NightClock", 1 /* PLAYBACK */, NULL, "Alarm", &ss, NULL, NULL, &err);
            if (pa) {
                while (g_audio_running) {
                    render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
                    fn_write(pa, buf, sizeof(buf), &err);
                }
                fn_free(pa);
                dlclose(pa_lib);
                return NULL;
            }
        }
        dlclose(pa_lib);
    }

    /* 2. Fallback to ALSA (libasound.so.2) */
    void *alsa_lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (alsa_lib) {
        typedef int  (*snd_open_fn)(void **, const char *, int, int);
        typedef int  (*snd_set_params_fn)(void *, int, int, unsigned int, unsigned int, int, unsigned int);
        typedef long (*snd_writei_fn)(void *, const void *, unsigned long);
        typedef int  (*snd_recover_fn)(void *, int, int);
        typedef int  (*snd_close_fn)(void *);

        snd_open_fn       fn_open    = (snd_open_fn)dlsym(alsa_lib, "snd_pcm_open");
        snd_set_params_fn fn_params  = (snd_set_params_fn)dlsym(alsa_lib, "snd_pcm_set_params");
        snd_writei_fn     fn_writei  = (snd_writei_fn)dlsym(alsa_lib, "snd_pcm_writei");
        snd_recover_fn    fn_recover = (snd_recover_fn)dlsym(alsa_lib, "snd_pcm_recover");
        snd_close_fn      fn_close   = (snd_close_fn)dlsym(alsa_lib, "snd_pcm_close");

        if (fn_open && fn_params && fn_writei && fn_close) {
            void *pcm = NULL;
            if (fn_open(&pcm, "default", 0, 0) == 0 && pcm) {
                if (fn_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, 1, AUDIO_SAMPLE_RATE, 1, 50000) == 0) {
                    while (g_audio_running) {
                        render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
                        long r = fn_writei(pcm, buf, AUDIO_CHUNK_FRAMES);
                        if (r < 0 && fn_recover) fn_recover(pcm, (int)r, 1);
                    }
                    fn_close(pcm);
                    dlclose(alsa_lib);
                    return NULL;
                }
                fn_close(pcm);
            }
        }
        dlclose(alsa_lib);
    }

    /* 3. Silent pacing fallback if no audio device is present */
    while (g_audio_running) {
        render_alarm_samples(buf, AUDIO_CHUNK_FRAMES);
        rgds_delay_ms(10);
    }
    return NULL;
}

static void audio_init(void) {
#ifndef HOST_X11
    const char *sdl_names[] = {
        "libSDL2-2.0.so.0",
        "/usr/lib/libSDL2-2.0.so.0",
        "/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0",
        NULL
    };
    for (int i = 0; sdl_names[i]; i++) {
        g_sdl_audio_handle = dlopen(sdl_names[i], RTLD_LAZY | RTLD_GLOBAL);
        if (g_sdl_audio_handle) break;
    }
    if (g_sdl_audio_handle) {
        int  (*fn_InitSub)(uint32_t) = dlsym(g_sdl_audio_handle, "SDL_InitSubSystem");
        int  (*fn_OpenAudio)(sdl_audio_spec_t *, sdl_audio_spec_t *) = dlsym(g_sdl_audio_handle, "SDL_OpenAudio");
        void (*fn_PauseAudio)(int) = dlsym(g_sdl_audio_handle, "SDL_PauseAudio");
        pSDL_CloseAudio = dlsym(g_sdl_audio_handle, "SDL_CloseAudio");

        if (fn_InitSub && fn_OpenAudio && fn_PauseAudio && fn_InitSub(SDL_INIT_AUDIO_SUBSYS) == 0) {
            sdl_audio_spec_t want;
            memset(&want, 0, sizeof(want));
            want.freq     = AUDIO_SAMPLE_RATE;
            want.format   = SDL_AUDIO_S16SYS_FMT;
            want.channels = 1;
            want.samples  = AUDIO_CHUNK_FRAMES;
            want.callback = sdl_audio_cb;
            if (fn_OpenAudio(&want, NULL) == 0) {
                fn_PauseAudio(0);
                g_use_sdl_audio = true;
                return;
            }
        }
    }
#endif
    g_audio_running = true;
    pthread_create(&g_audio_thread, NULL, audio_thread_fn, NULL);
}

static void audio_shutdown(void) {
    if (g_use_sdl_audio) {
        if (pSDL_CloseAudio) pSDL_CloseAudio();
        g_use_sdl_audio = false;
    } else if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
}

static void trigger_loud_confirmation_sound(void) {
    /* Ensure system volume is loud enough to hear the confirmation clearly */
    int cur_vol = rgds_get_volume();
    if (cur_vol < 9) {
        g_saved_vol_before_alrm = cur_vol;
        rgds_set_volume(9);
    }
    g_tone_sample_idx = 0;
    /* Play 2 full 4-pulse groups (~1.6 seconds at 44100 Hz) */
    g_confirm_samples_rem = 35280 * 2;
}

/* ============================================================================
 * 5. NATIVE 1024x768 ANTI-ALIASED DRAWING & RETRO LED DOT-MATRIX FONT
 * ============================================================================ */

/* Apply brightness dimming curve (22% at level 1 .. 100% at level 10) */
static uint32_t dim_color(uint32_t col, int brightness_1_to_10) {
    if (brightness_1_to_10 < 1)  brightness_1_to_10 = 1;
    if (brightness_1_to_10 > 10) brightness_1_to_10 = 10;
    float scale = 0.22f + 0.78f * ((float)(brightness_1_to_10 - 1) / 9.0f);
    uint32_t r = (uint32_t)((float)((col >> 16) & 0xFFu) * scale);
    uint32_t g = (uint32_t)((float)((col >>  8) & 0xFFu) * scale);
    uint32_t b = (uint32_t)((float)( col        & 0xFFu) * scale);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return RGDS_RGB(r, g, b);
}

static inline void blend_pixel(uint32_t *buf, int x, int y, uint32_t color, float alpha) {
    if (x < 0 || x >= NATIVE_W || y < 0 || y >= NATIVE_H || alpha <= 0.0f) return;
    if (alpha >= 1.0f) {
        buf[y * NATIVE_W + x] = color;
        return;
    }
    uint32_t dst = buf[y * NATIVE_W + x];
    float inv = 1.0f - alpha;
    uint32_t sr = (color >> 16) & 0xFFu;
    uint32_t sg = (color >>  8) & 0xFFu;
    uint32_t sb =  color        & 0xFFu;
    uint32_t dr = (dst   >> 16) & 0xFFu;
    uint32_t dg = (dst   >>  8) & 0xFFu;
    uint32_t db =  dst          & 0xFFu;
    uint32_t r  = (uint32_t)(sr * alpha + dr * inv);
    uint32_t g  = (uint32_t)(sg * alpha + dg * inv);
    uint32_t b  = (uint32_t)(sb * alpha + db * inv);
    buf[y * NATIVE_W + x] = RGDS_RGB(r, g, b);
}

/* Anti-aliased filled disc using signed distance field */
static void draw_aa_disc(rgds_screen_t scr, float cx, float cy, float radius, uint32_t color) {
    uint32_t *buf = rgds_get_native_buffer(scr);
    int x0 = (int)floorf(cx - radius - 1.5f);
    int x1 = (int)ceilf (cx + radius + 1.5f);
    int y0 = (int)floorf(cy - radius - 1.5f);
    int y1 = (int)ceilf (cy + radius + 1.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= NATIVE_W) x1 = NATIVE_W - 1;
    if (y1 >= NATIVE_H) y1 = NATIVE_H - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = radius + 0.5f - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(buf, x, y, color, cov);
            }
        }
    }
}

/* Anti-aliased ring outline (inner_r .. outer_r) */
static void draw_aa_ring(rgds_screen_t scr, float cx, float cy, float inner_r, float outer_r, uint32_t color) {
    uint32_t *buf = rgds_get_native_buffer(scr);
    int x0 = (int)floorf(cx - outer_r - 1.5f);
    int x1 = (int)ceilf (cx + outer_r + 1.5f);
    int y0 = (int)floorf(cy - outer_r - 1.5f);
    int y1 = (int)ceilf (cy + outer_r + 1.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= NATIVE_W) x1 = NATIVE_W - 1;
    if (y1 >= NATIVE_H) y1 = NATIVE_H - 1;

    float min_r_bound = (inner_r > 2.0f) ? (inner_r - 1.5f) : 0.0f;
    float min_r_sq    = min_r_bound * min_r_bound;
    float max_r_bound = outer_r + 1.5f;
    float max_r_sq    = max_r_bound * max_r_bound;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - cy;
        float dy2 = dy * dy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float d2 = dx * dx + dy2;
            if (d2 < min_r_sq || d2 > max_r_sq) continue;
            float dist = sqrtf(d2);
            float a_out = outer_r + 0.5f - dist;
            float a_in  = dist - (inner_r - 0.5f);
            float cov   = (a_out < a_in) ? a_out : a_in;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(buf, x, y, color, cov);
            }
        }
    }
}

/* Anti-aliased capsule / baton line segment from (x0, y0) to (x1, y1) with thickness */
static void draw_aa_segment(rgds_screen_t scr, float x0, float y0, float x1, float y1,
                            float thickness, bool round_caps, uint32_t color) {
    uint32_t *buf = rgds_get_native_buffer(scr);
    float half_w = thickness * 0.5f;
    float min_x = ((x0 < x1) ? x0 : x1) - half_w - 2.0f;
    float max_x = ((x0 > x1) ? x0 : x1) + half_w + 2.0f;
    float min_y = ((y0 < y1) ? y0 : y1) - half_w - 2.0f;
    float max_y = ((y0 > y1) ? y0 : y1) + half_w + 2.0f;

    int ix0 = (int)floorf(min_x);
    int ix1 = (int)ceilf (max_x);
    int iy0 = (int)floorf(min_y);
    int iy1 = (int)ceilf (max_y);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 >= NATIVE_W) ix1 = NATIVE_W - 1;
    if (iy1 >= NATIVE_H) iy1 = NATIVE_H - 1;

    float vx = x1 - x0;
    float vy = y1 - y0;
    float len = sqrtf(vx * vx + vy * vy);
    if (len < 0.001f) {
        draw_aa_disc(scr, x0, y0, half_w, color);
        return;
    }
    float ux = vx / len;
    float uy = vy / len;

    for (int y = iy0; y <= iy1; y++) {
        float py = (float)y - y0;
        for (int x = ix0; x <= ix1; x++) {
            float px = (float)x - x0;
            float proj = px * ux + py * uy;
            float perp = fabsf(-px * uy + py * ux);
            float dist;
            if (round_caps) {
                float clamped = (proj < 0.0f) ? 0.0f : (proj > len ? len : proj);
                float dx = px - clamped * ux;
                float dy = py - clamped * uy;
                dist = sqrtf(dx * dx + dy * dy) - half_w;
            } else {
                float d_along = (proj < 0.0f) ? -proj : (proj > len ? (proj - len) : 0.0f);
                float d_perp  = perp - half_w;
                if (d_along <= 0.0f && d_perp <= 0.0f) {
                    dist = (d_along > d_perp) ? d_along : d_perp;
                } else if (d_along > 0.0f && d_perp > 0.0f) {
                    dist = sqrtf(d_along * d_along + d_perp * d_perp);
                } else {
                    dist = (d_along > d_perp) ? d_along : d_perp;
                }
            }
            float cov = 0.5f - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                blend_pixel(buf, x, y, color, cov);
            }
        }
    }
}

/*
 * Custom 7x11 High-Contrast Retro LED Dot-Matrix Bit-Font for Digital Clock Numerals (0..9)
 * Each row is a 7-bit mask (bit 6 = leftmost column, bit 0 = rightmost column).
 */
static const uint8_t k_led_digits_7x11[10][11] = {
    /* 0 */ {
        0x1C, /* ..###.. */
        0x3E, /* .#####. */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x67, /* ##..### */
        0x6B, /* ##.#.## */
        0x73, /* ###..## */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x3E, /* .#####. */
        0x1C  /* ..###.. */
    },
    /* 1 */ {
        0x0C, /* ...##.. */
        0x1C, /* ..###.. */
        0x3C, /* .####.. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x3F, /* .###### */
        0x3F  /* .###### */
    },
    /* 2 */ {
        0x3E, /* .#####. */
        0x7F, /* ####### */
        0x63, /* ##...## */
        0x03, /* .....## */
        0x0E, /* ...###. */
        0x1C, /* ..###.. */
        0x38, /* .###... */
        0x70, /* ###.... */
        0x60, /* ##..... */
        0x7F, /* ####### */
        0x7F  /* ####### */
    },
    /* 3 */ {
        0x3E, /* .#####. */
        0x7F, /* ####### */
        0x63, /* ##...## */
        0x03, /* .....## */
        0x1E, /* ..####. */
        0x1E, /* ..####. */
        0x03, /* .....## */
        0x03, /* .....## */
        0x63, /* ##...## */
        0x7F, /* ####### */
        0x3E  /* .#####. */
    },
    /* 4 */ {
        0x06, /* ....##. */
        0x0E, /* ...###. */
        0x1E, /* ..####. */
        0x36, /* .##.##. */
        0x66, /* ##..##. */
        0x66, /* ##..##. */
        0x7F, /* ####### */
        0x7F, /* ####### */
        0x06, /* ....##. */
        0x06, /* ....##. */
        0x06  /* ....##. */
    },
    /* 5 */ {
        0x7F, /* ####### */
        0x7F, /* ####### */
        0x60, /* ##..... */
        0x60, /* ##..... */
        0x7E, /* ######. */
        0x7F, /* ####### */
        0x03, /* .....## */
        0x03, /* .....## */
        0x63, /* ##...## */
        0x7F, /* ####### */
        0x3E  /* .#####. */
    },
    /* 6 */ {
        0x1E, /* ..####. */
        0x3E, /* .#####. */
        0x60, /* ##..... */
        0x60, /* ##..... */
        0x7E, /* ######. */
        0x7F, /* ####### */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x3E, /* .#####. */
        0x1C  /* ..###.. */
    },
    /* 7 */ {
        0x7F, /* ####### */
        0x7F, /* ####### */
        0x63, /* ##...## */
        0x06, /* ....##. */
        0x0C, /* ...##.. */
        0x0C, /* ...##.. */
        0x18, /* ..##... */
        0x18, /* ..##... */
        0x18, /* ..##... */
        0x18, /* ..##... */
        0x18  /* ..##... */
    },
    /* 8 */ {
        0x3E, /* .#####. */
        0x7F, /* ####### */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x3E, /* .#####. */
        0x3E, /* .#####. */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x7F, /* ####### */
        0x3E  /* .#####. */
    },
    /* 9 */ {
        0x1C, /* ..###.. */
        0x3E, /* .#####. */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x63, /* ##...## */
        0x7F, /* ####### */
        0x3F, /* .###### */
        0x03, /* .....## */
        0x03, /* .....## */
        0x3E, /* .#####. */
        0x3C  /* .####.. */
    }
};

static void draw_led_digit_7x11(rgds_screen_t scr, int x0, int y0, int digit,
                                int pitch, float dot_r,
                                uint32_t on_col, uint32_t core_col, uint32_t off_col) {
    if (digit < 0 || digit > 9) return;
    for (int r = 0; r < 11; r++) {
        uint8_t row_bits = k_led_digits_7x11[digit][r];
        float cy = (float)(y0 + r * pitch) + (float)pitch * 0.5f;
        for (int c = 0; c < 7; c++) {
            float cx = (float)(x0 + c * pitch) + (float)pitch * 0.5f;
            bool lit = (row_bits & (1u << (6 - c))) != 0;
            if (lit) {
                draw_aa_disc(scr, cx, cy, dot_r, on_col);
                if (dot_r >= 4.0f) {
                    draw_aa_disc(scr, cx, cy, dot_r * 0.45f, core_col);
                }
            } else if (off_col != 0) {
                draw_aa_disc(scr, cx, cy, dot_r * 0.52f, off_col);
            }
        }
    }
}

static void draw_led_colon(rgds_screen_t scr, int x0, int y0, int pitch, float dot_r,
                           bool lit, uint32_t on_col, uint32_t core_col, uint32_t off_col) {
    /* 2x2 dot clusters at rows 2..3 and rows 7..8 */
    int rows[4] = { 2, 3, 7, 8 };
    for (int i = 0; i < 4; i++) {
        float cy = (float)(y0 + rows[i] * pitch) + (float)pitch * 0.5f;
        for (int c = 0; c < 2; c++) {
            float cx = (float)(x0 + c * pitch) + (float)pitch * 0.5f;
            if (lit) {
                draw_aa_disc(scr, cx, cy, dot_r, on_col);
                if (dot_r >= 4.0f) {
                    draw_aa_disc(scr, cx, cy, dot_r * 0.45f, core_col);
                }
            } else if (off_col != 0) {
                draw_aa_disc(scr, cx, cy, dot_r * 0.52f, off_col);
            }
        }
    }
}

/* ============================================================================
 * 6. TOP SCREEN RENDERERS (DIGITAL & ANALOG WITH 10-MIN BURN-IN ORBIT)
 * ============================================================================ */

static void update_burn_in_orbit(uint64_t now_ms) {
    /*
     * 9-point bounded orbit stepped every 10 minutes (600,000 ms) so no pixel
     * remains permanently fixed on the IPS panel.
     */
    static const float k_orbit_pts[9][2] = {
        {   0.0f,   0.0f },
        {  62.0f, -38.0f },
        { -58.0f,  40.0f },
        {  66.0f,  36.0f },
        { -64.0f, -36.0f },
        {   0.0f,  44.0f },
        { -68.0f,   0.0f },
        {   0.0f, -44.0f },
        {  68.0f,   0.0f }
    };
    uint64_t elapsed_ms = (now_ms >= g_boot_mono_ms) ? (now_ms - g_boot_mono_ms) : 0;
    int slot = (int)((elapsed_ms / (BURN_IN_INTERVAL_SEC * 1000ULL)) % 9ULL);
    float tx = k_orbit_pts[slot][0];
    float ty = k_orbit_pts[slot][1];

    /* Smoothly glide toward the target slot center */
    g_orbit_cur_x += (tx - g_orbit_cur_x) * 0.08f;
    g_orbit_cur_y += (ty - g_orbit_cur_y) * 0.08f;
}

static void format_tz_shift(char *buf, size_t sz, int shift_mins) {
    char sign = (shift_mins >= 0) ? '+' : '-';
    int abs_m = (shift_mins >= 0) ? shift_mins : -shift_mins;
    int hh = abs_m / 60;
    int mm = abs_m % 60;
    snprintf(buf, sz, "DEV %c%02d:%02d", sign, hh, mm);
}

static void render_top_digital_clock(const struct tm *shifted_tm, int cx, int cy, uint64_t now_ms) {
    /*
     * Retro LED Dot-Matrix 24-Hour Digital Clock (HH:MM only, constant colon):
     *   Main HH:MM digits: 7x11 dots each, pitch = 24px -> each digit is 168x264 px
     */
    int pitch_main = 24;
    float dot_r_main = 8.9f;
    int digit_w = 7 * pitch_main;   /* 168 */
    int digit_h = 11 * pitch_main;  /* 264 */
    int gap_d   = 26;
    int colon_w = 2 * pitch_main;   /* 48  */

    int total_w = digit_w * 4 + gap_d * 4 + colon_w;
    int start_x = cx - total_w / 2;
    int start_y = cy - digit_h / 2 - 14;

    uint32_t on_col   = dim_color(RGDS_RGB(88, 224, 255),  g_brightness);
    uint32_t core_col = dim_color(RGDS_RGB(215, 250, 255), g_brightness);
    uint32_t off_col  = dim_color(RGDS_RGB(14, 24, 36),    g_brightness);

    if (g_alarm_ringing && ((now_ms / 250ULL) % 2ULL == 0ULL)) {
        on_col   = RGDS_RGB(255, 90, 82);
        core_col = RGDS_RGB(255, 225, 220);
    }

    int hr = shifted_tm->tm_hour % 24;
    int mn = shifted_tm->tm_min  % 60;

    int x = start_x;
    /* H1, H2 */
    draw_led_digit_7x11(RGDS_SCREEN_TOP, x, start_y, hr / 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;
    draw_led_digit_7x11(RGDS_SCREEN_TOP, x, start_y, hr % 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;

    /* Constant LED Colon separating Hours and Minutes */
    draw_led_colon(RGDS_SCREEN_TOP, x, start_y, pitch_main, dot_r_main, true, on_col, core_col, off_col);
    x += colon_w + gap_d;

    /* M1, M2 */
    draw_led_digit_7x11(RGDS_SCREEN_TOP, x, start_y, mn / 10, pitch_main, dot_r_main, on_col, core_col, off_col);
    x += digit_w + gap_d;
    draw_led_digit_7x11(RGDS_SCREEN_TOP, x, start_y, mn % 10, pitch_main, dot_r_main, on_col, core_col, off_col);

    /* Subtle metadata strip underneath the digital clock */
    char tz_buf[32];
    format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
    char meta_line[128];
    if (g_alarm_enabled) {
        snprintf(meta_line, sizeof(meta_line), "24H   |   %s   |   ALARM %02d:%02d [ON]",
                 tz_buf, g_alarm_hour, g_alarm_min);
    } else {
        snprintf(meta_line, sizeof(meta_line), "24H   |   %s   |   ALARM OFF", tz_buf);
    }
    uint32_t meta_col = dim_color(g_alarm_enabled ? RGDS_RGB(110, 210, 155) : RGDS_RGB(95, 120, 155), g_brightness);
    int meta_w = rgds_text_width(meta_line, 3);
    rgds_draw_text(RGDS_SCREEN_TOP, cx - meta_w / 2, start_y + digit_h + 40, meta_line, 3, meta_col);
}

static void render_top_analog_clock(const struct tm *shifted_tm, int cx, int cy, uint64_t now_ms) {
    float fcx = (float)cx;
    float fcy = (float)cy;
    float dial_r = 285.0f;

    uint32_t ring_col   = dim_color(RGDS_RGB(62, 86, 122),   g_brightness);
    uint32_t major_col  = dim_color(RGDS_RGB(238, 244, 252), g_brightness);
    uint32_t minor_col  = dim_color(RGDS_RGB(165, 185, 215), g_brightness);
    uint32_t tick_col   = dim_color(RGDS_RGB(78, 102, 138),  g_brightness);
    uint32_t hand_col   = dim_color(RGDS_RGB(244, 248, 255), g_brightness);
    uint32_t sec_col    = dim_color(RGDS_RGB(246, 88, 78),   g_brightness);
    uint32_t alarm_col  = dim_color(RGDS_RGB(248, 212, 108), g_brightness);
    uint32_t pin_col    = dim_color(RGDS_RGB(18, 24, 36),    g_brightness);

    if (g_alarm_ringing && ((now_ms / 250ULL) % 2ULL == 0ULL)) {
        ring_col = RGDS_RGB(255, 88, 78);
    }

    /* 1. Outer minimalist bezel ring */
    draw_aa_ring(RGDS_SCREEN_TOP, fcx, fcy, dial_r - 2.5f, dial_r + 1.0f, ring_col);

    /* 2. 60 Minute ticks & 12 Bold Baton Hour Markers */
    for (int m = 0; m < 60; m++) {
        float ang = ((float)m * 6.0f - 90.0f) * ((float)M_PI / 180.0f);
        float ca = cosf(ang);
        float sa = sinf(ang);
        if (m % 5 == 0) {
            bool is_cardinal = (m % 15 == 0);
            float r_out = dial_r - 12.0f;
            float r_in  = dial_r - (is_cardinal ? 48.0f : 36.0f);
            float thick = is_cardinal ? 10.0f : 6.5f;
            uint32_t c  = is_cardinal ? major_col : minor_col;
            draw_aa_segment(RGDS_SCREEN_TOP,
                            fcx + ca * r_in,  fcy + sa * r_in,
                            fcx + ca * r_out, fcy + sa * r_out,
                            thick, false, c);
        } else {
            float r_out = dial_r - 12.0f;
            float r_in  = dial_r - 22.0f;
            draw_aa_segment(RGDS_SCREEN_TOP,
                            fcx + ca * r_in,  fcy + sa * r_in,
                            fcx + ca * r_out, fcy + sa * r_out,
                            2.2f, false, tick_col);
        }
    }

    /* 3. Optional Alarm Indicator Hand (when alarm is enabled) */
    if (g_alarm_enabled) {
        float alrm_deg = ((float)(g_alarm_hour % 12) + (float)g_alarm_min / 60.0f) * 30.0f - 90.0f;
        float alrm_rad = alrm_deg * ((float)M_PI / 180.0f);
        float ax = fcx + cosf(alrm_rad) * 195.0f;
        float ay = fcy + sinf(alrm_rad) * 195.0f;
        draw_aa_segment(RGDS_SCREEN_TOP, fcx, fcy, ax, ay, 3.2f, true, alarm_col);
        draw_aa_disc(RGDS_SCREEN_TOP, ax, ay, 5.5f, alarm_col);
    }

    /* 5. Hour Hand (wide high-contrast baton) */
    float min_f  = (float)(shifted_tm->tm_min % 60);
    float hour_f = (float)(shifted_tm->tm_hour % 12) + min_f / 60.0f;

    float hr_rad = (hour_f * 30.0f - 90.0f) * ((float)M_PI / 180.0f);
    float hr_ca  = cosf(hr_rad);
    float hr_sa  = sinf(hr_rad);
    draw_aa_segment(RGDS_SCREEN_TOP,
                    fcx - hr_ca * 30.0f,  fcy - hr_sa * 30.0f,
                    fcx + hr_ca * 162.0f, fcy + hr_sa * 162.0f,
                    15.0f, true, hand_col);

    /* 6. Minute Hand (long high-contrast baton) */
    float mn_rad = (min_f * 6.0f - 90.0f) * ((float)M_PI / 180.0f);
    float mn_ca  = cosf(mn_rad);
    float mn_sa  = sinf(mn_rad);
    draw_aa_segment(RGDS_SCREEN_TOP,
                    fcx - mn_ca * 36.0f,  fcy - mn_sa * 36.0f,
                    fcx + mn_ca * 236.0f, fcy + mn_sa * 236.0f,
                    10.5f, true, hand_col);

    /* Center pivot cap */
    draw_aa_disc(RGDS_SCREEN_TOP, fcx, fcy, 12.0f, hand_col);
    draw_aa_disc(RGDS_SCREEN_TOP, fcx, fcy,  5.0f, sec_col);
    draw_aa_disc(RGDS_SCREEN_TOP, fcx, fcy,  2.5f, pin_col);
}

static void render_top_screen(const struct tm *shifted_tm, uint64_t now_ms) {
    rgds_clear(RGDS_SCREEN_TOP, RGDS_COLOR_BLACK);

    update_burn_in_orbit(now_ms);
    int cx = (NATIVE_W / 2) + (int)roundf(g_orbit_cur_x);
    int cy = (NATIVE_H / 2) + (int)roundf(g_orbit_cur_y);

    if (g_is_analog) {
        render_top_analog_clock(shifted_tm, cx, cy, now_ms);
    } else {
        render_top_digital_clock(shifted_tm, cx, cy, now_ms);
    }

    /* Alarm Ringing Banner on Top Screen */
    if (g_alarm_ringing) {
        bool flash = ((now_ms / 300ULL) % 2ULL == 0ULL);
        uint32_t bg = flash ? RGDS_RGB(235, 65, 60) : RGDS_RGB(40, 16, 20);
        uint32_t fg = flash ? RGDS_COLOR_WHITE      : RGDS_RGB(255, 120, 110);
        rgds_fill_rect(RGDS_SCREEN_TOP, 112, 24, 800, 64, bg);
        rgds_draw_rect(RGDS_SCREEN_TOP, 112, 24, 800, 64, 3, RGDS_COLOR_WHITE);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 42, "ALARM RINGING! PRESS ANY BUTTON TO DISMISS", 3, fg);
    } else if (g_ui_mode == UI_MODE_ALARM_SET) {
        rgds_fill_rect(RGDS_SCREEN_TOP, 212, 22, 600, 52, RGDS_RGB(24, 38, 62));
        rgds_draw_rect(RGDS_SCREEN_TOP, 212, 22, 600, 52, 2, RGDS_RGB(90, 185, 250));
        char buf[96];
        snprintf(buf, sizeof(buf), "SETTING ALARM: %02d:%02d [%s]",
                 g_edit_alarm_hour, g_edit_alarm_min, g_edit_alarm_enabled ? "ON" : "OFF");
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 38, buf, 3, RGDS_RGB(248, 212, 108));
    } else if (now_ms < g_toast_until_ms && g_toast_text[0]) {
        int tw = rgds_text_width(g_toast_text, 3);
        int bw = tw + 48;
        int bx = (NATIVE_W - bw) / 2;
        rgds_fill_rect(RGDS_SCREEN_TOP, bx, 24, bw, 50, RGDS_RGB(20, 30, 48));
        rgds_draw_rect(RGDS_SCREEN_TOP, bx, 24, bw, 50, 2, RGDS_RGB(88, 165, 235));
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 39, g_toast_text, 3, RGDS_RGB(220, 240, 255));
    }
}

/* ============================================================================
 * 7. BOTTOM SCREEN RENDERERS (DATE/STATUS, ALARM SET, HELP, & SCREEN-OFF)
 * ============================================================================ */

static const char * const k_weekdays[7] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
};

static const char * const k_months[12] = {
    "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
    "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
};

static void draw_pill_button(rgds_screen_t scr, int x, int y, int w, int h,
                             const char *key_lbl, const char *desc_lbl,
                             uint32_t badge_col, uint32_t border_col) {
    rgds_fill_rect(scr, x, y, w, h, RGDS_RGB(20, 28, 44));
    rgds_draw_rect(scr, x, y, w, h, 2, border_col);

    int kw = rgds_text_width(key_lbl, 2) + 20;
    rgds_fill_rect(scr, x + 8, y + 8, kw, h - 16, badge_col);
    rgds_draw_text(scr, x + 18, y + (h - 14) / 2, key_lbl, 2, RGDS_RGB(12, 18, 28));
    rgds_draw_text(scr, x + kw + 18, y + (h - 14) / 2, desc_lbl, 2, RGDS_RGB(225, 235, 250));
}

static void render_bottom_normal_date(const struct tm *shifted_tm, const struct tm *dev_tm, uint64_t now_ms) {
    rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(10, 14, 22));

    /* Top Header Bar with 5-second sleep countdown bar */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, NATIVE_W, 64, RGDS_RGB(18, 26, 42));
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 62, NATIVE_W, 2, RGDS_RGB(64, 110, 170));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, 36, 22, "NIGHT CLOCK  -  DATE & CONTROLS", 3, RGDS_RGB(130, 205, 255));

    uint64_t rem_ms = (g_bottom_wake_until_ms > now_ms) ? (g_bottom_wake_until_ms - now_ms) : 0;
    if (rem_ms > BOTTOM_WAKE_DURATION_MS) rem_ms = BOTTOM_WAKE_DURATION_MS;
    char sleep_lbl[48];
    snprintf(sleep_lbl, sizeof(sleep_lbl), "AUTO-OFF IN %u.%us",
             (unsigned)(rem_ms / 1000ULL), (unsigned)((rem_ms % 1000ULL) / 100ULL));
    int sl_w = rgds_text_width(sleep_lbl, 2);
    rgds_draw_text(RGDS_SCREEN_BOTTOM, NATIVE_W - sl_w - 36, 25, sleep_lbl, 2, RGDS_RGB(150, 175, 210));

    /* 1. Large Central Date Card (in Shifted Timezone) */
    int card_x = 36, card_y = 88, card_w = NATIVE_W - 72, card_h = 260;
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, card_x, card_y, card_w, card_h, RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, card_x, card_y, card_w, card_h, 2, RGDS_RGB(58, 86, 132));

    int wday = (shifted_tm->tm_wday >= 0 && shifted_tm->tm_wday < 7) ? shifted_tm->tm_wday : 0;
    int mon  = (shifted_tm->tm_mon  >= 0 && shifted_tm->tm_mon  < 12) ? shifted_tm->tm_mon  : 0;
    int mday = shifted_tm->tm_mday;
    int year = shifted_tm->tm_year + 1900;

    /* Day of Week (scale 6 -> 42px tall) */
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, card_y + 34, k_weekdays[wday], 6, RGDS_RGB(248, 212, 108));

    /* Full Month Day, Year (scale 7 -> 49px tall) */
    char date_str[96];
    snprintf(date_str, sizeof(date_str), "%s %d, %04d", k_months[mon], mday, year);
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, card_y + 112, date_str, 6, RGDS_COLOR_WHITE);

    /* ISO Date & Shifted vs. Device Time Comparison */
    char tz_buf[32];
    format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
    char iso_line[128];
    snprintf(iso_line, sizeof(iso_line), "%04d-%02d-%02d   |   CLOCK: %02d:%02d (%s)   |   DEVICE: %02d:%02d",
             year, mon + 1, mday,
             shifted_tm->tm_hour % 24, shifted_tm->tm_min % 60,
             tz_buf,
             dev_tm->tm_hour % 24, dev_tm->tm_min % 60);
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, card_y + 204, iso_line, 2, RGDS_RGB(145, 175, 215));

    /* 2. Three Status Cards: Timezone Shift, Alarm Status, Brightness */
    int sc_y = 372, sc_h = 190, sc_w = 300, sc_gap = 26;
    int sc1_x = 36;
    int sc2_x = sc1_x + sc_w + sc_gap;
    int sc3_x = sc2_x + sc_w + sc_gap;

    /* Card 1: Local Timezone Shift */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, sc1_x, sc_y, sc_w, sc_h, RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, sc1_x, sc_y, sc_w, sc_h, 2, RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc1_x + 20, sc_y + 20, "TIMEZONE SHIFT", 2, RGDS_RGB(140, 165, 205));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc1_x + 20, sc_y + 62, tz_buf, 4, RGDS_RGB(88, 215, 255));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc1_x + 20, sc_y + 124, "D-PAD LEFT / RIGHT", 2, RGDS_RGB(120, 145, 180));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc1_x + 20, sc_y + 150, "STEPS OF +/- 30 MIN", 2, RGDS_RGB(120, 145, 180));

    /* Card 2: Single Alarm (in Shifted TZ) */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, sc2_x, sc_y, sc_w, sc_h, RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, sc2_x, sc_y, sc_w, sc_h, 2,
                   g_alarm_enabled ? RGDS_RGB(72, 219, 136) : RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc2_x + 20, sc_y + 20, "ALARM (SHIFTED TZ)", 2, RGDS_RGB(140, 165, 205));
    char alrm_time[32];
    snprintf(alrm_time, sizeof(alrm_time), "%02d:%02d", g_alarm_hour, g_alarm_min);
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc2_x + 20, sc_y + 58, alrm_time, 5,
                   g_alarm_enabled ? RGDS_RGB(72, 219, 136) : RGDS_RGB(120, 135, 160));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc2_x + 195, sc_y + 72,
                   g_alarm_enabled ? "[ON]" : "[OFF]", 3,
                   g_alarm_enabled ? RGDS_RGB(72, 219, 136) : RGDS_RGB(120, 135, 160));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc2_x + 20, sc_y + 138, "PRESS [A] TO SET ALARM", 2, RGDS_RGB(145, 175, 215));

    /* Card 3: Brightness */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, sc3_x, sc_y, sc_w, sc_h, RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, sc3_x, sc_y, sc_w, sc_h, 2, RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc3_x + 20, sc_y + 20, "BRIGHTNESS", 2, RGDS_RGB(140, 165, 205));
    char br_str[32];
    snprintf(br_str, sizeof(br_str), "%d / 10", g_brightness);
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc3_x + 20, sc_y + 58, br_str, 4, RGDS_RGB(248, 212, 108));

    /* 10-segment brightness bar */
    for (int i = 0; i < 10; i++) {
        int bx = sc3_x + 20 + i * 26;
        uint32_t bc = (i < g_brightness) ? RGDS_RGB(248, 212, 108) : RGDS_RGB(34, 46, 68);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, bx, sc_y + 112, 20, 18, bc);
    }
    rgds_draw_text(RGDS_SCREEN_BOTTOM, sc3_x + 20, sc_y + 144, "D-PAD UP / DOWN", 2, RGDS_RGB(120, 145, 180));

    /* 3. Bottom Critical Button Mappings Bar (2x2 Grid of Prominent Pills) */
    int btn_y1 = 588, btn_y2 = 664;
    int btn_w  = 462, btn_h  = 60;
    int col1_x = 36,  col2_x = 526;

    draw_pill_button(RGDS_SCREEN_BOTTOM, col1_x, btn_y1, btn_w, btn_h,
                     "A", "SET / ARM ALARM", RGDS_RGB(72, 219, 136), RGDS_RGB(58, 125, 95));
    draw_pill_button(RGDS_SCREEN_BOTTOM, col2_x, btn_y1, btn_w, btn_h,
                     "UP/DN", "BRIGHTNESS SET", RGDS_RGB(248, 212, 108), RGDS_RGB(130, 115, 65));
    draw_pill_button(RGDS_SCREEN_BOTTOM, col1_x, btn_y2, btn_w, btn_h,
                     "X", g_is_analog ? "SWITCH TO DIGITAL" : "SWITCH TO ANALOG",
                     RGDS_RGB(88, 205, 255), RGDS_RGB(58, 110, 155));
    draw_pill_button(RGDS_SCREEN_BOTTOM, col2_x, btn_y2, btn_w, btn_h,
                     "R1", "SHOW ALL CONTROLS (HELP)", RGDS_RGB(255, 155, 75), RGDS_RGB(145, 95, 55));
}

static void render_bottom_alarm_set(const struct tm *shifted_tm) {
    rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(12, 16, 26));

    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, NATIVE_W, 68, RGDS_RGB(24, 36, 58));
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 66, NATIVE_W, 2, RGDS_RGB(88, 205, 255));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, 36, 22, "SET ALARM  (EVALUATED IN SHIFTED TIMEZONE)", 3, RGDS_RGB(248, 212, 108));

    char tz_buf[32];
    format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
    char cur_str[96];
    snprintf(cur_str, sizeof(cur_str), "CURRENT CLOCK TIME (%s):  %02d:%02d",
             tz_buf, shifted_tm->tm_hour % 24, shifted_tm->tm_min % 60);
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 104, cur_str, 3, RGDS_RGB(155, 185, 225));

    /* Three interactive selector boxes: HOUR | MINUTE | ON/OFF */
    int box_y = 176, box_h = 240;
    int box_w = 250;
    int hr_x  = 112;
    int mn_x  = 402;
    int st_x  = 692;

    /* Box 0: HOUR (00..23) */
    bool sel_hr = (g_alarm_field == ALARM_FIELD_HOUR);
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, hr_x, box_y, box_w, box_h,
                   sel_hr ? RGDS_RGB(28, 46, 76) : RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, hr_x, box_y, box_w, box_h, sel_hr ? 4 : 2,
                   sel_hr ? RGDS_RGB(88, 215, 255) : RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, hr_x + 68, box_y + 24, "HOUR (24H)", 2, RGDS_RGB(150, 180, 220));
    char hr_str[16];
    snprintf(hr_str, sizeof(hr_str), "%02d", g_edit_alarm_hour);
    rgds_draw_text(RGDS_SCREEN_BOTTOM, hr_x + 68, box_y + 88, hr_str, 10, RGDS_COLOR_WHITE);
    if (sel_hr) {
        rgds_draw_text(RGDS_SCREEN_BOTTOM, hr_x + 48, box_y + 196, "[UP/DN] ADJUST", 2, RGDS_RGB(88, 215, 255));
    }

    /* Colon between Hour and Minute */
    rgds_draw_text(RGDS_SCREEN_BOTTOM, hr_x + box_w + 10, box_y + 92, ":", 9, RGDS_RGB(150, 180, 220));

    /* Box 1: MINUTE (00..59) */
    bool sel_mn = (g_alarm_field == ALARM_FIELD_MINUTE);
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, mn_x, box_y, box_w, box_h,
                   sel_mn ? RGDS_RGB(28, 46, 76) : RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, mn_x, box_y, box_w, box_h, sel_mn ? 4 : 2,
                   sel_mn ? RGDS_RGB(88, 215, 255) : RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, mn_x + 78, box_y + 24, "MINUTE", 2, RGDS_RGB(150, 180, 220));
    char mn_str[16];
    snprintf(mn_str, sizeof(mn_str), "%02d", g_edit_alarm_min);
    rgds_draw_text(RGDS_SCREEN_BOTTOM, mn_x + 68, box_y + 88, mn_str, 10, RGDS_COLOR_WHITE);
    if (sel_mn) {
        rgds_draw_text(RGDS_SCREEN_BOTTOM, mn_x + 48, box_y + 196, "[UP/DN] ADJUST", 2, RGDS_RGB(88, 215, 255));
    }

    /* Box 2: STATUS (ON / OFF) */
    bool sel_st = (g_alarm_field == ALARM_FIELD_STATE);
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, st_x, box_y, 220, box_h,
                   sel_st ? RGDS_RGB(28, 46, 76) : RGDS_RGB(16, 23, 36));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, st_x, box_y, 220, box_h, sel_st ? 4 : 2,
                   sel_st ? RGDS_RGB(88, 215, 255) : RGDS_RGB(58, 86, 132));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, st_x + 68, box_y + 24, "STATUS", 2, RGDS_RGB(150, 180, 220));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, st_x + (g_edit_alarm_enabled ? 58 : 42), box_y + 98,
                   g_edit_alarm_enabled ? "ON" : "OFF", 8,
                   g_edit_alarm_enabled ? RGDS_RGB(72, 219, 136) : RGDS_RGB(246, 88, 78));
    if (sel_st) {
        rgds_draw_text(RGDS_SCREEN_BOTTOM, st_x + 34, box_y + 196, "[UP/DN] TOGGLE", 2, RGDS_RGB(88, 215, 255));
    }

    /* Confirmation Note */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 76, 448, NATIVE_W - 152, 82, RGDS_RGB(20, 32, 48));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, 76, 448, NATIVE_W - 152, 82, 2, RGDS_RGB(72, 219, 136));
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 466,
                            "PRESS [A] TO SAVE ALARM & PLAY LOUD CONFIRMATION SOUND", 2, RGDS_RGB(72, 219, 136));
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 496,
                            "PRESS [B] TO DISABLE ALARM & CANCEL", 2, RGDS_RGB(185, 205, 230));

    /* Bottom Controls in Alarm Set Mode */
    int btn_y1 = 564, btn_y2 = 644, btn_w = 462, btn_h = 60;
    draw_pill_button(RGDS_SCREEN_BOTTOM, 36,  btn_y1, btn_w, btn_h,
                     "L / R", "SELECT HOUR / MIN / ON-OFF", RGDS_RGB(88, 205, 255), RGDS_RGB(58, 110, 155));
    draw_pill_button(RGDS_SCREEN_BOTTOM, 526, btn_y1, btn_w, btn_h,
                     "UP/DN", "CHANGE VALUE (HOLD=FAST)", RGDS_RGB(248, 212, 108), RGDS_RGB(130, 115, 65));
    draw_pill_button(RGDS_SCREEN_BOTTOM, 36,  btn_y2, btn_w, btn_h,
                     "A", "CONFIRM & TEST ALARM SOUND", RGDS_RGB(72, 219, 136), RGDS_RGB(58, 125, 95));
    draw_pill_button(RGDS_SCREEN_BOTTOM, 526, btn_y2, btn_w, btn_h,
                     "R1", "SHOW HELP", RGDS_RGB(255, 155, 75), RGDS_RGB(145, 95, 55));
}

static void render_bottom_help(void) {
    rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(10, 15, 24));

    /* Header */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, NATIVE_W, 68, RGDS_RGB(24, 36, 58));
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 66, NATIVE_W, 2, RGDS_RGB(255, 155, 75));
    rgds_draw_text(RGDS_SCREEN_BOTTOM, 36, 22, "NIGHT CLOCK  -  HELP & ALL BUTTON MAPPINGS", 3, RGDS_RGB(255, 185, 110));

    int table_x = 36, table_y = 90, table_w = NATIVE_W - 72;
    static const struct {
        const char *btn;
        const char *action;
    } k_help_rows[] = {
        { "D-PAD LEFT / RIGHT", "Shift local app timezone (-30m / +30m, saved on boot)" },
        { "D-PAD UP / DOWN",    "Adjust screen & clock brightness (1..10, hold=repeat)" },
        { "X BUTTON",           "Switch Top Screen: Retro LED Digital / Minimal Analog" },
        { "A BUTTON",           "Open Alarm Set Mode; press A again to save & test" },
        { "B BUTTON",           "Disable / cancel in Alarm Set Mode (or wake screen)" },
        { "R1 (RIGHT SHOULDER)","Toggle this Help screen ON / OFF" },
        { "L1 / R2 (IN ALARM)", "Step alarm minutes by -5 / +5 mins while setting" },
        { "VOL+ / VOL-",        "Adjust system speaker volume" },
        { "ANY BUTTON PRESS",   "Performs action immediately & wakes bottom screen (5s)" },
        { "START / SELECT",     "Save configuration & exit Night Clock" }
    };

    int num_rows = (int)(sizeof(k_help_rows) / sizeof(k_help_rows[0]));
    int row_h = 54;
    for (int i = 0; i < num_rows; i++) {
        int ry = table_y + i * row_h;
        uint32_t row_bg = (i % 2 == 0) ? RGDS_RGB(16, 24, 38) : RGDS_RGB(22, 32, 50);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, table_x, ry, table_w, row_h - 4, row_bg);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, table_x, ry, table_w, row_h - 4, 1, RGDS_RGB(48, 70, 108));

        rgds_fill_rect(RGDS_SCREEN_BOTTOM, table_x + 10, ry + 8, 250, row_h - 20, RGDS_RGB(32, 52, 84));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, table_x + 20, ry + 18, k_help_rows[i].btn, 2, RGDS_RGB(248, 212, 108));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, table_x + 280, ry + 18, k_help_rows[i].action, 2, RGDS_RGB(225, 235, 250));
    }

    /* Footer dismiss banner */
    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 36, 656, NATIVE_W - 72, 76, RGDS_RGB(26, 40, 64));
    rgds_draw_rect(RGDS_SCREEN_BOTTOM, 36, 656, NATIVE_W - 72, 76, 2, RGDS_RGB(255, 155, 75));
    rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 682,
                            "PRESS [R1] (RIGHT SHOULDER) AGAIN TO DISMISS HELP", 3, RGDS_RGB(255, 195, 120));
}

static void render_bottom_screen(const struct tm *shifted_tm, const struct tm *dev_tm, uint64_t now_ms) {
    if (!g_bottom_awake && g_ui_mode == UI_MODE_NORMAL && !g_alarm_ringing) {
#ifdef HOST_X11
        /*
         * On Linux X11 desktop simulator, mimic the hardware bottom-screen power-off
         * state by rendering a dark screen with a clear "SCREEN OFF" overlay.
         */
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(4, 6, 10));
        int bw = 640, bh = 220;
        int bx = (NATIVE_W - bw) / 2;
        int by = (NATIVE_H - bh) / 2;
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, bx, by, bw, bh, RGDS_RGB(10, 14, 22));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, bx, by, bw, bh, 2, RGDS_RGB(42, 58, 84));
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, by + 54, "SCREEN OFF", 6, RGDS_RGB(95, 118, 152));
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, by + 132,
                                "BOTTOM SCREEN BACKLIGHT IS ASLEEP", 2, RGDS_RGB(72, 92, 122));
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, by + 164,
                                "PRESS ANY BUTTON TO PERFORM ACTION & WAKE FOR 5S", 2, RGDS_RGB(72, 92, 122));
#else
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_COLOR_BLACK);
#endif
        return;
    }

    if (g_ui_mode == UI_MODE_HELP) {
        render_bottom_help();
    } else if (g_ui_mode == UI_MODE_ALARM_SET) {
        render_bottom_alarm_set(shifted_tm);
    } else {
        render_bottom_normal_date(shifted_tm, dev_tm, now_ms);
    }
}

/* ============================================================================
 * 8. INPUT HANDLING & CONTINUOUS HOLD AUTO-REPEAT
 * ============================================================================ */

typedef struct {
    uint32_t mask;
    uint64_t next_repeat_ms;
} repeat_tracker_t;

static bool check_button_with_repeat(const rgds_input_state_t *in, repeat_tracker_t *rt, uint64_t now_ms) {
    if (in->pressed & rt->mask) {
        rt->next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
        return true;
    }
    if ((in->held & rt->mask) && rt->next_repeat_ms != 0 && now_ms >= rt->next_repeat_ms) {
        rt->next_repeat_ms = now_ms + REPEAT_INTERVAL_MS;
        return true;
    }
    if (!(in->held & rt->mask)) {
        rt->next_repeat_ms = 0;
    }
    return false;
}

/* ============================================================================
 * 9. MAIN LOOP
 * ============================================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

#ifndef HOST_X11
    g_lcd_swap_flag = sysfs_read_int("/sys/class/anbernic_misc/lcdswap", 0);
#endif

    load_config();

    if (rgds_init("Anbernic RG DS Plus Night Clock") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK.\n");
        return 1;
    }

    /* Render directly into the dual 1024x768 native framebuffers */
    rgds_set_direct_native_mode(true);

    apply_hardware_brightness(g_brightness);
    audio_init();

    g_boot_mono_ms = rgds_ticks_ms();
    wake_bottom_screen(g_boot_mono_ms);

    repeat_tracker_t rep_left  = { RGDS_BTN_LEFT,  0 };
    repeat_tracker_t rep_right = { RGDS_BTN_RIGHT, 0 };
    repeat_tracker_t rep_up    = { RGDS_BTN_UP,    0 };
    repeat_tracker_t rep_down  = { RGDS_BTN_DOWN,  0 };

    while (!g_sig_quit) {
        rgds_input_state_t input;
        if (!rgds_poll_input(&input)) {
            break;
        }
        if ((input.pressed & (RGDS_BTN_START | RGDS_BTN_SELECT | RGDS_BTN_MENU)) != 0) {
            break;
        }

        uint64_t now_ms = rgds_ticks_ms();

        /* Restore volume after confirmation tone completes */
        if (g_confirm_samples_rem <= 0 && !g_alarm_ringing && g_saved_vol_before_alrm >= 0) {
            rgds_set_volume(g_saved_vol_before_alrm);
            g_saved_vol_before_alrm = -1;
        }

        /* Any button press or touch wakes the bottom screen for 5 seconds */
        bool any_input = (input.pressed != 0) || (input.held != 0) ||
                         input.touch.just_pressed || input.touch.touching;
        if (any_input) {
            wake_bottom_screen(now_ms);
        }

        /* If the alarm is currently ringing, any button press or touch dismisses it */
        if (g_alarm_ringing) {
            if (input.pressed != 0 || input.touch.just_pressed) {
                g_alarm_ringing = false;
                if (g_saved_vol_before_alrm >= 0) {
                    rgds_set_volume(g_saved_vol_before_alrm);
                    g_saved_vol_before_alrm = -1;
                }
                show_toast("ALARM DISMISSED", now_ms);
                /* Consume this press so dismissing the alarm doesn't trigger another action */
                input.pressed = 0;
            } else if (now_ms - g_alarm_ring_start_ms > 300000ULL) {
                /* Auto-silence after 5 minutes */
                g_alarm_ringing = false;
            }
        }

        /* Right Shoulder (R1) toggles the Help screen from any mode.
         * On the Linux X11 desktop simulator, the 'r' / 'R' key maps to RGDS_BTN_Y,
         * so allow that to toggle the Help screen on desktop as well.
         */
#ifdef HOST_X11
#define HELP_TOGGLE_MASK (RGDS_BTN_R1 | RGDS_BTN_Y)
#else
#define HELP_TOGGLE_MASK RGDS_BTN_R1
#endif
        if (input.pressed & HELP_TOGGLE_MASK) {
            if (g_ui_mode == UI_MODE_HELP) {
                g_ui_mode = g_mode_before_help;
                wake_bottom_screen(now_ms);
            } else {
                g_mode_before_help = g_ui_mode;
                g_ui_mode = UI_MODE_HELP;
                wake_bottom_screen(now_ms);
            }
            input.pressed &= ~HELP_TOGGLE_MASK;
        }

        if (g_ui_mode == UI_MODE_HELP) {
            /* In Help mode, B or A also dismisses Help */
            if (input.pressed & (RGDS_BTN_B | RGDS_BTN_A)) {
                g_ui_mode = g_mode_before_help;
                wake_bottom_screen(now_ms);
            }
            wake_bottom_screen(now_ms);
        } else if (g_ui_mode == UI_MODE_ALARM_SET) {
            wake_bottom_screen(now_ms);

            if (input.pressed & RGDS_BTN_LEFT) {
                g_alarm_field = (alarm_field_t)((g_alarm_field + 2) % 3);
            } else if (input.pressed & RGDS_BTN_RIGHT) {
                g_alarm_field = (alarm_field_t)((g_alarm_field + 1) % 3);
            }

            bool step_up   = check_button_with_repeat(&input, &rep_up,   now_ms);
            bool step_down = check_button_with_repeat(&input, &rep_down, now_ms);

            if (step_up || step_down) {
                int delta = step_up ? 1 : -1;
                if (g_alarm_field == ALARM_FIELD_HOUR) {
                    g_edit_alarm_hour = (g_edit_alarm_hour + delta + 24) % 24;
                } else if (g_alarm_field == ALARM_FIELD_MINUTE) {
                    g_edit_alarm_min = (g_edit_alarm_min + delta + 60) % 60;
                } else {
                    g_edit_alarm_enabled = !g_edit_alarm_enabled;
                }
            }

            /* Fast +/-5 minute step with L1/L2 or R2 */
            if (input.pressed & (RGDS_BTN_L1 | RGDS_BTN_L2)) {
                g_edit_alarm_min = (g_edit_alarm_min - 5 + 60) % 60;
            } else if (input.pressed & RGDS_BTN_R2) {
                g_edit_alarm_min = (g_edit_alarm_min + 5) % 60;
            }

            if (input.pressed & RGDS_BTN_A) {
                /* Save & arm the alarm, then play loud confirmation sound */
                g_alarm_hour    = g_edit_alarm_hour;
                g_alarm_min     = g_edit_alarm_min;
                g_alarm_enabled = g_edit_alarm_enabled;
                save_config();
                g_ui_mode = UI_MODE_NORMAL;
                wake_bottom_screen(now_ms);

                if (g_alarm_enabled) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "ALARM SET FOR %02d:%02d", g_alarm_hour, g_alarm_min);
                    show_toast(msg, now_ms);
                    trigger_loud_confirmation_sound();
                } else {
                    show_toast("ALARM TURNED OFF", now_ms);
                }
            } else if (input.pressed & RGDS_BTN_B) {
                g_alarm_enabled = false;
                save_config();
                g_ui_mode = UI_MODE_NORMAL;
                wake_bottom_screen(now_ms);
                show_toast("ALARM OFF", now_ms);
            }
        } else {
            /* UI_MODE_NORMAL: First button press performs action AND wakes bottom screen */

            /* (0) LEFT / RIGHT: Adjust local timezone shift in 30-minute increments */
            bool tz_dec = check_button_with_repeat(&input, &rep_left,  now_ms);
            bool tz_inc = check_button_with_repeat(&input, &rep_right, now_ms);
            if (tz_dec || tz_inc) {
                int next_tz = g_tz_shift_mins + (tz_inc ? 30 : -30);
                if (next_tz < -1440) next_tz = -1440;
                if (next_tz >  1440) next_tz =  1440;
                if (next_tz != g_tz_shift_mins) {
                    g_tz_shift_mins = next_tz;
                    save_config();
                }
                char tz_buf[32], msg[64];
                format_tz_shift(tz_buf, sizeof(tz_buf), g_tz_shift_mins);
                snprintf(msg, sizeof(msg), "TIMEZONE SHIFT: %s", tz_buf);
                show_toast(msg, now_ms);
            }

            /* (3a) UP / DOWN: Adjust brightness (1..10) */
            bool br_up   = check_button_with_repeat(&input, &rep_up,   now_ms);
            bool br_down = check_button_with_repeat(&input, &rep_down, now_ms);
            if (br_up || br_down) {
                int next_br = g_brightness + (br_up ? 1 : -1);
                if (next_br < 1)  next_br = 1;
                if (next_br > 10) next_br = 10;
                if (next_br != g_brightness) {
                    g_brightness = next_br;
                    apply_hardware_brightness(g_brightness);
                    save_config();
                }
                char msg[64];
                snprintf(msg, sizeof(msg), "BRIGHTNESS: %d / 10", g_brightness);
                show_toast(msg, now_ms);
            }

            /* (1) X Key: Switch between Retro LED Digital and Minimal Analog */
            if (input.pressed & RGDS_BTN_X) {
                g_is_analog = !g_is_analog;
                save_config();
                show_toast(g_is_analog ? "ANALOG CLOCK MODE" : "DIGITAL CLOCK MODE (24H)", now_ms);
            }

            /* (3b) A Key: Enter Alarm Set Mode */
            if (input.pressed & RGDS_BTN_A) {
                g_edit_alarm_hour    = g_alarm_hour;
                g_edit_alarm_min     = g_alarm_min;
                g_edit_alarm_enabled = true;
                g_alarm_field        = ALARM_FIELD_HOUR;
                g_ui_mode            = UI_MODE_ALARM_SET;
                wake_bottom_screen(now_ms);
            }
        }

        /* Compute Device Time and Shifted Time (device_time + g_tz_shift_mins) */
        time_t raw_now = time(NULL);
        struct tm dev_tm;
        localtime_r(&raw_now, &dev_tm);

        struct tm shifted_tm = dev_tm;
        shifted_tm.tm_min  += g_tz_shift_mins;
        shifted_tm.tm_isdst = -1;
        mktime(&shifted_tm);

        /* Check if Alarm should trigger in the Shifted Timezone */
        int cur_min_key = (shifted_tm.tm_year * 530000) + (shifted_tm.tm_yday * 1440) +
                          (shifted_tm.tm_hour * 60) + shifted_tm.tm_min;
        if (g_alarm_enabled && g_ui_mode != UI_MODE_ALARM_SET &&
            shifted_tm.tm_hour == g_alarm_hour &&
            shifted_tm.tm_min  == g_alarm_min  &&
            cur_min_key != g_last_alarm_min_key) {
            g_last_alarm_min_key = cur_min_key;
            g_alarm_ringing      = true;
            g_alarm_ring_start_ms = now_ms;
            int cur_vol = rgds_get_volume();
            if (cur_vol < 10) {
                g_saved_vol_before_alrm = cur_vol;
                rgds_set_volume(10);
            }
            wake_bottom_screen(now_ms);
        }

        /* Auto-sleep the bottom screen after 5 seconds of inactivity in Normal Mode */
        if (g_bottom_awake && g_ui_mode == UI_MODE_NORMAL && !g_alarm_ringing &&
            now_ms >= g_bottom_wake_until_ms) {
            g_bottom_awake = false;
            set_bottom_screen_hw_power(false);
        }

        /* Render both 1024x768 screens and present */
        render_top_screen(&shifted_tm, now_ms);
        render_bottom_screen(&shifted_tm, &dev_tm, now_ms);
        rgds_present();

        rgds_delay_ms(16);
    }

    /* Restore bottom screen power & volume before exiting */
    set_bottom_screen_hw_power(true);
    if (g_saved_vol_before_alrm >= 0) {
        rgds_set_volume(g_saved_vol_before_alrm);
    }
    save_config();
    audio_shutdown();
    rgds_shutdown();
    return 0;
}
