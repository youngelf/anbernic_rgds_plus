/*
 * metronome.c - Dual-Screen Precision Metronome for Anbernic RG DS Plus & Linux Host
 *               Built on the Anbernic RG DS Plus Native C SDK (`rgds_sdk.h`)
 *
 * Dual-Screen Layout (Each Screen is 1024x768 on RG DS Plus, 320x240 logical canvas):
 *   - UPPER SCREEN (RGDS_SCREEN_TOP):
 *       Active Metronome (BPM, Classical Tempo Marking, Time Signature,
 *       Beat Visualizer Boxes with Beat 1 Accent, Swinging Pendulum, Volume Toast)
 *   - LOWER SCREEN (RGDS_SCREEN_BOTTOM):
 *       Overall & Session Practice Statistics (Session Time, Session Beats & Bars,
 *       Lifetime Practice Time, Lifetime Beats & Bars, Min/Max/Avg BPM, and
 *       Per-Time-Signature Practice Breakdown)
 *
 * Controls on Anbernic RG DS Plus & Linux Desktop Simulator:
 *   - A Button (Space / 'A')          : Play / Pause metronome
 *   - B Button (while playing)        : Pause metronome
 *   - B Button (while paused)         : Reset current session counters (keeps lifetime stats)
 *   - D-Pad UP / DOWN                 : Increase / Decrease Tempo (+/-1 BPM, hold for continuous repeat)
 *   - D-Pad LEFT / RIGHT              : Cycle Time Signature (4/4, 3/4, 2/4, 5/4, 7/4)
 *   - L1 / L2 & R1 / R2 Shoulders     : Jump Tempo -5 / +5 BPM
 *   - VOL+ / VOL-                     : Adjust system & click volume continuously (0..10)
 *   - START or SELECT (or MENU / ESC) : Save stats & Exit
 */

#include "rgds_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ============================================================================
 * 1. METRONOME CONSTANTS & STATE
 * ============================================================================ */

#define SAMPLE_RATE             44100
#define AUDIO_CHANNELS          2

#define MIN_BPM                 30
#define MAX_BPM                 260
#define DEFAULT_BPM             120

#define MIN_VOLUME              0
#define MAX_VOLUME              10
#define DEFAULT_VOLUME          10

/* Continuous auto-repeat timing for holding D-Pad UP / DOWN */
#define REPEAT_INITIAL_DELAY_MS 320ULL
#define REPEAT_INTERVAL_MS      65ULL

/* Time Signatures: 4/4, 3/4, 2/4, 5/4, 7/4 */
#define NUM_TIME_SIGS           5
static const int   k_time_sig_beats[NUM_TIME_SIGS] = { 4, 3, 2, 5, 7 };
static const char *k_time_sig_names[NUM_TIME_SIGS] = { "4/4", "3/4", "2/4", "5/4", "7/4" };

static volatile int  g_bpm          = DEFAULT_BPM;
static volatile int  g_time_sig_idx = 0;
static volatile bool g_playing      = false;
static volatile int  g_volume       = DEFAULT_VOLUME; /* 0 (Mute) .. 10 (100%) */
static bool          g_vol_from_cfg = false;

/* ============================================================================
 * 2. CLICK SYNTHESIZER & AUDIO OUTPUT (SDL2 Audio / PulseAudio / ALSA)
 * ============================================================================ */

#define ACCENT_CLICK_MS 75
#define NORMAL_CLICK_MS 55

static volatile int  g_click_samples_left = 0;
static volatile bool g_click_is_accent    = false;
static uint32_t      g_click_sample_pos   = 0;

