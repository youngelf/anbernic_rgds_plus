/*
 * pomodoro.c - Dual-Screen Pomodoro Work Timer for Anbernic RG DS Plus & Linux Host
 *
 * Built using the Anbernic RG DS Plus Native C SDK (~/xkdr/rg_ds_plus_sdk):
 *   - Upper Screen (RGDS_SCREEN_TOP, 1024x768):
 *       Active Pomodoro countdown timer, status header ("WORKING: DO NOT DISTURB"),
 *       progress bar, and live work-hours summary (identical layout to the single-screen version).
 *   - Lower Screen (RGDS_SCREEN_BOTTOM, 1024x768):
 *       Previous Sessions & Work History dashboard:
 *         * Summary cards (Total/Completed Sessions, Average & Longest Session, Last Session)
 *         * Scrollable table of previous work & break sessions (#, Date/Time, Mode, Duration, Status)
 *         * Visual session duration bar chart & completion rate
 *
 * Controls (Anbernic RG DS Plus & Linux X11 Desktop Simulator):
 *   A Button ('A' / Space / Left Alt)   : Toggle Start / Stop (Pause) timer
 *   B Button ('B' / Backspace / L-Ctrl) : Stop / Pause timer
 *   X Button ('X' / Left Shift)         : Switch Mode (Work -> Short Break -> Long Break)
 *   Y Button ('Y' / 'R')                : Reset current timer (logs partial work if > 0s)
 *   D-Pad UP / DOWN or R1 / L1          : Adjust current mode duration (+/- 1 min while stopped)
 *   D-Pad LEFT / RIGHT                  : Switch Mode (Work / Short Break / Long Break)
 *   L2 / R2 Triggers ('1'/'2' / PgDn/Up): Scroll Previous Sessions table (or +/-5m when holding START)
 *   START Button (Enter)                : Log & commit current paused work session to history
 *   SELECT / MENU ('M' / Esc / START+SELECT): Save stats & session history and exit
 */

#include "rgds_sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SCREEN_W 320
#define SCREEN_H 240

/* Default Pomodoro durations (in minutes) */
#define DEFAULT_WORK_MINS       25
#define DEFAULT_SHORT_BRK_MINS   5
#define DEFAULT_LONG_BRK_MINS   15

#define MAX_HISTORY_SESSIONS    64
#define VISIBLE_SESSION_ROWS     6

typedef enum {
    MODE_WORK = 0,
    MODE_SHORT_BREAK = 1,
    MODE_LONG_BREAK = 2
} TimerMode;

typedef enum {
    STATUS_READY = 0,
    STATUS_RUNNING = 1,
    STATUS_PAUSED = 2,
    STATUS_DONE = 3
} TimerStatus;

typedef struct {
    uint32_t id;             /* Sequential 1-based session number */
    char     timestamp[48];  /* "YYYY-MM-DD HH:MM" or "SESSION" */
    int      mode;           /* TimerMode (0=Work, 1=Short Break, 2=Long Break) */
    uint32_t duration_sec;   /* Actual seconds spent in this session */
    uint32_t target_sec;     /* Configured target duration in seconds */
    int      completed;      /* 1 = Full timer completed, 0 = Partial logged session */
} SessionRecord;

typedef struct {
    uint64_t total_work_seconds;
    uint64_t today_work_seconds;
    uint32_t total_pomodoros;
    uint32_t today_pomodoros;
    char     last_date[32];
    int      work_mins;
    int      short_break_mins;
    int      long_break_mins;
} WorkStats;

static WorkStats     g_stats;
static SessionRecord g_history[MAX_HISTORY_SESSIONS];
static int           g_history_count = 0;       /* Number of saved sessions (oldest..newest) */
static uint32_t      g_next_session_id = 1;

static char g_save_path[512];
static char g_sessions_path[512];
static char g_log_path[512];

static volatile sig_atomic_t g_sig_quit = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_sig_quit = 1;
}

/* ============================================================================
 * OPTIONAL COMPLETION BEEP AUDIO (SDL2 Audio via dlopen)
 * ============================================================================ */
static volatile int g_beep_samples_remaining = 0;

#ifndef HOST_X11
static uint32_t g_beep_phase = 0;

static void audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    int16_t *samples = (int16_t *)stream;
    int count = len / 2;
    for (int i = 0; i < count; i++) {
        if (g_beep_samples_remaining > 0) {
            int16_t val = 0;
            int rem = g_beep_samples_remaining;
            bool tone_on = (rem > 14000) || (rem < 10000 && rem > 2000);
            if (tone_on) {
                val = ((g_beep_phase / 25) % 2) ? 4500 : -4500;
                g_beep_phase++;
            }
            samples[i] = val;
            g_beep_samples_remaining--;
        } else {
            samples[i] = 0;
        }
    }
}

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} RGDS_AudioSpec;

#define SDL2_INIT_AUDIO  0x00000010u
#define SDL2_AUDIO_S16   0x8010u

static void *g_audio_sdl = NULL;
static void (*fn_SDL_CloseAudio)(void) = NULL;
static bool g_audio_opened = false;

static void init_completion_audio(void) {
    g_audio_sdl = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_audio_sdl) return;

    int (*fn_InitSub)(uint32_t) = dlsym(g_audio_sdl, "SDL_InitSubSystem");
    int (*fn_OpenAudio)(RGDS_AudioSpec *, RGDS_AudioSpec *) = dlsym(g_audio_sdl, "SDL_OpenAudio");
    void (*fn_PauseAudio)(int) = dlsym(g_audio_sdl, "SDL_PauseAudio");
    fn_SDL_CloseAudio = dlsym(g_audio_sdl, "SDL_CloseAudio");

    if (fn_InitSub && fn_OpenAudio && fn_PauseAudio) {
        if (fn_InitSub(SDL2_INIT_AUDIO) == 0) {
            RGDS_AudioSpec desired;
            memset(&desired, 0, sizeof(desired));
            desired.freq = 22050;
            desired.format = SDL2_AUDIO_S16;
            desired.channels = 1;
            desired.samples = 512;
            desired.callback = audio_callback;
            if (fn_OpenAudio(&desired, NULL) == 0) {
                fn_PauseAudio(0);
                g_audio_opened = true;
            }
        }
    }
}

static void shutdown_completion_audio(void) {
    if (g_audio_opened && fn_SDL_CloseAudio) {
        fn_SDL_CloseAudio();
        g_audio_opened = false;
    }
    if (g_audio_sdl) {
        dlclose(g_audio_sdl);
        g_audio_sdl = NULL;
    }
}
#else
static void init_completion_audio(void) {}
static void shutdown_completion_audio(void) {}
#endif

static void trigger_beep(void) {
    g_beep_samples_remaining = 22050; /* ~1 second double-beep at 22050 Hz */
}

/* ============================================================================
 * DATE, TIME & PERSISTENT STORAGE HELPERS
 * ============================================================================ */

static void get_current_date(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t && t->tm_year + 1900 >= 2020) {
        int yr = (t->tm_year + 1900) % 10000;
        int mo = (t->tm_mon + 1) % 100;
        int dy = t->tm_mday % 100;
        snprintf(buf, size, "%04d-%02d-%02d", yr, mo, dy);
    } else {
        snprintf(buf, size, "SESSION");
    }
}

static void get_current_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t && t->tm_year + 1900 >= 2020) {
        int yr = (t->tm_year + 1900) % 10000;
        int mo = (t->tm_mon + 1) % 100;
        int dy = t->tm_mday % 100;
        int hr = t->tm_hour % 24;
        int mn = t->tm_min % 60;
        snprintf(buf, size, "%04d-%02d-%02d %02d:%02d", yr, mo, dy, hr, mn);
    } else {
        snprintf(buf, size, "SESSION #%u", g_next_session_id);
    }
}

static void resolve_save_paths(void) {
    struct stat st;
    const char *rgds_dirs[] = {
        "/mnt/sdcard/Roms/APPS/pomodoro",
        "/mnt/mmc/Roms/APPS/pomodoro",
        "/mnt/sdcard/Ports/Pomodoro",
        "/mnt/mmc/Ports/Pomodoro",
        "/userdata",
        NULL
    };

    for (int i = 0; rgds_dirs[i]; i++) {
        if (stat(rgds_dirs[i], &st) == 0 && S_ISDIR(st.st_mode) && access(rgds_dirs[i], W_OK) == 0) {
            snprintf(g_save_path, sizeof(g_save_path), "%s/.pomodoro_stats.cfg", rgds_dirs[i]);
            snprintf(g_sessions_path, sizeof(g_sessions_path), "%s/.pomodoro_sessions.log", rgds_dirs[i]);
            snprintf(g_log_path, sizeof(g_log_path), "%s/pomodoro_work_log.txt", rgds_dirs[i]);
            return;
        }
    }

    const char *home = getenv("HOME");
    if (home && access(home, W_OK) == 0) {
        snprintf(g_save_path, sizeof(g_save_path), "%s/.pomodoro_stats.cfg", home);
        snprintf(g_sessions_path, sizeof(g_sessions_path), "%s/.pomodoro_sessions.log", home);
        snprintf(g_log_path, sizeof(g_log_path), "%s/pomodoro_work_log.txt", home);
    } else {
        snprintf(g_save_path, sizeof(g_save_path), "./.pomodoro_stats.cfg");
        snprintf(g_sessions_path, sizeof(g_sessions_path), "./.pomodoro_sessions.log");
        snprintf(g_log_path, sizeof(g_log_path), "./pomodoro_work_log.txt");
    }
}

static uint32_t parse_u32_str(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static bool parse_u64_after_prefix(const char *line, const char *prefix, uint64_t *out) {
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0) return false;
    const char *p = line + plen;
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    *out = v;
    return true;
}

static const char *mode_short_name(int m) {
    if (m == MODE_WORK)        return "WORK";
    if (m == MODE_SHORT_BREAK) return "S-BRK";
    return "L-BRK";
}

static void init_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.work_mins = DEFAULT_WORK_MINS;
    g_stats.short_break_mins = DEFAULT_SHORT_BRK_MINS;
    g_stats.long_break_mins = DEFAULT_LONG_BRK_MINS;
    get_current_date(g_stats.last_date, sizeof(g_stats.last_date));
}

static void load_sessions(void) {
    g_history_count = 0;
    g_next_session_id = 1;

    FILE *f = fopen(g_sessions_path, "r");
    if (!f) {
        /* If no session log exists yet but lifetime work stats exist, seed a baseline record */
        if (g_stats.total_work_seconds > 0) {
            SessionRecord rec;
            memset(&rec, 0, sizeof(rec));
            rec.id = g_next_session_id++;
            snprintf(rec.timestamp, sizeof(rec.timestamp), "%s PRIOR", g_stats.last_date);
            rec.mode = MODE_WORK;
            rec.duration_sec = (uint32_t)g_stats.total_work_seconds;
            rec.target_sec = (uint32_t)(g_stats.work_mins * 60);
            rec.completed = (g_stats.total_pomodoros > 0) ? 1 : 0;
            g_history[g_history_count++] = rec;
        }
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Format: id|timestamp|mode|duration_sec|target_sec|completed */
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1++ = '\0';
        char *p2 = strchr(p1, '|');
        if (!p2) continue;
        *p2++ = '\0';
        char *p3 = strchr(p2, '|');
        if (!p3) continue;
        *p3++ = '\0';
        char *p4 = strchr(p3, '|');
        if (!p4) continue;
        *p4++ = '\0';
        char *p5 = strchr(p4, '|');
        if (!p5) continue;
        *p5++ = '\0';

        SessionRecord rec;
        memset(&rec, 0, sizeof(rec));
        rec.id = parse_u32_str(line);
        snprintf(rec.timestamp, sizeof(rec.timestamp), "%s", p1);
        rec.mode = (int)parse_u32_str(p2);
        rec.duration_sec = parse_u32_str(p3);
        rec.target_sec = parse_u32_str(p4);
        rec.completed = (int)parse_u32_str(p5);

        if (rec.id >= g_next_session_id) {
            g_next_session_id = rec.id + 1;
        }

        if (g_history_count < MAX_HISTORY_SESSIONS) {
            g_history[g_history_count++] = rec;
        } else {
            memmove(&g_history[0], &g_history[1], sizeof(SessionRecord) * (MAX_HISTORY_SESSIONS - 1));
            g_history[MAX_HISTORY_SESSIONS - 1] = rec;
        }
    }
    fclose(f);
}