static void trigger_click(bool accent) {
    g_click_is_accent    = accent;
    g_click_sample_pos   = 0;
    int duration_ms      = accent ? ACCENT_CLICK_MS : NORMAL_CLICK_MS;
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

/* Audio backend state */
static pthread_t     g_audio_thread;
static volatile bool g_audio_thread_running = false;
static bool          g_use_sdl2_audio       = false;
static void         *g_sdl2_audio_lib       = NULL;
static void        (*fn_SDL_CloseAudio)(void) = NULL;

/* PulseAudio simple dynamically loaded types */
typedef struct pa_simple pa_simple;
typedef struct {
    int      format;
    uint32_t rate;
    uint8_t  channels;
} pa_sample_spec;

#define PA_STREAM_PLAYBACK 1
#define PA_SAMPLE_S16LE    3

typedef pa_simple* (*pa_simple_new_fn)(const char*, const char*, int, const char*,
                                       const char*, const pa_sample_spec*,
                                       const void*, const void*, int*);
typedef int  (*pa_simple_write_fn)(pa_simple*, const void*, size_t, int*);
typedef void (*pa_simple_free_fn)(pa_simple*);

/* ALSA dynamically loaded types */
typedef int  (*snd_pcm_open_fn)(void**, const char*, int, int);
typedef int  (*snd_pcm_set_params_fn)(void*, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*snd_pcm_writei_fn)(void*, const void*, unsigned long);
typedef int  (*snd_pcm_recover_fn)(void*, int, int);
typedef int  (*snd_pcm_close_fn)(void*);

static void *audio_worker_thread(void *arg) {
    (void)arg;

    /* 1. Try PulseAudio (libpulse-simple.so.0) first */
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    pa_simple *pa = NULL;
    pa_simple_new_fn   fn_pa_new   = NULL;
    pa_simple_write_fn fn_pa_write = NULL;
    pa_simple_free_fn  fn_pa_free  = NULL;

    if (pa_lib) {
        fn_pa_new   = (pa_simple_new_fn)dlsym(pa_lib, "pa_simple_new");
        fn_pa_write = (pa_simple_write_fn)dlsym(pa_lib, "pa_simple_write");
        fn_pa_free  = (pa_simple_free_fn)dlsym(pa_lib, "pa_simple_free");
        if (fn_pa_new && fn_pa_write && fn_pa_free) {
            pa_sample_spec ss = { PA_SAMPLE_S16LE, SAMPLE_RATE, AUDIO_CHANNELS };
            int err = 0;
            pa = fn_pa_new(NULL, "RG DS Plus Metronome", PA_STREAM_PLAYBACK,
                           NULL, "Metronome Click", &ss, NULL, NULL, &err);
        }
    }

    /* 2. If PulseAudio is unavailable, try ALSA (libasound.so.2) */
    void *asound_lib = NULL;
    void *pcm = NULL;
    snd_pcm_writei_fn  fn_alsa_writei  = NULL;
    snd_pcm_recover_fn fn_alsa_recover = NULL;
    snd_pcm_close_fn   fn_alsa_close   = NULL;

    if (!pa) {
        asound_lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (asound_lib) {
            snd_pcm_open_fn       fn_open   = (snd_pcm_open_fn)dlsym(asound_lib, "snd_pcm_open");
            snd_pcm_set_params_fn fn_params = (snd_pcm_set_params_fn)dlsym(asound_lib, "snd_pcm_set_params");
            fn_alsa_writei  = (snd_pcm_writei_fn)dlsym(asound_lib, "snd_pcm_writei");
            fn_alsa_recover = (snd_pcm_recover_fn)dlsym(asound_lib, "snd_pcm_recover");
            fn_alsa_close   = (snd_pcm_close_fn)dlsym(asound_lib, "snd_pcm_close");

            if (fn_open && fn_params && fn_alsa_writei && fn_alsa_close) {
                if (fn_open(&pcm, "default", 0, 0) < 0) {
                    if (fn_open(&pcm, "plughw:0,0", 0, 0) < 0) {
                        pcm = NULL;
                    }
                }
                if (pcm) {
                    fn_params(pcm, 2, 3, AUDIO_CHANNELS, SAMPLE_RATE, 1, 25000);
                }
            }
        }
    }

    int chunk_frames = 512;
    int chunk_bytes  = chunk_frames * AUDIO_CHANNELS * 2;
    int16_t buf[512 * AUDIO_CHANNELS];

    while (g_audio_thread_running) {
        audio_callback(NULL, (uint8_t *)buf, chunk_bytes);
        if (pa && fn_pa_write) {
            int err = 0;
            fn_pa_write(pa, buf, (size_t)chunk_bytes, &err);
        } else if (pcm && fn_alsa_writei) {
            long written = fn_alsa_writei(pcm, buf, (unsigned long)chunk_frames);
            if (written < 0 && fn_alsa_recover) {
                fn_alsa_recover(pcm, (int)written, 1);
            }
        } else {
            rgds_delay_ms((uint32_t)((chunk_frames * 1000) / SAMPLE_RATE));
        }
    }

    if (pa && fn_pa_free) fn_pa_free(pa);
    if (pa_lib) dlclose(pa_lib);
    if (pcm && fn_alsa_close) fn_alsa_close(pcm);
    if (asound_lib) dlclose(asound_lib);
    return NULL;
}

#ifndef HOST_X11
#define SDL2_INIT_AUDIO   0x00000010U
#define SDL2_AUDIO_S16LSB 0x8010

typedef void (*SDL2_AudioCallback)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int                freq;
    uint16_t           format;
    uint8_t            channels;
    uint8_t            silence;
    uint16_t           samples;
    uint16_t           padding;
    uint32_t           size;
    SDL2_AudioCallback callback;
    void              *userdata;
} SDL2_AudioSpec;
#endif

static void audio_init(void) {
#ifndef HOST_X11
    /* On RG DS Plus hardware, try SDL2 audio first (already loaded by rgds_init) */
    g_sdl2_audio_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (g_sdl2_audio_lib) {
        int  (*fn_SDL_InitSubSystem)(uint32_t)               = dlsym(g_sdl2_audio_lib, "SDL_InitSubSystem");
        int  (*fn_SDL_OpenAudio)(SDL2_AudioSpec*, SDL2_AudioSpec*) = dlsym(g_sdl2_audio_lib, "SDL_OpenAudio");
        void (*fn_SDL_PauseAudio)(int)                       = dlsym(g_sdl2_audio_lib, "SDL_PauseAudio");
        fn_SDL_CloseAudio                                    = dlsym(g_sdl2_audio_lib, "SDL_CloseAudio");

        if (fn_SDL_InitSubSystem) {
            fn_SDL_InitSubSystem(SDL2_INIT_AUDIO);
        }
        if (fn_SDL_OpenAudio && fn_SDL_PauseAudio) {
            SDL2_AudioSpec desired;
            memset(&desired, 0, sizeof(desired));
            desired.freq     = SAMPLE_RATE;
            desired.format   = SDL2_AUDIO_S16LSB;
            desired.channels = AUDIO_CHANNELS;
            desired.samples  = 512;
            desired.callback = audio_callback;
            if (fn_SDL_OpenAudio(&desired, NULL) == 0) {
                fn_SDL_PauseAudio(0);
                g_use_sdl2_audio = true;
                return;
            }
        }
    }
#endif
    g_audio_thread_running = true;
    pthread_create(&g_audio_thread, NULL, audio_worker_thread, NULL);
}

static void audio_shutdown(void) {
    if (g_use_sdl2_audio && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
        g_use_sdl2_audio = false;
    }
    if (g_audio_thread_running) {
        g_audio_thread_running = false;
        pthread_join(g_audio_thread, NULL);
    }
    if (g_sdl2_audio_lib) {
        dlclose(g_sdl2_audio_lib);
        g_sdl2_audio_lib = NULL;
    }
}

/* ============================================================================
 * 3. PRACTICE STATISTICS & CONFIGURATION PERSISTENCE
 * ============================================================================ */

static char g_config_path[512];

/* Session stats (reset when app launches or when B is pressed while paused) */
static uint64_t g_session_ms    = 0;
static uint64_t g_session_beats = 0;
static uint64_t g_session_bars  = 0;