static void save_sessions(void) {
    FILE *f = fopen(g_sessions_path, "w");
    if (!f) return;
    for (int i = 0; i < g_history_count; i++) {
        fprintf(f, "%u|%s|%d|%u|%u|%d\n",
                g_history[i].id,
                g_history[i].timestamp,
                g_history[i].mode,
                g_history[i].duration_sec,
                g_history[i].target_sec,
                g_history[i].completed);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void save_stats(void) {
    FILE *f = fopen(g_save_path, "w");
    if (f) {
        fprintf(f, "total_work_seconds=%llu\n", (unsigned long long)g_stats.total_work_seconds);
        fprintf(f, "today_work_seconds=%llu\n", (unsigned long long)g_stats.today_work_seconds);
        fprintf(f, "total_pomodoros=%u\n", g_stats.total_pomodoros);
        fprintf(f, "today_pomodoros=%u\n", g_stats.today_pomodoros);
        fprintf(f, "last_date=%s\n", g_stats.last_date);
        fprintf(f, "work_mins=%d\n", g_stats.work_mins);
        fprintf(f, "short_break_mins=%d\n", g_stats.short_break_mins);
        fprintf(f, "long_break_mins=%d\n", g_stats.long_break_mins);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }

    FILE *lf = fopen(g_log_path, "w");
    if (lf) {
        double total_hrs = (double)g_stats.total_work_seconds / 3600.0;
        double today_hrs = (double)g_stats.today_work_seconds / 3600.0;
        uint64_t th = g_stats.total_work_seconds / 3600;
        uint64_t tm = (g_stats.total_work_seconds % 3600) / 60;
        uint64_t ts = g_stats.total_work_seconds % 60;

        fprintf(lf, "=== Pomodoro Work Log (%s) ===\n", g_stats.last_date);
        fprintf(lf, "Total Work Time : %.2f hours (%lluh %02llum %02llus)\n",
                total_hrs, (unsigned long long)th, (unsigned long long)tm, (unsigned long long)ts);
        fprintf(lf, "Today Work Time : %.2f hours\n", today_hrs);
        fprintf(lf, "Total Pomodoros : %u\n", g_stats.total_pomodoros);
        fprintf(lf, "Today Pomodoros : %u\n", g_stats.today_pomodoros);
        fprintf(lf, "\n--- Previous Sessions (%d saved) ---\n", g_history_count);
        for (int i = g_history_count - 1; i >= 0; i--) {
            uint32_t dm = g_history[i].duration_sec / 60;
            uint32_t ds = g_history[i].duration_sec % 60;
            fprintf(lf, "#%-3u | %-16s | %-5s | %02um %02us | %s\n",
                    g_history[i].id,
                    g_history[i].timestamp,
                    mode_short_name(g_history[i].mode),
                    dm, ds,
                    g_history[i].completed ? "COMPLETED" : "PARTIAL");
        }
        fflush(lf);
        fsync(fileno(lf));
        fclose(lf);
    }
}

static void record_session(int mode, uint32_t duration_sec, uint32_t target_sec, int completed) {
    if (duration_sec == 0) return;

    SessionRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = g_next_session_id++;
    get_current_timestamp(rec.timestamp, sizeof(rec.timestamp));
    rec.mode = mode;
    rec.duration_sec = duration_sec;
    rec.target_sec = (target_sec > 0) ? target_sec : duration_sec;
    rec.completed = completed;

    if (g_history_count < MAX_HISTORY_SESSIONS) {
        g_history[g_history_count++] = rec;
    } else {
        memmove(&g_history[0], &g_history[1], sizeof(SessionRecord) * (MAX_HISTORY_SESSIONS - 1));
        g_history[MAX_HISTORY_SESSIONS - 1] = rec;
    }

    save_sessions();
    save_stats();
}

static void load_stats(void) {
    init_stats();
    FILE *f = fopen(g_save_path, "r");
    if (f) {
        char line[256];
        char loaded_date[32] = {0};
        while (fgets(line, sizeof(line), f)) {
            uint64_t val = 0;
            if (parse_u64_after_prefix(line, "total_work_seconds=", &val)) {
                g_stats.total_work_seconds = val;
            } else if (parse_u64_after_prefix(line, "today_work_seconds=", &val)) {
                g_stats.today_work_seconds = val;
            } else if (parse_u64_after_prefix(line, "total_pomodoros=", &val)) {
                g_stats.total_pomodoros = (uint32_t)val;
            } else if (parse_u64_after_prefix(line, "today_pomodoros=", &val)) {
                g_stats.today_pomodoros = (uint32_t)val;
            } else if (strncmp(line, "last_date=", 10) == 0) {
                const char *p = line + 10;
                size_t i = 0;
                while (p[i] && p[i] != '\n' && p[i] != '\r' && p[i] != ' ' && i < sizeof(loaded_date) - 1) {
                    loaded_date[i] = p[i];
                    i++;
                }
                loaded_date[i] = '\0';
            } else if (parse_u64_after_prefix(line, "work_mins=", &val) && val >= 1 && val <= 180) {
                g_stats.work_mins = (int)val;
            } else if (parse_u64_after_prefix(line, "short_break_mins=", &val) && val >= 1 && val <= 60) {
                g_stats.short_break_mins = (int)val;
            } else if (parse_u64_after_prefix(line, "long_break_mins=", &val) && val >= 1 && val <= 90) {
                g_stats.long_break_mins = (int)val;
            }
        }
        fclose(f);

        char cur_date[32];
        get_current_date(cur_date, sizeof(cur_date));
        if (strcmp(cur_date, "SESSION") != 0 && loaded_date[0] != '\0' && strcmp(cur_date, loaded_date) != 0) {
            g_stats.today_work_seconds = 0;
            g_stats.today_pomodoros = 0;
        }
        snprintf(g_stats.last_date, sizeof(g_stats.last_date), "%s", cur_date);
    }

    load_sessions();
}

static int get_mode_duration(TimerMode mode) {
    switch (mode) {
        case MODE_WORK:        return g_stats.work_mins * 60;
        case MODE_SHORT_BREAK: return g_stats.short_break_mins * 60;
        case MODE_LONG_BREAK:  return g_stats.long_break_mins * 60;
        default:               return DEFAULT_WORK_MINS * 60;
    }
}

/* Draw text centered around arbitrary horizontal coordinate `cx` on `screen` */
static void draw_text_centered_at(rgds_screen_t screen, int cx, int y, const char *str, int scale, uint32_t color) {
    int w = rgds_text_width(str, scale);
    rgds_draw_text(screen, cx - w / 2, y, str, scale, color);
}

/* Format duration concisely as "XXm YYs" or "Xh YYm" */
static void format_duration_short(uint32_t sec, char *buf, size_t size) {
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    if (h > 0) {
        snprintf(buf, size, "%uh %02um", h, m);
    } else {
        snprintf(buf, size, "%02um %02us", m, s);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    resolve_save_paths();
    load_stats();

    if (rgds_init("Pomodoro Timer") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }
    init_completion_audio();

    /* Palette matching the original Pomodoro Timer UI */
    uint32_t col_bg      = RGDS_RGB(18, 22, 31);
    uint32_t col_header  = RGDS_RGB(28, 35, 49);
    uint32_t col_panel   = RGDS_RGB(25, 31, 44);
    uint32_t col_row_alt = RGDS_RGB(31, 39, 55);
    uint32_t col_border  = RGDS_RGB(55, 68, 92);
    uint32_t col_white   = RGDS_RGB(240, 244, 248);
    uint32_t col_muted   = RGDS_RGB(145, 160, 180);
    uint32_t col_work    = RGDS_RGB(235, 87, 87);   /* Pomodoro Red */
    uint32_t col_running = RGDS_RGB(46, 204, 113);  /* Emerald Green */
    uint32_t col_paused  = RGDS_RGB(242, 153, 74);  /* Amber Orange */
    uint32_t col_break   = RGDS_RGB(52, 152, 219);  /* Calm Blue */
    uint32_t col_gold    = RGDS_RGB(241, 196, 15);  /* Gold */
    uint32_t col_cyan    = RGDS_RGB(70, 210, 255);  /* Cyan Accent */
    uint32_t col_bar_bg  = RGDS_RGB(12, 15, 22);

    TimerMode mode = MODE_WORK;
    TimerStatus status = STATUS_READY;
    int total_phase_sec = get_mode_duration(mode);
    int remaining_sec = total_phase_sec;
    uint64_t current_session_work_sec = 0;
    uint32_t active_phase_elapsed_sec = 0; /* Unlogged seconds in the current phase */

    int history_scroll = 0; /* 0 = newest session at top of table */

    uint64_t last_tick_ms = rgds_ticks_ms();
    uint64_t accum_ms = 0;
    int unsaved_work_sec = 0;

    bool running = true;
    rgds_input_state_t input;

    while (running && !g_sig_quit && rgds_poll_input(&input)) {
        uint32_t p = input.pressed;

        /* SELECT button also saves and exits (matching V90 and RG35XXSP behavior) */
        if (p & RGDS_BTN_SELECT) {
            running = false;
            break;
        }

        /* A Button: Toggle Start / Stop (Pause) */
        if (p & RGDS_BTN_A) {
            if (status == STATUS_RUNNING) {
                status = STATUS_PAUSED;
                save_stats();
                unsaved_work_sec = 0;
            } else {
                if (status == STATUS_DONE) {
                    if (mode == MODE_WORK) {
                        mode = (g_stats.today_pomodoros % 4 == 0) ? MODE_LONG_BREAK : MODE_SHORT_BREAK;
                    } else {
                        mode = MODE_WORK;
                    }
                    total_phase_sec = get_mode_duration(mode);
                    remaining_sec = total_phase_sec;
                    active_phase_elapsed_sec = 0;
                }
                status = STATUS_RUNNING;
                last_tick_ms = rgds_ticks_ms();
            }
        }
        /* B Button: Stop / Pause timer */
        else if (p & RGDS_BTN_B) {
            if (status == STATUS_RUNNING) {
                status = STATUS_PAUSED;
                save_stats();
                unsaved_work_sec = 0;
            } else if (status == STATUS_DONE) {
                status = STATUS_READY;
                total_phase_sec = get_mode_duration(mode);
                remaining_sec = total_phase_sec;
                active_phase_elapsed_sec = 0;
            }
        }
        /* X Button or D-Pad LEFT / RIGHT: Switch Mode */
        else if (p & (RGDS_BTN_X | RGDS_BTN_LEFT | RGDS_BTN_RIGHT)) {
            if (active_phase_elapsed_sec > 0) {
                record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 0);
                active_phase_elapsed_sec = 0;
                history_scroll = 0;
            } else {
                save_stats();
            }
            unsaved_work_sec = 0;
            if (p & RGDS_BTN_LEFT) {
                mode = (TimerMode)((mode + 2) % 3);
            } else {
                mode = (TimerMode)((mode + 1) % 3);
            }
            status = STATUS_READY;
            total_phase_sec = get_mode_duration(mode);
            remaining_sec = total_phase_sec;
            accum_ms = 0;
        }
        /* Y Button: Reset current timer (logs partial session if work was done) */
        else if (p & RGDS_BTN_Y) {
            if (active_phase_elapsed_sec > 0) {
                record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 0);
                active_phase_elapsed_sec = 0;
                history_scroll = 0;
            }
            status = STATUS_READY;
            total_phase_sec = get_mode_duration(mode);
            remaining_sec = total_phase_sec;
            accum_ms = 0;
            save_stats();
            unsaved_work_sec = 0;
        }
        /* START Button: Explicitly log/commit current partial session into Previous Sessions history (or start if ready) */
        else if (p & RGDS_BTN_START) {
            if (active_phase_elapsed_sec > 0) {
                record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 0);
                active_phase_elapsed_sec = 0;
                history_scroll = 0;
                status = STATUS_READY;
                total_phase_sec = get_mode_duration(mode);
                remaining_sec = total_phase_sec;
                accum_ms = 0;
            } else if (status != STATUS_RUNNING) {
                status = STATUS_RUNNING;
                last_tick_ms = rgds_ticks_ms();
                history_scroll = 0;
            } else {
                history_scroll = 0;
            }
        }
        /* D-Pad UP / DOWN or L1 / R1: Adjust duration (+/- 1 min) when timer is not running */
        else if ((p & (RGDS_BTN_UP | RGDS_BTN_DOWN | RGDS_BTN_L1 | RGDS_BTN_R1)) && status != STATUS_RUNNING) {
            if (active_phase_elapsed_sec > 0) {
                record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 0);
                active_phase_elapsed_sec = 0;
            }
            int delta = ((p & RGDS_BTN_UP) || (p & RGDS_BTN_R1)) ? 1 : -1;
            if (mode == MODE_WORK) {
                g_stats.work_mins += delta;
                if (g_stats.work_mins < 1) g_stats.work_mins = 1;
                if (g_stats.work_mins > 120) g_stats.work_mins = 120;
            } else if (mode == MODE_SHORT_BREAK) {
                g_stats.short_break_mins += delta;
                if (g_stats.short_break_mins < 1) g_stats.short_break_mins = 1;
                if (g_stats.short_break_mins > 60) g_stats.short_break_mins = 60;
            } else {
                g_stats.long_break_mins += delta;
                if (g_stats.long_break_mins < 1) g_stats.long_break_mins = 1;
                if (g_stats.long_break_mins > 90) g_stats.long_break_mins = 90;
            }
            status = STATUS_READY;
            total_phase_sec = get_mode_duration(mode);
            remaining_sec = total_phase_sec;
            accum_ms = 0;
            save_stats();
        }
        /* L2 / R2 Triggers: Scroll Previous Sessions history table on the Bottom Screen */
        else if (p & (RGDS_BTN_L2 | RGDS_BTN_R2)) {
            int max_scroll = g_history_count - VISIBLE_SESSION_ROWS;
            if (max_scroll < 0) max_scroll = 0;
            if (p & RGDS_BTN_R2) {
                history_scroll++;
                if (history_scroll > max_scroll) history_scroll = max_scroll;
            } else {
                history_scroll--;
                if (history_scroll < 0) history_scroll = 0;
            }
        }

        /* ================= UPDATE TIMER ================= */
        uint64_t now_ms = rgds_ticks_ms();
        if (status == STATUS_RUNNING) {
            uint64_t dt = (now_ms >= last_tick_ms) ? (now_ms - last_tick_ms) : 0;
            if (dt > 5000) dt = 1000; /* Guard against system suspend/clock jumps */
            last_tick_ms = now_ms;
            accum_ms += dt;

            while (accum_ms >= 1000) {
                accum_ms -= 1000;
                if (remaining_sec > 0) {
                    remaining_sec--;
                }
                active_phase_elapsed_sec++;

                if (mode == MODE_WORK) {
                    g_stats.total_work_seconds++;
                    g_stats.today_work_seconds++;
                    current_session_work_sec++;
                    unsaved_work_sec++;
                    if (unsaved_work_sec >= 30) {
                        save_stats();
                        unsaved_work_sec = 0;
                    }
                }

                if (remaining_sec <= 0) {
                    remaining_sec = 0;
                    status = STATUS_DONE;
                    if (mode == MODE_WORK) {
                        g_stats.total_pomodoros++;
                        g_stats.today_pomodoros++;
                    }
                    record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 1);
                    active_phase_elapsed_sec = 0;
                    history_scroll = 0;
                    unsaved_work_sec = 0;
                    trigger_beep();
                    break;
                }
            }
        } else {
            last_tick_ms = now_ms;
        }

        /* ====================================================================
         * 1. RENDER TOP SCREEN (RGDS_SCREEN_TOP - Existing Pomodoro Screen)
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_TOP, col_bg);

        /* 1a. Top Header Bar (y: 0..26) */
        const char *top_str = "READY TO WORK";
        uint32_t top_col = col_work;

        if (status == STATUS_RUNNING) {
            if (mode == MODE_WORK) {
                top_str = "WORKING: DO NOT DISTURB";
                top_col = col_work; /* Red */
            } else if (mode == MODE_SHORT_BREAK) {
                top_str = "SHORT BREAK: RELAX";
                top_col = col_break;
            } else {
                top_str = "LONG BREAK: RELAX";
                top_col = col_break;
            }
        } else if (status == STATUS_PAUSED) {
            top_str = "PAUSED: TIMER STOPPED";
            top_col = col_paused;
        } else if (status == STATUS_DONE) {
            top_str = (mode == MODE_WORK) ? "WORK DONE! TAKE A BREAK" : "BREAK OVER! READY TO WORK";
            top_col = col_gold;
        } else {
            if (mode == MODE_WORK) {
                top_str = "WORKING: DO NOT DISTURB";
                top_col = col_work; /* Red */
            } else if (mode == MODE_SHORT_BREAK) {
                top_str = "SHORT BREAK: READY";
                top_col = col_break;
            } else {
                top_str = "LONG BREAK: READY";
                top_col = col_break;
            }
        }

        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 0, SCREEN_W, 26, col_header);
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 24, SCREEN_W, 2, top_col);
        draw_text_centered_at(RGDS_SCREEN_TOP, SCREEN_W / 2, 6, top_str, 2, top_col);

        /* 1b. Status Banner (y: 32..54) */
        uint32_t status_col = col_muted;
        const char *status_str = "READY - PRESS A TO START";
        if (status == STATUS_RUNNING) {
            if (mode == MODE_WORK) {
                status_col = col_running;
                status_str = "STATUS: WORKING (PRESS A TO PAUSE)";
            } else {
                status_col = col_break;
                status_str = "STATUS: ON BREAK (PRESS A TO PAUSE)";
            }
        } else if (status == STATUS_PAUSED) {
            status_col = col_paused;
            status_str = "STATUS: PAUSED (PRESS A TO RESUME)";
        } else if (status == STATUS_DONE) {
            status_col = col_gold;
            status_str = (mode == MODE_WORK) ? "WORK COMPLETE! PRESS A FOR BREAK" :
                                               "BREAK OVER! PRESS A TO WORK";
        } else {
            status_col = (mode == MODE_WORK) ? col_work : col_break;
            status_str = (mode == MODE_WORK) ? "STATUS: READY TO WORK (PRESS A)" :
                                               "STATUS: READY FOR BREAK (PRESS A)";
        }

        rgds_fill_rect(RGDS_SCREEN_TOP, 10, 32, 300, 22, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 10, 32, 300, 22, 1, status_col);
        if (status != STATUS_RUNNING || ((now_ms / 500) % 2 == 0)) {
            rgds_fill_rect(RGDS_SCREEN_TOP, 18, 39, 8, 8, status_col);
        }
        draw_text_centered_at(RGDS_SCREEN_TOP, 165, 40, status_str, 1, col_white);

        /* 1c. Main Countdown Timer Box (y: 60..136) */
        rgds_fill_rect(RGDS_SCREEN_TOP, 10, 60, 300, 76, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 10, 60, 300, 76, 1, col_border);

        int mins = remaining_sec / 60;
        int secs = remaining_sec % 60;
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", mins, secs);

        uint32_t timer_col = (status == STATUS_PAUSED) ? col_paused :
                             (status == STATUS_DONE)   ? col_gold   : col_white;
        draw_text_centered_at(RGDS_SCREEN_TOP, 160, 68, time_buf, 6, timer_col);

        /* Progress Bar inside Timer Box (y: 120..128) */
        int bar_x = 24, bar_y = 120, bar_w = 272, bar_h = 8;
        rgds_fill_rect(RGDS_SCREEN_TOP, bar_x, bar_y, bar_w, bar_h, col_bar_bg);
        if (total_phase_sec > 0) {
            int elapsed = total_phase_sec - remaining_sec;
            if (elapsed < 0) elapsed = 0;
            if (elapsed > total_phase_sec) elapsed = total_phase_sec;
            int fill_w = (elapsed * bar_w) / total_phase_sec;
            if (fill_w > 0) {
                rgds_fill_rect(RGDS_SCREEN_TOP, bar_x, bar_y, fill_w, bar_h, status_col);
            }
        }
        rgds_draw_rect(RGDS_SCREEN_TOP, bar_x, bar_y, bar_w, bar_h, 1, col_border);

        /* 1d. Work Hours & Stats Panel (y: 142..208) */
        rgds_fill_rect(RGDS_SCREEN_TOP, 10, 142, 300, 66, col_panel);
        rgds_draw_rect(RGDS_SCREEN_TOP, 10, 142, 300, 66, 1, col_border);

        double total_hours = (double)g_stats.total_work_seconds / 3600.0;
        uint64_t tot_h = g_stats.total_work_seconds / 3600;
        uint64_t tot_m = (g_stats.total_work_seconds % 3600) / 60;
        uint64_t tot_s = g_stats.total_work_seconds % 60;

        char line1[64];
        snprintf(line1, sizeof(line1), "WORKED: %.2f HRS", total_hours);
        rgds_draw_text(RGDS_SCREEN_TOP, 18, 149, line1, 2, col_gold);

        char line2[64];
        snprintf(line2, sizeof(line2), "TOTAL : %lluh %02llum %02llus  (%u POMS)",
                 (unsigned long long)tot_h, (unsigned long long)tot_m,
                 (unsigned long long)tot_s, g_stats.total_pomodoros);
        rgds_draw_text(RGDS_SCREEN_TOP, 18, 171, line2, 1, col_white);

        double today_hours = (double)g_stats.today_work_seconds / 3600.0;
        uint64_t sess_m = current_session_work_sec / 60;
        uint64_t sess_s = current_session_work_sec % 60;
        char line3[64];
        snprintf(line3, sizeof(line3), "TODAY : %.2f hrs (%u poms) | SESSION: %llum %02llus",
                 today_hours, g_stats.today_pomodoros,
                 (unsigned long long)sess_m, (unsigned long long)sess_s);
        rgds_draw_text(RGDS_SCREEN_TOP, 18, 184, line3, 1, col_muted);

        char line4[64];
        const char *mode_label = (mode == MODE_WORK) ? "Work" :
                                 (mode == MODE_SHORT_BREAK) ? "Short Break" : "Long Break";
        snprintf(line4, sizeof(line4), "MODE  : %s (%d min)  [UP/DN/L/R: +/-m]",
                 mode_label, total_phase_sec / 60);
        rgds_draw_text(RGDS_SCREEN_TOP, 18, 196, line4, 1, col_muted);

        /* 1e. Bottom Controls Footer (y: 214..240) */
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 214, SCREEN_W, 26, col_header);
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 214, SCREEN_W, 1, col_border);

        uint32_t a_btn_col = (status == STATUS_RUNNING) ? col_paused : col_running;
        rgds_fill_rect(RGDS_SCREEN_TOP, 8, 220, 96, 14, a_btn_col);
        draw_text_centered_at(RGDS_SCREEN_TOP, 56, 224, "[A] START/STOP", 1, col_bg);

        rgds_fill_rect(RGDS_SCREEN_TOP, 110, 220, 62, 14, col_break);
        draw_text_centered_at(RGDS_SCREEN_TOP, 141, 224, "[X] MODE", 1, col_white);

        rgds_draw_text(RGDS_SCREEN_TOP, 180, 224, "[Y]RST  [SEL]EXIT", 1, col_white);

        /* ====================================================================
         * 2. RENDER BOTTOM SCREEN (RGDS_SCREEN_BOTTOM - Previous Sessions Info)
         * ==================================================================== */
        rgds_clear(RGDS_SCREEN_BOTTOM, col_bg);

        /* 2a. Bottom Screen Header Bar (y: 0..24) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, SCREEN_W, 24, col_header);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 22, SCREEN_W, 2, col_cyan);
        draw_text_centered_at(RGDS_SCREEN_BOTTOM, SCREEN_W / 2, 5, "PREVIOUS SESSIONS", 2, col_cyan);

        /* Compute aggregate statistics across saved sessions */
        uint32_t work_sess_count = 0;
        uint32_t completed_count = 0;
        uint64_t sum_work_sec = 0;
        uint32_t max_work_sec = 0;

        for (int i = 0; i < g_history_count; i++) {
            if (g_history[i].completed) completed_count++;
            if (g_history[i].mode == MODE_WORK) {
                work_sess_count++;
                sum_work_sec += g_history[i].duration_sec;
                if (g_history[i].duration_sec > max_work_sec) {
                    max_work_sec = g_history[i].duration_sec;
                }
            }
        }
        uint32_t avg_work_sec = (work_sess_count > 0) ? (uint32_t)(sum_work_sec / work_sess_count) : 0;

        /* 2b. Summary Cards Row (y: 28..64) */
        /* Card 1: Total & Completed Sessions (x: 8..106) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 8, 28, 98, 36, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 8, 28, 98, 36, 1, col_border);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14, 32, "HISTORY COUNT", 1, col_muted);
        char c1_line1[32], c1_line2[32];
        snprintf(c1_line1, sizeof(c1_line1), "%d SESSIONS", g_history_count);
        snprintf(c1_line2, sizeof(c1_line2), "%u COMPLETED", completed_count);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14, 43, c1_line1, 1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14, 53, c1_line2, 1, col_running);

        /* Card 2: Average & Longest Work Session (x: 111..209) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 111, 28, 98, 36, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 111, 28, 98, 36, 1, col_border);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 117, 32, "WORK SESSIONS", 1, col_muted);
        char avg_buf[16], max_buf[16], c2_line1[32], c2_line2[32];
        format_duration_short(avg_work_sec, avg_buf, sizeof(avg_buf));
        format_duration_short(max_work_sec, max_buf, sizeof(max_buf));
        snprintf(c2_line1, sizeof(c2_line1), "AVG: %s", avg_buf);
        snprintf(c2_line2, sizeof(c2_line2), "MAX: %s", max_buf);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 117, 43, c2_line1, 1, col_white);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 117, 53, c2_line2, 1, col_gold);

        /* Card 3: Last Session Summary (x: 214..312) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 214, 28, 98, 36, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 214, 28, 98, 36, 1, col_border);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 220, 32, "LAST SESSION", 1, col_muted);
        if (g_history_count > 0) {
            const SessionRecord *last = &g_history[g_history_count - 1];
            char last_dur[16], c3_line1[32], c3_line2[32];
            format_duration_short(last->duration_sec, last_dur, sizeof(last_dur));
            snprintf(c3_line1, sizeof(c3_line1), "%s %s", mode_short_name(last->mode), last_dur);
            const char *short_ts = (strlen(last->timestamp) > 5) ? (last->timestamp + 5) : last->timestamp;
            snprintf(c3_line2, sizeof(c3_line2), "%.14s", short_ts);
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 220, 43, c3_line1, 1,
                           last->completed ? col_running : col_paused);
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 220, 53, c3_line2, 1, col_white);
        } else {
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 220, 43, "NO SESSIONS", 1, col_muted);
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 220, 53, "LOGGED YET", 1, col_muted);
        }

        /* 2c. Previous Sessions Table (y: 68..176, height 108) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 8, 68, 304, 108, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 8, 68, 304, 108, 1, col_border);

        /* Table Column Headers */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 9, 69, 302, 13, col_header);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14,  72, "#",          1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 40,  72, "DATE/TIME",  1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 118, 72, "MODE",       1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 156, 72, "DURATION",   1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 214, 72, "PCT",        1, col_cyan);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 266, 72, "STATUS",     1, col_cyan);

        if (g_history_count == 0 && active_phase_elapsed_sec == 0) {
            draw_text_centered_at(RGDS_SCREEN_BOTTOM, 160, 102, "NO PREVIOUS SESSIONS RECORDED YET", 1, col_white);
            draw_text_centered_at(RGDS_SCREEN_BOTTOM, 160, 118, "PRESS [A] TO START WORKING ON TOP SCREEN", 1, col_muted);
            draw_text_centered_at(RGDS_SCREEN_BOTTOM, 160, 134, "SESSIONS LOG ON FINISH, RESET, OR [START]", 1, col_muted);
        } else {
            int row_idx = 0;
            /* If a session is currently active, show it as the top LIVE row when scroll == 0 */
            if (active_phase_elapsed_sec > 0 && history_scroll == 0) {
                int ry = 84 + row_idx * 15;
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, 9, ry, 302, 14, RGDS_RGB(24, 44, 58));

                char id_str[12], dur_str[16];
                snprintf(id_str, sizeof(id_str), "#%u", g_next_session_id);
                format_duration_short(active_phase_elapsed_sec, dur_str, sizeof(dur_str));

                rgds_draw_text(RGDS_SCREEN_BOTTOM, 14,  ry + 3, id_str, 1, col_cyan);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 40,  ry + 3, "IN PROGRESS", 1, col_white);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 118, ry + 3, mode_short_name(mode), 1,
                               (mode == MODE_WORK) ? col_work : col_break);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 156, ry + 3, dur_str, 1, col_white);

                /* Mini progress bar */
                int pb_w = 42;
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, pb_w, 6, col_bar_bg);
                int fill = (total_phase_sec > 0) ? (int)((active_phase_elapsed_sec * (uint32_t)pb_w) / (uint32_t)total_phase_sec) : 0;
                if (fill > pb_w) fill = pb_w;
                if (fill > 0) rgds_fill_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, fill, 6, col_cyan);
                rgds_draw_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, pb_w, 6, 1, col_border);

                rgds_draw_text(RGDS_SCREEN_BOTTOM, 266, ry + 3,
                               (status == STATUS_RUNNING) ? "LIVE" : "PAUS", 1,
                               (status == STATUS_RUNNING) ? col_cyan : col_paused);
                row_idx++;
            }

            /* Render saved previous sessions from newest to oldest */
            for (int i = g_history_count - 1 - history_scroll;
                 i >= 0 && row_idx < VISIBLE_SESSION_ROWS;
                 i--, row_idx++) {
                const SessionRecord *rec = &g_history[i];
                int ry = 84 + row_idx * 15;

                if (row_idx % 2 == 1) {
                    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 9, ry, 302, 14, col_row_alt);
                }

                char id_str[12], ts_short[16], dur_str[16];
                snprintf(id_str, sizeof(id_str), "#%u", rec->id);
                const char *ts_ptr = (strlen(rec->timestamp) > 5) ? (rec->timestamp + 5) : rec->timestamp;
                snprintf(ts_short, sizeof(ts_short), "%.11s", ts_ptr);
                format_duration_short(rec->duration_sec, dur_str, sizeof(dur_str));

                uint32_t mode_c = (rec->mode == MODE_WORK) ? col_work : col_break;
                uint32_t stat_c = rec->completed ? col_running : col_paused;

                rgds_draw_text(RGDS_SCREEN_BOTTOM, 14,  ry + 3, id_str, 1, col_gold);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 40,  ry + 3, ts_short, 1, col_white);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 118, ry + 3, mode_short_name(rec->mode), 1, mode_c);
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 156, ry + 3, dur_str, 1, col_white);

                /* Mini completion bar (x: 214..256) */
                int pb_w = 42;
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, pb_w, 6, col_bar_bg);
                uint32_t tgt = (rec->target_sec > 0) ? rec->target_sec : 1;
                int fill = (int)((rec->duration_sec * (uint32_t)pb_w) / tgt);
                if (fill > pb_w) fill = pb_w;
                if (fill > 0) {
                    rgds_fill_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, fill, 6, stat_c);
                }
                rgds_draw_rect(RGDS_SCREEN_BOTTOM, 214, ry + 4, pb_w, 6, 1, col_border);

                rgds_draw_text(RGDS_SCREEN_BOTTOM, 266, ry + 3,
                               rec->completed ? "DONE" : "PART", 1, stat_c);
            }
        }

        /* 2d. Recent Sessions Visual Chart & Completion Rate Strip (y: 180..214) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 8, 180, 304, 34, col_panel);
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 8, 180, 304, 34, 1, col_border);

        int comp_pct = (g_history_count > 0) ? (int)((completed_count * 100u) / (uint32_t)g_history_count) : 0;
        char chart_line1[40], chart_line2[40];
        snprintf(chart_line1, sizeof(chart_line1), "COMPLETION: %d%%", comp_pct);
        snprintf(chart_line2, sizeof(chart_line2), "TODAY: %u POMS (%.1fH)",
                 g_stats.today_pomodoros, (double)g_stats.today_work_seconds / 3600.0);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14, 186, chart_line1, 1, col_gold);
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 14, 199, chart_line2, 1, col_muted);

        /* Mini bar chart of last 10 sessions on the right side (x: 152..304) */
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 152, 184, "LAST 10 SESSIONS:", 1, col_muted);
        int max_bars = 10;
        int start_idx = (g_history_count > max_bars) ? (g_history_count - max_bars) : 0;
        int num_bars = g_history_count - start_idx;
        uint32_t chart_max_sec = 1500; /* 25m baseline */
        for (int i = start_idx; i < g_history_count; i++) {
            if (g_history[i].duration_sec > chart_max_sec) {
                chart_max_sec = g_history[i].duration_sec;
            }
        }
        for (int b = 0; b < max_bars; b++) {
            int bx = 152 + b * 15;
            int by_bottom = 210;
            int max_bh = 16;
            rgds_fill_rect(RGDS_SCREEN_BOTTOM, bx, by_bottom - max_bh, 11, max_bh, col_bar_bg);
            if (b < num_bars) {
                const SessionRecord *sr = &g_history[start_idx + b];
                int bh = (int)((sr->duration_sec * (uint32_t)max_bh) / chart_max_sec);
                if (bh < 2 && sr->duration_sec > 0) bh = 2;
                if (bh > max_bh) bh = max_bh;
                uint32_t bc = sr->completed ? col_running : col_paused;
                rgds_fill_rect(RGDS_SCREEN_BOTTOM, bx, by_bottom - bh, 11, bh, bc);
            }
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, bx, by_bottom - max_bh, 11, max_bh, 1, col_border);
        }

        /* 2e. Bottom Screen Footer Bar (y: 218..240) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 218, SCREEN_W, 22, col_header);
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 218, SCREEN_W, 1, col_border);
        draw_text_centered_at(RGDS_SCREEN_BOTTOM, SCREEN_W / 2, 226,
                              "[L2/R2] SCROLL   [START] LOG NOW   [M/SEL] EXIT", 1, col_white);

        /* Present both 1024x768 screens */
        rgds_present();
        rgds_delay_ms(16); /* ~60 FPS for responsive input */
    }

    /* Record any unlogged active work session before exiting */
    if (active_phase_elapsed_sec > 0) {
        record_session(mode, active_phase_elapsed_sec, (uint32_t)total_phase_sec, 0);
    } else {
        save_stats();
        save_sessions();
    }

    shutdown_completion_audio();
    rgds_shutdown();
    return 0;
}