/* Lifetime persisted stats */
static uint64_t g_total_ms                = 0;
static uint64_t g_total_beats             = 0;
static uint64_t g_total_bars              = 0;
static uint64_t g_total_sessions          = 0;
static int      g_min_bpm_used            = 0;
static int      g_max_bpm_used            = 0;
static uint64_t g_bpm_weighted_sum        = 0;
static uint64_t g_sig_ms[NUM_TIME_SIGS]   = {0};
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
                g_vol_from_cfg = true;
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
        } else if (strncmp(line, "sig_ms_", 7) == 0 &&
                   line[7] >= '0' && line[7] < ('0' + NUM_TIME_SIGS) && line[8] == '=') {
            g_sig_ms[line[7] - '0'] = parse_u64(line + 9);
        } else if (strncmp(line, "sig_bars_", 9) == 0 &&
                   line[9] >= '0' && line[9] < ('0' + NUM_TIME_SIGS) && line[10] == '=') {
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

/* Helper to center text around an arbitrary horizontal coordinate `cx` using the SDK */
static void draw_text_at_center(rgds_screen_t scr, int cx, int y, const char *str, int scale, uint32_t color) {
    int width = rgds_text_width(str, scale);
    rgds_draw_text(scr, cx - width / 2, y, str, scale, color);
}

/* ============================================================================
 * 4. MAIN APPLICATION LOOP (USING RGDS_SDK)
 * ============================================================================ */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    resolve_config_path();
    load_config();

    if (rgds_init("Anbernic RG DS Plus Metronome") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }

    /* Synchronize saved volume with system volume (or adopt system volume if not in cfg) */
    if (g_vol_from_cfg) {
        rgds_set_volume(g_volume);
    } else {
        g_volume = rgds_get_volume();
    }

    audio_init();

    /* Color Palette */
    const uint32_t col_bg         = RGDS_RGB(14,  18,  28);
    const uint32_t col_panel      = RGDS_RGB(26,  33,  50);
    const uint32_t col_panel_dark = RGDS_RGB(20,  25,  38);
    const uint32_t col_border     = RGDS_RGB(65,  82, 120);
    const uint32_t col_white      = RGDS_RGB(245, 247, 252);
    const uint32_t col_muted      = RGDS_RGB(145, 160, 190);
    const uint32_t col_accent     = RGDS_RGB(255,  70,  70); /* Beat 1 Accent Red */
    const uint32_t col_beat       = RGDS_RGB(46,  213, 115); /* Normal Beat Green */
    const uint32_t col_gold       = RGDS_RGB(255, 200,  60); /* Time Signature Gold */
    const uint32_t col_cyan       = RGDS_RGB(70,  205, 255); /* Tempo / UI Highlights */
    const uint32_t col_purple     = RGDS_RGB(175, 120, 255); /* Lower Screen Stats Accent */
    const uint32_t col_box_idle   = RGDS_RGB(36,  44,  66);

    bool     counted_this_session = false;
    int      current_beat         = 0;
    double   next_beat_time_ms    = 0.0;
    uint64_t last_beat_trigger_ms = 0;
    uint64_t last_vol_change_ms   = 0;
    uint64_t last_frame_ms        = rgds_ticks_ms();
    uint64_t last_autosave_ms     = last_frame_ms;

    /* Continuous auto-repeat state for holding D-Pad UP/DOWN */
    int      dpad_repeat_dir      = 0; /* +1 = BPM+, -1 = BPM-, 0 = neutral */
    uint64_t dpad_next_repeat_ms  = 0;

    rgds_input_state_t input;
    while (rgds_poll_input(&input)) {
        uint64_t now_ms = rgds_ticks_ms();
        uint64_t dt_ms  = now_ms - last_frame_ms;
        last_frame_ms   = now_ms;

        /* Requirement (1): Exit immediately when SELECT or START (or MENU) is pressed */
        if ((input.pressed & (RGDS_BTN_SELECT | RGDS_BTN_START | RGDS_BTN_MENU)) != 0) {
            break;
        }

        if (g_playing && dt_ms > 0 && dt_ms < 1000) {
            g_session_ms             += dt_ms;
            g_total_ms               += dt_ms;
            g_sig_ms[g_time_sig_idx] += dt_ms;
        }

        /* Periodic autosave every 15 seconds while playing */
        if (g_playing && (now_ms - last_autosave_ms) >= 15000ULL) {
            save_config();
            last_autosave_ms = now_ms;
        }

        /* --------------------------------------------------------------------
         * Play / Pause (A button, or B button while playing) & Reset Session (B while paused)
         * -------------------------------------------------------------------- */
        bool toggle_play = (input.pressed & RGDS_BTN_A) != 0 ||
                           ((input.pressed & RGDS_BTN_B) != 0 && g_playing);
        if (toggle_play) {
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
        } else if ((input.pressed & RGDS_BTN_B) != 0 && !g_playing) {
            g_session_ms    = 0;
            g_session_beats = 0;
            g_session_bars  = 0;
        }

        /* --------------------------------------------------------------------
         * Requirement (2): Continuous Tempo Change by Holding D-Pad UP / DOWN
         * -------------------------------------------------------------------- */
        if (input.pressed & RGDS_BTN_UP) {
            if (g_bpm < MAX_BPM) {
                g_bpm++;
                save_config();
            }
            dpad_repeat_dir     = +1;
            dpad_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
        } else if (input.pressed & RGDS_BTN_DOWN) {
            if (g_bpm > MIN_BPM) {
                g_bpm--;
                save_config();
            }
            dpad_repeat_dir     = -1;
            dpad_next_repeat_ms = now_ms + REPEAT_INITIAL_DELAY_MS;
        }

        if ((dpad_repeat_dir > 0 && !(input.held & RGDS_BTN_UP)) ||
            (dpad_repeat_dir < 0 && !(input.held & RGDS_BTN_DOWN))) {
            dpad_repeat_dir = 0;
            save_config();
        } else if (dpad_repeat_dir != 0 && now_ms >= dpad_next_repeat_ms) {
            dpad_next_repeat_ms = now_ms + REPEAT_INTERVAL_MS;
            int next_bpm = g_bpm + dpad_repeat_dir;
            if (next_bpm < MIN_BPM) next_bpm = MIN_BPM;
            if (next_bpm > MAX_BPM) next_bpm = MAX_BPM;
            g_bpm = next_bpm;
        }

        /* Fast Tempo Jump (-5 / +5 BPM) via L1/L2 and R1/R2 shoulders */
        if (input.pressed & (RGDS_BTN_R1 | RGDS_BTN_R2)) {
            int next_bpm = g_bpm + 5;
            if (next_bpm > MAX_BPM) next_bpm = MAX_BPM;
            g_bpm = next_bpm;
            save_config();
        }
        if (input.pressed & (RGDS_BTN_L1 | RGDS_BTN_L2)) {
            int next_bpm = g_bpm - 5;
            if (next_bpm < MIN_BPM) next_bpm = MIN_BPM;
            g_bpm = next_bpm;
            save_config();
        }

        /* Cycle Time Signature via D-Pad LEFT / RIGHT */
        if (input.pressed & (RGDS_BTN_LEFT | RGDS_BTN_RIGHT)) {
            if (input.pressed & RGDS_BTN_LEFT) {
                g_time_sig_idx = (g_time_sig_idx + NUM_TIME_SIGS - 1) % NUM_TIME_SIGS;
            } else {
                g_time_sig_idx = (g_time_sig_idx + 1) % NUM_TIME_SIGS;
            }
            current_beat = 0;
            if (g_playing) {
                trigger_click(true);
                record_beat_stat(g_bpm, g_time_sig_idx, true);
                last_beat_trigger_ms = now_ms;
                next_beat_time_ms = (double)now_ms + (60000.0 / (double)g_bpm);
            }
            save_config();
        }

        /* --------------------------------------------------------------------
         * Requirement (3): Continuous System & Click Volume Control via VOL+ / VOL-
         * `rgds_poll_input()` in the SDK automatically steps and auto-repeats
         * system volume (/sys/class/anbernic_misc/openbor_volume + PulseAudio sinks)
         * while VOL+ or VOL- is held. Here we sync `g_volume` and show the toast.
         * -------------------------------------------------------------------- */
        int sdk_vol = rgds_get_volume();
        if (sdk_vol != g_volume) {
            g_volume = sdk_vol;
            last_vol_change_ms = now_ms;
            save_config();
        } else if ((input.pressed | input.held) & (RGDS_BTN_VOL_UP | RGDS_BTN_VOL_DOWN)) {
            last_vol_change_ms = now_ms;
        }

        /* --------------------------------------------------------------------
         * Metronome Beat Scheduler
         * -------------------------------------------------------------------- */
        now_ms = rgds_ticks_ms();
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

        /* ====================================================================
         * UPPER SCREEN: METRONOME DISPLAY (RGDS_SCREEN_TOP, 320x240 -> 1024x768)
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_TOP, col_bg);

        /* 1. Top Status Bar (y: 0..26) */
        bool vol_toast = (last_vol_change_ms != 0 && (now_ms - last_vol_change_ms) < 1400ULL);
        uint32_t top_bar_col = g_playing
            ? ((current_beat == 0 && (now_ms - last_beat_trigger_ms) < 120ULL) ? col_accent : col_panel)
            : col_panel_dark;
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 0, RGDS_CANVAS_W, 26, top_bar_col);
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 26, RGDS_CANVAS_W, 2, col_border);

        if (vol_toast) {
            char vol_banner[40];
            if (g_volume == 0) {
                snprintf(vol_banner, sizeof(vol_banner), "VOLUME: MUTE (0/10)");
            } else {
                snprintf(vol_banner, sizeof(vol_banner), "VOLUME: %d%% (%d/10)",
                         g_volume * 10, g_volume);
            }
            rgds_draw_text_centered(RGDS_SCREEN_TOP, 6, vol_banner, 2,
                                    (g_volume == 0) ? col_accent : col_cyan);
        } else {
            if (g_playing) {
                draw_text_at_center(RGDS_SCREEN_TOP, 134, 6, "METRONOME PLAYING", 2, col_white);
            } else {
                draw_text_at_center(RGDS_SCREEN_TOP, 134, 6, "METRONOME PAUSED", 2, col_gold);
            }
            char vol_badge[16];
            if (g_volume == 0) {
                snprintf(vol_badge, sizeof(vol_badge), "MUTE");
            } else {
                snprintf(vol_badge, sizeof(vol_badge), "V:%d0%%", g_volume);
            }
            rgds_fill_rect(RGDS_SCREEN_TOP, 272, 4, 42, 18, col_bg);
            rgds_draw_rect(RGDS_SCREEN_TOP, 272, 4, 42, 18, 1,
                           (g_volume == 0) ? col_accent : col_border);
            draw_text_at_center(RGDS_SCREEN_TOP, 293, 10, vol_badge, 1,
                                (g_volume == 0) ? col_accent : col_cyan);
        }

        /* 2. BPM Card (Left) & Time Signature Card (Right) */
        rgds_fill_rect(RGDS_SCREEN_TOP, 12, 36, 184, 92, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 12, 36, 184, 92, 2, col_border);

        draw_text_at_center(RGDS_SCREEN_TOP, 104, 43, "TEMPO (UP/DOWN)", 1, col_muted);

        char bpm_str[16];
        snprintf(bpm_str, sizeof(bpm_str), "%d", g_bpm);
        draw_text_at_center(RGDS_SCREEN_TOP, 88, 58, bpm_str, 5, col_white);
        rgds_draw_text(RGDS_SCREEN_TOP, 150, 79, "BPM", 2, col_cyan);

        draw_text_at_center(RGDS_SCREEN_TOP, 104, 108, get_tempo_marking(g_bpm), 2, col_cyan);

        /* Time Signature Card */
        rgds_fill_rect(RGDS_SCREEN_TOP, 204, 36, 104, 92, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 204, 36, 104, 92, 2, col_border);

        draw_text_at_center(RGDS_SCREEN_TOP, 256, 43, "TIME SIG", 1, col_muted);
        draw_text_at_center(RGDS_SCREEN_TOP, 256, 62, k_time_sig_names[g_time_sig_idx], 4, col_gold);
        draw_text_at_center(RGDS_SCREEN_TOP, 256, 108, "< LEFT/RIGHT >", 1, col_muted);

        /* 3. Beat Visualizer Panel */
        rgds_fill_rect(RGDS_SCREEN_TOP, 12, 136, 296, 72, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 12, 136, 296, 72, 2, col_border);

        int margin_x = 22;
        int avail_w  = 296 - 2 * margin_x;
        int gap      = (beats_per_bar <= 4) ? 10 : 6;
        int box_w    = (avail_w - gap * (beats_per_bar - 1)) / beats_per_bar;
        if (box_w > 48) box_w = 48;
        int total_boxes_w = box_w * beats_per_bar + gap * (beats_per_bar - 1);
        int start_x = 12 + (296 - total_boxes_w) / 2;
        int box_y   = 146;
        int box_h   = 42;

        bool beat_flash = g_playing &&
                          ((now_ms - last_beat_trigger_ms) < (uint64_t)(interval_ms * 0.55));

        for (int b = 0; b < beats_per_bar; b++) {
            int bx = start_x + b * (box_w + gap);
            bool is_active = g_playing && (b == current_beat);

            uint32_t fill_col = col_box_idle;
            uint32_t bdr_col  = (b == 0) ? col_accent : col_border;

            if (is_active) {
                fill_col = (b == 0) ? col_accent : col_beat;
                bdr_col  = col_white;
            }

            rgds_fill_rect(RGDS_SCREEN_TOP, bx, box_y, box_w, box_h, fill_col);
            rgds_draw_rect(RGDS_SCREEN_TOP, bx, box_y, box_w, box_h, (b == 0) ? 2 : 1, bdr_col);

            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%d", b + 1);
            uint32_t num_col = is_active ? col_bg : ((b == 0) ? col_accent : col_white);
            draw_text_at_center(RGDS_SCREEN_TOP, bx + box_w / 2, box_y + 10, num_str, 2, num_col);

            if (b == 0) {
                draw_text_at_center(RGDS_SCREEN_TOP, bx + box_w / 2, box_y + 29, "ACC", 1,
                                    is_active ? col_bg : col_accent);
            }
        }

        /* Pendulum / Phase Progress Bar inside Beat Panel */
        int pbar_x = 24, pbar_y = 194, pbar_w = 272, pbar_h = 6;
        rgds_fill_rect(RGDS_SCREEN_TOP, pbar_x, pbar_y, pbar_w, pbar_h, col_bg);
        if (g_playing && interval_ms > 0.0) {
            double elapsed = (double)(now_ms - last_beat_trigger_ms);
            double phase = elapsed / interval_ms;
            if (phase < 0.0) phase = 0.0;
            if (phase > 1.0) phase = 1.0;
            double pos_norm = (current_beat % 2 == 0) ? phase : (1.0 - phase);
            int knob_w = 20;
            int knob_x = pbar_x + (int)(pos_norm * (double)(pbar_w - knob_w));
            uint32_t knob_col = (current_beat == 0 && beat_flash) ? col_accent : col_cyan;
            rgds_fill_rect(RGDS_SCREEN_TOP, knob_x, pbar_y, knob_w, pbar_h, knob_col);
        }

        /* 4. Upper Screen Bottom Bar */
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 214, RGDS_CANVAS_W, 26, col_panel_dark);
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 214, RGDS_CANVAS_W, 1, col_border);
        rgds_draw_text_centered(RGDS_SCREEN_TOP, 223,
                                "A:PLAY/PAUSE  UP/DN:BPM  L/R:SIG  L1/R1:+/-5", 1, col_muted);

        /* ====================================================================
         * LOWER SCREEN: OVERALL STATISTICS (RGDS_SCREEN_BOTTOM, 320x240 -> 1024x768)
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_BOTTOM, col_bg);

        /* 1. Lower Screen Header Bar (y: 0..24) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, RGDS_CANVAS_W, 24, col_panel_dark);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 24, RGDS_CANVAS_W, 2, col_border);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 5, "OVERALL PRACTICE STATS", 2, col_purple);

        /* 2. Top Row Cards: Current Session (Left) & Lifetime Totals (Right) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 32, 146, 76, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 10, 32, 146, 76, 1, col_border);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 83, 38, "CURRENT SESSION", 1, col_cyan);

        char sess_time_str[32];
        format_duration(g_session_ms, sess_time_str, sizeof(sess_time_str));
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 83, 52, sess_time_str, 2, col_white);

        char sess_detail[64];
        snprintf(sess_detail, sizeof(sess_detail), "BEATS: %llu", (unsigned long long)g_session_beats);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 83, 74, sess_detail, 1, col_beat);
        snprintf(sess_detail, sizeof(sess_detail), "BARS : %llu", (unsigned long long)g_session_bars);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 83, 88, sess_detail, 1, col_gold);

        /* Lifetime Totals Card (Right) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 164, 32, 146, 76, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 164, 32, 146, 76, 1, col_border);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 237, 38, "ALL-TIME TOTALS", 1, col_gold);

        char total_time_str[32];
        format_duration(g_total_ms, total_time_str, sizeof(total_time_str));
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 237, 52, total_time_str, 2, col_white);

        char tot_detail[64];
        snprintf(tot_detail, sizeof(tot_detail), "BEATS: %llu", (unsigned long long)g_total_beats);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 237, 74, tot_detail, 1, col_beat);
        snprintf(tot_detail, sizeof(tot_detail), "BARS : %llu  SESS: %llu",
                 (unsigned long long)g_total_bars, (unsigned long long)g_total_sessions);
        draw_text_at_center(RGDS_SCREEN_BOTTOM, 237, 88, tot_detail, 1, col_muted);

        /* 3. Tempo Summary Strip (y: 112..130) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 112, 300, 18, col_panel_dark);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 10, 112, 300, 18, 1, col_border);
        int avg_bpm = (g_total_beats > 0) ? (int)(g_bpm_weighted_sum / g_total_beats) : g_bpm;
        int min_b   = (g_min_bpm_used > 0) ? g_min_bpm_used : g_bpm;
        int max_b   = (g_max_bpm_used > 0) ? g_max_bpm_used : g_bpm;
        char bpm_stat_line[80];
        snprintf(bpm_stat_line, sizeof(bpm_stat_line),
                 "BPM STATS   AVG: %d   MIN: %d   MAX: %d", avg_bpm, min_b, max_b);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 118, bpm_stat_line, 1, col_cyan);

        /* 4. Time Signature Breakdown Panel (y: 134..212) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 10, 134, 300, 78, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 10, 134, 300, 78, 1, col_border);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 139, "TIME SIGNATURE BREAKDOWN", 1, col_muted);

        uint64_t max_sig_ms = 1;
        for (int s = 0; s < NUM_TIME_SIGS; s++) {
            if (g_sig_ms[s] > max_sig_ms) max_sig_ms = g_sig_ms[s];
        }

        for (int s = 0; s < NUM_TIME_SIGS; s++) {
            int row_y = 151 + s * 12;
            bool is_cur_sig = (s == g_time_sig_idx);
            uint32_t lbl_col = is_cur_sig ? col_gold : col_white;

            rgds_draw_text(RGDS_SCREEN_BOTTOM, 18, row_y, k_time_sig_names[s], 1, lbl_col);

            /* Horizontal progress bar (x: 46..182) */
            int bar_x = 46, bar_w = 136, bar_h = 7;
            rgds_fill_rect(RGDS_SCREEN_BOTTOM, bar_x, row_y, bar_w, bar_h, col_bg);
            int fill_w = (int)((g_sig_ms[s] * (uint64_t)bar_w) / max_sig_ms);
            if (g_sig_ms[s] > 0 && fill_w < 3) fill_w = 3;
            if (fill_w > 0) {
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, bar_x, row_y, fill_w, bar_h,
                               is_cur_sig ? col_gold : col_cyan);
            }

            /* Time & Bars for this signature */
            char sig_stat[48];
            uint64_t sec = g_sig_ms[s] / 1000ULL;
            snprintf(sig_stat, sizeof(sig_stat), "%llum%02llus (%lluB)",
                     (unsigned long long)(sec / 60ULL),
                     (unsigned long long)(sec % 60ULL),
                     (unsigned long long)g_sig_bars[s]);
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 190, row_y, sig_stat, 1,
                           is_cur_sig ? col_gold : col_muted);
        }

        /* 5. Lower Screen Bottom Bar (y: 216..240) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 216, RGDS_CANVAS_W, 24, col_panel_dark);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 216, RGDS_CANVAS_W, 1, col_border);
        rgds_draw_text_centered(RGDS_SCREEN_BOTTOM, 224,
                                "B:CLR SESSION   START/SELECT:EXIT", 1, col_muted);

        rgds_present();
        rgds_delay_ms(8);
    }

    save_config();
    audio_shutdown();
    rgds_shutdown();
    return 0;
}
