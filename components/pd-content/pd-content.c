#include "pd-content.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lodepng.h"
#include "pd-display.h"
#include "pd-transition.h"
#include "pd-network.h"
#include "pd-discovery.h"
#include "pd-config.h"
#include "pd-sprite-scene.h"

#define PD_PALETTE_MAX 64
#define PD_QUANT_SAMPLES_MAX 8192

/* ARCHIVED: multi-layer background/overlay compositing.
 * The code paths below are retained, but runtime wiring is off. Independent
 * overlay vs content frame counts (e.g. 13 vs 19), animated overlay PNG cost,
 * and too many play-time variables made FPS and timing hard to reason about.
 * Set to 1 to restore the old behavior. */
#ifndef PD_CONTENT_COMPOSITING_ENABLED
#define PD_CONTENT_COMPOSITING_ENABLED 0
#endif

static const char *TAG = "pd-content";

static char content_base[PD_CONTENT_MAX_PATH] = "";
static bool content_playing = false;
static bool content_is_seq = false;
static bool content_is_sprite_scene = false;
/* After a classic FB transition into a sprite scene, enter assembled (no bump). */
static bool content_sprite_pending_assembled = false;
static char content_current[PD_CONTENT_MAX_PATH] = "";
static int  content_frame = 0;
static int  content_total_frames = 0;
static int  content_fps = 12;
static float content_achieved_fps = 0.0f;
static bool content_loop = true;
static int64_t content_last_frame_us = 0;
static char content_frame_pattern[64] = "%04d.png";  /* frame filename pattern */
static int  content_frame_start = 1;                  /* first frame index (0 or 1) */

/* ---- transition state ---- */
static pd_transition_t *content_transition = NULL;
static pd_framebuf_t   *content_fb = NULL;  /* current display framebuffer */

/* ---- background decode pipeline ----
 * decode_png_file() (LittleFS read + PNG inflate + composite) takes ~60ms,
 * and the display's bulk draw_pixels() call takes ~55ms — together they
 * capped playback at ~7-8fps even after switching off per-pixel set_pixel().
 * Since decode and render don't depend on each other for *different*
 * frames, a dedicated task on the other core decodes frame N+1 while the
 * main task is still busy pushing frame N to the panel, turning the
 * per-frame cost from decode+render into roughly max(decode, render). Only
 * one frame of lookahead is kept (not the whole sequence) to keep the RAM
 * cost to a single extra framebuffer regardless of sequence length. */
static pd_framebuf_t     *content_prefetch_fb = NULL;
static TaskHandle_t       content_decode_task = NULL;
static SemaphoreHandle_t  content_decode_request_sem = NULL;
static volatile bool      content_decode_busy = false;
static volatile bool      content_prefetch_valid = false;
static volatile int       content_prefetch_frame = -1;
static char               content_decode_req_base[PD_CONTENT_MAX_PATH];
static char               content_decode_req_pattern[64];
static volatile int       content_decode_req_frame = -1;
/* bumped on every play()/stop()/transition-play() so a decode that was
 * in-flight for content that's no longer current gets discarded instead of
 * being published into content_prefetch_fb for the wrong sequence. */
static volatile uint32_t  content_epoch = 0;

/* ---- global config ---- */
static pd_content_config_t content_config = {
    .trans_mode = PD_TRANS_MODE_BASELINE,
    .trans_baseline = "fade",
    .trans_duration_ms = 800,
    .hold_ms = 5000,
    .loop_sequences = true,
    .background = "#000000",
    .overlay = "",
    .attract_enabled = false,
    .attract_path = "images/",
    .attract_shuffle = true,
    .attract_idle_timeout_ms = 0,
    .auto_quantize_palette = false,
    .show_fps_counter = false,
};

/* ---- diagnostic event ring (GET /api/log) ---- */
#define PD_CONTENT_LOG_LINES 64
#define PD_CONTENT_LOG_LINE_LEN 160
static char content_log_lines[PD_CONTENT_LOG_LINES][PD_CONTENT_LOG_LINE_LEN];
static int content_log_head = 0;
static int content_log_count = 0;
static char content_cache_fail_reason[96] = "";

static void content_log_line(const char *fmt, ...)
{
    char *slot = content_log_lines[content_log_head];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, PD_CONTENT_LOG_LINE_LEN, fmt, ap);
    va_end(ap);
    content_log_head = (content_log_head + 1) % PD_CONTENT_LOG_LINES;
    if (content_log_count < PD_CONTENT_LOG_LINES) content_log_count++;
    ESP_LOGI(TAG, "%s", slot);
}

static void content_reset_achieved_fps(void);

/* ---- per-sequence 64-color PSRAM frame cache ----
 * Indices are stored at SOURCE frame size (e.g. 64x64), not the full matrix,
 * so wide canvases don't balloon SPIRAM. play() starts truecolor immediately;
 * a background worker builds/loads a cache. Multiple LRU slots keep recent
 * sequences warm across plays; a LittleFS sidecar (.pd_palcache) persists
 * across reboot. */
#define PD_PAL_CACHE_SLOTS    4  /* content + overlay + 2 warm LRU */
#define PD_PAL_CACHE_MAGIC    0x31434450u  /* 'PDC1' LE */
#define PD_PAL_CACHE_VERSION  1
#define PD_PAL_CACHE_FILENAME ".pd_palcache"

typedef struct {
    bool     in_use;
    bool     live;
    char     seq_path[PD_CONTENT_MAX_PATH];  /* absolute sequence dir */
    uint32_t pattern_hash;
    int      width;       /* source content width */
    int      height;
    int      frame_count;
    int      frame_start;
    int      palette_size;
    uint8_t  palette[PD_PALETTE_MAX][4];
    uint8_t *indices;     /* frame_count * width * height, SPIRAM */
    uint32_t last_used;
} pd_seq_cache_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    int16_t  frame_start;
    uint16_t palette_size;
    uint32_t pattern_hash;
    uint8_t  palette[PD_PALETTE_MAX][4];
} pd_palcache_hdr_t;

typedef enum {
    PD_PALCACHE_JOB_CONTENT = 0,
    PD_PALCACHE_JOB_OVERLAY = 1,
    PD_PALCACHE_JOB_EXPAND  = 2,  /* expand content frame N into back RGB */
} pd_palcache_job_t;

static pd_seq_cache_slot_t content_pal_slots[PD_PAL_CACHE_SLOTS];
static pd_seq_cache_slot_t *content_seq_cache_active = NULL;
static pd_seq_cache_slot_t *content_overlay_cache_active = NULL;
static uint32_t content_pal_lru_clock = 1;
static TaskHandle_t content_cache_task = NULL;
static volatile uint32_t content_cache_req_epoch = 0;
static volatile pd_palcache_job_t content_cache_job = PD_PALCACHE_JOB_CONTENT;
static volatile bool content_cache_building = false;
static volatile bool content_cache_fallback = false;
static volatile bool content_overlay_cache_building = false;
static uint32_t content_overlay_pattern_hash = 0;

/* Palette-path expand ping-pong: source-sized RGB in SPIRAM. Front is
 * presented on the main tick; the cache worker expands lookahead into back. */
static uint8_t *s_pal_rgb[2] = {NULL, NULL};
static size_t   s_pal_rgb_cap = 0;
static int      s_pal_rgb_w = 0;
static int      s_pal_rgb_h = 0;
static int      s_pal_front = 0;
static volatile int  s_pal_ready_frame = -1; /* frame sitting in back buffer */
static volatile bool s_pal_back_ready = false;
static volatile int  s_pal_expand_req_frame = -1;
static uint8_t *s_pal_rgba_scratch = NULL;   /* content expand RGBA */
static size_t   s_pal_rgba_scratch_cap = 0;
#if PD_CONTENT_COMPOSITING_ENABLED
static uint8_t *s_ov_rgba_scratch = NULL;    /* overlay expand RGBA (separate) */
static size_t   s_ov_rgba_scratch_cap = 0;
#endif

/* Current sequence source geometry + identity (set in content_setup_playback). */
static int content_src_w = 0;
static int content_src_h = 0;
static uint32_t content_pattern_hash = 0;
static char content_rel_path[PD_CONTENT_MAX_PATH] = "";

static uint32_t content_fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/* ---- compositing state (archived when PD_CONTENT_COMPOSITING_ENABLED=0) ---- */
#if !PD_CONTENT_COMPOSITING_ENABLED
#define PD_COMP_UNUSED __attribute__((unused))
#else
#define PD_COMP_UNUSED
#endif
static PD_COMP_UNUSED uint8_t *cached_background_rgb = NULL;
static PD_COMP_UNUSED uint8_t *cached_overlay_rgba = NULL;
static PD_COMP_UNUSED char cached_bg_path[PD_CONTENT_MAX_PATH] = "";
static PD_COMP_UNUSED char cached_overlay_path[PD_CONTENT_MAX_PATH] = "";
static PD_COMP_UNUSED int cached_width = 0;
static PD_COMP_UNUSED int cached_height = 0;

/* saved global defaults (restored when switching content) */
static char saved_background[PD_CONTENT_MAX_PATH] = "#000000";
static char saved_overlay[PD_CONTENT_MAX_PATH] = "";

/* source-status overlay state (on-demand screen) */
static int64_t status_overlay_until_us = 0;
static int64_t status_last_render_us = 0;
static char status_resume_path[PD_CONTENT_MAX_PATH] = "";
static bool status_resume_was_playing = false;
static bool status_overlay_just_expired = false;

/* animated overlay state */
static PD_COMP_UNUSED bool overlay_is_seq = false;
static PD_COMP_UNUSED int overlay_fps = 12;
static PD_COMP_UNUSED int overlay_frame = 0;
static PD_COMP_UNUSED int overlay_frame_start = 0;
static PD_COMP_UNUSED int overlay_total_frames = 0;
static PD_COMP_UNUSED char overlay_frame_pattern[64] = "%04d.png";
static PD_COMP_UNUSED char overlay_base_path[PD_CONTENT_MAX_PATH] = "";
static PD_COMP_UNUSED int64_t overlay_last_frame_us = 0;

/* Dual-path panel push: for sparse content (src < canvas, no overlay /
 * transition) push only the content rect. Tracks the last drawn rect so a
 * later position change can erase old∖new (motion-ready). */
static bool content_sprite_last_valid = false;
static int  content_sprite_last_x = 0;
static int  content_sprite_last_y = 0;
static int  content_sprite_last_w = 0;
static int  content_sprite_last_h = 0;
static bool content_bounds_logged = false;

/* Prefetch keeps a source-sized RGB copy so bounds mode can avoid a full
 * canvas panel push after a successful lookahead decode. */
static uint8_t *content_prefetch_src_rgb = NULL;
static size_t   content_prefetch_src_cap = 0;
static int      content_prefetch_src_w = 0;
static int      content_prefetch_src_h = 0;

/* ---- helpers ---- */

static bool is_png(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return false;
    return strcasecmp(name + len - 4, ".png") == 0;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool path_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

/* Read a JSON number or numeric string into *out. Returns false if absent/invalid. */
static bool json_get_int(const cJSON *item, int *out)
{
    if (!item || !out) return false;
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v > (double)INT_MAX) v = (double)INT_MAX;
        if (v < (double)INT_MIN) v = (double)INT_MIN;
        *out = (int)v;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        char *end = NULL;
        long v = strtol(item->valuestring, &end, 10);
        if (end && end != item->valuestring && *end == '\0') {
            *out = (int)v;
            return true;
        }
    }
    return false;
}

/* Count contiguous frames for a printf pattern (e.g. "%04d.png" or "mai%04d.png"). */
static int count_sequence_frames_pattern(const char *dir_path, const char *pattern, int start)
{
    if (!pattern || !pattern[0]) pattern = "%04d.png";
    if (start < 0) start = 0;

    int count = 0;
    char path[PD_CONTENT_MAX_PATH];
    for (int i = start; i < start + 9999; i++) {
        int n = snprintf(path, sizeof(path), "%s/", dir_path);
        if (n < 0 || n >= (int)sizeof(path)) break;
        int m = snprintf(path + n, sizeof(path) - (size_t)n, pattern, i);
        if (m < 0 || m >= (int)sizeof(path) - n) break;
        if (!path_exists(path)) break;
        count++;
    }
    return count;
}

/* Legacy helper: plain 0001.png … N.png */
static int count_sequence_frames(const char *dir_path)
{
    return count_sequence_frames_pattern(dir_path, "%04d.png", 1);
}

/* Discover pattern/start/count from PNGs in a folder when meta.json is absent. */
static bool discover_sequence_from_dir(const char *dir_path, char *pattern_out, size_t pattern_size,
                                       int *start_out, int *frame_count_out)
{
    DIR *d = opendir(dir_path);
    if (!d) return false;

    char first_name[64] = "";
    char prefix[64] = "";
    int digit_width = 0;
    int first_num = -1;
    int file_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcasecmp(name + len - 4, ".png") != 0) continue;
        if (strcasecmp(name, "background.png") == 0) continue;

        file_count++;
        if (first_name[0] == '\0' || strcmp(name, first_name) < 0) {
            strlcpy(first_name, name, sizeof(first_name));
        }
    }
    closedir(d);

    if (file_count == 0 || first_name[0] == '\0') return false;

    /* Strip .png and trailing digits → prefix + width + start */
    size_t stem_len = strlen(first_name);
    if (stem_len > 4) stem_len -= 4; /* remove .png */
    size_t digits = 0;
    while (stem_len > digits && isdigit((unsigned char)first_name[stem_len - 1 - digits])) {
        digits++;
    }
    if (digits == 0 || digits > 8) {
        /* Fall back to plain %04d.png from 1 or 0 */
        int c1 = count_sequence_frames_pattern(dir_path, "%04d.png", 1);
        int c0 = count_sequence_frames_pattern(dir_path, "%04d.png", 0);
        if (c1 <= 0 && c0 <= 0) return false;
        if (pattern_out && pattern_size) strlcpy(pattern_out, "%04d.png", pattern_size);
        if (start_out) *start_out = (c0 > c1) ? 0 : 1;
        if (frame_count_out) *frame_count_out = (c0 > c1) ? c0 : c1;
        return true;
    }

    size_t prefix_len = stem_len - digits;
    if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
    memcpy(prefix, first_name, prefix_len);
    prefix[prefix_len] = '\0';
    digit_width = (int)digits;

    char numbuf[16];
    if (digits >= sizeof(numbuf)) return false;
    memcpy(numbuf, first_name + prefix_len, digits);
    numbuf[digits] = '\0';
    first_num = atoi(numbuf);

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s%%0%dd.png", prefix, digit_width);
    int count = count_sequence_frames_pattern(dir_path, pattern, first_num);
    if (count <= 0) return false;

    if (pattern_out && pattern_size) strlcpy(pattern_out, pattern, pattern_size);
    if (start_out) *start_out = first_num;
    if (frame_count_out) *frame_count_out = count;
    return true;
}

static bool load_sequence_meta(const char *dir_path, int *fps, bool *loop_out, int *frame_count,
                               char *pattern_out, size_t pattern_size, int *start_out,
                               char *bg_out, size_t bg_size, char *ov_out, size_t ov_size,
                               int *width_out, int *height_out)
{
    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/meta.json", dir_path);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4096) { fclose(f); return false; }

    char *buf = calloc(1, sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *j_fps = cJSON_GetObjectItem(root, "fps");
    cJSON *j_loop = cJSON_GetObjectItem(root, "loop");
    cJSON *j_frames = cJSON_GetObjectItem(root, "frames");
    cJSON *j_pattern = cJSON_GetObjectItem(root, "pattern");
    cJSON *j_start = cJSON_GetObjectItem(root, "start");
    cJSON *j_bg = cJSON_GetObjectItem(root, "background");
    cJSON *j_ov = cJSON_GetObjectItem(root, "overlay");
    cJSON *j_w = cJSON_GetObjectItem(root, "width");
    cJSON *j_h = cJSON_GetObjectItem(root, "height");

    int tmp = 0;
    if (fps && json_get_int(j_fps, &tmp) && tmp > 0) *fps = tmp;
    if (loop_out) *loop_out = cJSON_IsBool(j_loop) ? cJSON_IsTrue(j_loop) : true;

    if (pattern_out && cJSON_IsString(j_pattern) && j_pattern->valuestring) {
        strlcpy(pattern_out, j_pattern->valuestring, pattern_size);
    }
    if (start_out) {
        if (!json_get_int(j_start, start_out)) {
            /* keep caller default */
        }
    }

    if (frame_count) {
        if (json_get_int(j_frames, &tmp) && tmp > 0) {
            *frame_count = tmp;
        } else {
            const char *pat = (pattern_out && pattern_out[0]) ? pattern_out : "%04d.png";
            int start = start_out ? *start_out : 1;
            *frame_count = count_sequence_frames_pattern(dir_path, pat, start);
            if (*frame_count <= 0) {
                *frame_count = count_sequence_frames(dir_path);
            }
        }
    }
    if (bg_out && cJSON_IsString(j_bg)) {
        strlcpy(bg_out, j_bg->valuestring, bg_size);
    }
    if (ov_out && cJSON_IsString(j_ov)) {
        strlcpy(ov_out, j_ov->valuestring, ov_size);
    }
    if (width_out && json_get_int(j_w, &tmp) && tmp > 0) {
        *width_out = tmp;
    }
    if (height_out && json_get_int(j_h, &tmp) && tmp > 0) {
        *height_out = tmp;
    }

    cJSON_Delete(root);
    return true;
}

/* ---- config load/save ---- */

static void load_config(void)
{
    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/config.json", content_base);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGI(TAG, "no config.json, using defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4096) { fclose(f); return; }

    char *buf = calloc(1, sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "config.json parse failed");
        return;
    }

    /* transition section */
    cJSON *trans = cJSON_GetObjectItem(root, "transition");
    if (trans) {
        cJSON *mode = cJSON_GetObjectItem(trans, "mode");
        if (cJSON_IsString(mode)) {
            if (strcmp(mode->valuestring, "random") == 0)
                content_config.trans_mode = PD_TRANS_MODE_RANDOM;
            else if (strcmp(mode->valuestring, "baseline") == 0)
                content_config.trans_mode = PD_TRANS_MODE_BASELINE;
            else if (strcmp(mode->valuestring, "per-item") == 0)
                content_config.trans_mode = PD_TRANS_MODE_PER_ITEM;
        }
        cJSON *baseline = cJSON_GetObjectItem(trans, "baseline");
        if (cJSON_IsString(baseline))
            strlcpy(content_config.trans_baseline, baseline->valuestring,
                    sizeof(content_config.trans_baseline));
        cJSON *dur = cJSON_GetObjectItem(trans, "duration_ms");
        if (cJSON_IsNumber(dur))
            content_config.trans_duration_ms = dur->valueint;
    }

    /* display section */
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (disp) {
        cJSON *hold = cJSON_GetObjectItem(disp, "hold_ms");
        if (cJSON_IsNumber(hold))
            content_config.hold_ms = hold->valueint;
        cJSON *loop_seq = cJSON_GetObjectItem(disp, "loop_sequences");
        if (cJSON_IsBool(loop_seq))
            content_config.loop_sequences = cJSON_IsTrue(loop_seq);
        cJSON *bg = cJSON_GetObjectItem(disp, "background");
        if (cJSON_IsString(bg))
            strlcpy(content_config.background, bg->valuestring,
                    sizeof(content_config.background));
        cJSON *ov = cJSON_GetObjectItem(disp, "overlay");
        if (cJSON_IsString(ov))
            strlcpy(content_config.overlay, ov->valuestring,
                    sizeof(content_config.overlay));
        cJSON *aq = cJSON_GetObjectItem(disp, "auto_quantize_palette");
        if (cJSON_IsBool(aq))
            content_config.auto_quantize_palette = cJSON_IsTrue(aq);
        cJSON *sfc = cJSON_GetObjectItem(disp, "show_fps_counter");
        if (cJSON_IsBool(sfc))
            content_config.show_fps_counter = cJSON_IsTrue(sfc);
    }

    /* attract section */
    cJSON *attr = cJSON_GetObjectItem(root, "attract");
    if (attr) {
        cJSON *enabled = cJSON_GetObjectItem(attr, "enabled");
        if (cJSON_IsBool(enabled))
            content_config.attract_enabled = cJSON_IsTrue(enabled);
        cJSON *apath = cJSON_GetObjectItem(attr, "path");
        if (cJSON_IsString(apath))
            strlcpy(content_config.attract_path, apath->valuestring,
                    sizeof(content_config.attract_path));
        cJSON *shuffle = cJSON_GetObjectItem(attr, "shuffle");
        if (cJSON_IsBool(shuffle))
            content_config.attract_shuffle = cJSON_IsTrue(shuffle);
        cJSON *idle = cJSON_GetObjectItem(attr, "idle_timeout_ms");
        if (cJSON_IsNumber(idle))
            content_config.attract_idle_timeout_ms = idle->valueint;
    }

    cJSON_Delete(root);

    /* save global defaults for restoration when switching content */
    strlcpy(saved_background, content_config.background, sizeof(saved_background));
    strlcpy(saved_overlay, content_config.overlay, sizeof(saved_overlay));

    ESP_LOGI(TAG, "config loaded: mode=%d baseline=%s dur=%d auto_quantize=%d show_fps=%d",
             content_config.trans_mode, content_config.trans_baseline,
             content_config.trans_duration_ms, (int)content_config.auto_quantize_palette,
             (int)content_config.show_fps_counter);
}

const pd_content_config_t *pd_content_get_config(void)
{
    return &content_config;
}

esp_err_t pd_content_set_config(const pd_content_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memcpy(&content_config, cfg, sizeof(content_config));
    return ESP_OK;
}

/* select transition based on config mode */
static const char *select_transition_name(void)
{
    switch (content_config.trans_mode) {
    case PD_TRANS_MODE_RANDOM:
        return pd_transition_type_name(pd_transition_random());
    case PD_TRANS_MODE_BASELINE:
    default:
        return content_config.trans_baseline;
    case PD_TRANS_MODE_PER_ITEM:
        /* per-item would check item meta, but for now fall back to baseline */
        return content_config.trans_baseline;
    }
}

esp_err_t pd_content_save_config(void)
{
    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/config.json", content_base);

    const char *mode_str = "baseline";
    if (content_config.trans_mode == PD_TRANS_MODE_RANDOM) mode_str = "random";
    else if (content_config.trans_mode == PD_TRANS_MODE_PER_ITEM) mode_str = "per-item";

    cJSON *root = cJSON_CreateObject();
    cJSON *trans = cJSON_AddObjectToObject(root, "transition");
    cJSON_AddStringToObject(trans, "mode", mode_str);
    cJSON_AddStringToObject(trans, "baseline", content_config.trans_baseline);
    cJSON_AddNumberToObject(trans, "duration_ms", content_config.trans_duration_ms);

    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(disp, "hold_ms", content_config.hold_ms);
    cJSON_AddBoolToObject(disp, "loop_sequences", content_config.loop_sequences);
    /* Persist global defaults only — never the per-item play overrides that
     * temporarily live in content_config while lizard/etc. is playing. */
    cJSON_AddStringToObject(disp, "background", saved_background);
    cJSON_AddStringToObject(disp, "overlay", saved_overlay);
    cJSON_AddBoolToObject(disp, "auto_quantize_palette", content_config.auto_quantize_palette);
    cJSON_AddBoolToObject(disp, "show_fps_counter", content_config.show_fps_counter);

    cJSON *attr = cJSON_AddObjectToObject(root, "attract");
    cJSON_AddBoolToObject(attr, "enabled", content_config.attract_enabled);
    cJSON_AddStringToObject(attr, "path", content_config.attract_path);
    cJSON_AddBoolToObject(attr, "shuffle", content_config.attract_shuffle);
    cJSON_AddNumberToObject(attr, "idle_timeout_ms", content_config.attract_idle_timeout_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    ESP_LOGI(TAG, "config saved");
    return ESP_OK;
}

/* ---- PNG decode ---- */

static uint8_t *decode_png_rgba(const char *path, unsigned *w, unsigned *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) { fclose(f); return NULL; }

    unsigned char *png_data = malloc(sz);
    if (!png_data) { fclose(f); return NULL; }
    fread(png_data, 1, sz, f);
    fclose(f);

    unsigned char *rgba = NULL;
    unsigned error = lodepng_decode32(&rgba, w, h, png_data, sz);
    free(png_data);

    if (error || !rgba) {
        ESP_LOGE(TAG, "lodepng error %u: %s", error, lodepng_error_text(error));
        return NULL;
    }
    return rgba;
}

/* Decoded pixel data in whichever form is cheapest for the source PNG.
 * Paletted (indexed-color, PNG color type 3) sources decode to 1 byte/pixel
 * plus a small (<=256 entry) palette instead of being expanded to full
 * RGBA — cheaper to decode (smaller file on flash, less zlib inflate work)
 * and cheaper to composite (palette lookup instead of always-blend math).
 * Non-indexed PNGs (existing truecolor content) fall back to the RGBA path
 * unchanged, so nothing needs to be re-exported for this to keep working. */
typedef struct {
    bool     is_indexed;
    uint8_t *indices;          /* w*h bytes, only valid if is_indexed */
    uint8_t  palette[256][4];  /* RGBA per index, only valid if is_indexed */
    int      palette_size;
    uint8_t *rgba;             /* w*h*4 bytes, only valid if !is_indexed */
} pd_decoded_frame_t;

static void free_decoded_frame(pd_decoded_frame_t *f)
{
    if (!f) return;
    free(f->indices);
    free(f->rgba);
    f->indices = NULL;
    f->rgba = NULL;
}

static bool decode_png_indexed_or_rgba(const char *path, unsigned *w, unsigned *h,
                                        pd_decoded_frame_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return false; }

    unsigned char *png_data = malloc(sz);
    if (!png_data) { fclose(f); return false; }
    fread(png_data, 1, sz, f);
    fclose(f);

    LodePNGState state;
    lodepng_state_init(&state);

    unsigned iw, ih;
    unsigned err = lodepng_inspect(&iw, &ih, &state, png_data, sz);
    bool source_is_palette = (!err && state.info_png.color.colortype == LCT_PALETTE);

    if (source_is_palette) {
        state.info_raw.colortype = LCT_PALETTE;
        state.info_raw.bitdepth = 8;
        unsigned char *raw = NULL;
        err = lodepng_decode(&raw, w, h, &state, png_data, sz);
        if (!err && raw) {
            int n = (int)state.info_png.color.palettesize;
            if (n > 256) n = 256;
            out->is_indexed = true;
            out->indices = raw;
            out->palette_size = n;
            memcpy(out->palette, state.info_png.color.palette, (size_t)n * 4);
            lodepng_state_cleanup(&state);
            free(png_data);
            return true;
        }
        free(raw);  /* no-op if NULL — defensive against a partial alloc on error */
    }
    lodepng_state_cleanup(&state);

    /* fallback: not a paletted PNG (or the indexed decode failed for some
     * reason) — decode straight to RGBA exactly as before. */
    unsigned char *rgba = NULL;
    err = lodepng_decode32(&rgba, w, h, png_data, sz);
    free(png_data);
    if (err || !rgba) {
        ESP_LOGE(TAG, "lodepng error %u: %s", err, lodepng_error_text(err));
        return false;
    }
    out->is_indexed = false;
    out->rgba = rgba;
    return true;
}

#if PD_CONTENT_COMPOSITING_ENABLED
static bool parse_hex_color(const char *str, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!str || str[0] != '#' || strlen(str) != 7) return false;
    unsigned int val;
    if (sscanf(str + 1, "%06x", &val) != 1) return false;
    *r = (val >> 16) & 0xFF;
    *g = (val >> 8) & 0xFF;
    *b = val & 0xFF;
    return true;
}
#endif

/* Drop decoded bg/overlay caches and animated-overlay playback state. */
static void content_reset_compositing_state(void)
{
    free(cached_background_rgb);
    cached_background_rgb = NULL;
    free(cached_overlay_rgba);
    cached_overlay_rgba = NULL;
    cached_bg_path[0] = '\0';
    cached_overlay_path[0] = '\0';
    overlay_is_seq = false;
    overlay_total_frames = 0;
    overlay_frame = 0;
    overlay_frame_start = 0;
    overlay_base_path[0] = '\0';
    overlay_last_frame_us = 0;
    content_overlay_cache_active = NULL;
    content_overlay_cache_building = false;
    content_overlay_pattern_hash = 0;
    strlcpy(overlay_frame_pattern, "%04d.png", sizeof(overlay_frame_pattern));
}

#if PD_CONTENT_COMPOSITING_ENABLED
static void content_request_overlay_cache_build(void);
static uint8_t *content_ov_ensure_rgba_scratch(size_t pixels);
#endif
static void content_request_pal_expand(int frame);

static void update_compositing_cache(int width, int height)
{
#if !PD_CONTENT_COMPOSITING_ENABLED
    (void)width;
    (void)height;
    return;
#else
    /* update background cache if changed */
    const char *bg = content_config.background;
    if (strcmp(bg, cached_bg_path) != 0) {
        free(cached_background_rgb);
        cached_background_rgb = NULL;
        strlcpy(cached_bg_path, bg, sizeof(cached_bg_path));

        if (bg[0] == '#') {
            /* solid color */
            uint8_t r, g, b;
            if (parse_hex_color(bg, &r, &g, &b)) {
                cached_background_rgb = malloc(width * height * 3);
                if (cached_background_rgb) {
                    for (int i = 0; i < width * height; i++) {
                        cached_background_rgb[i * 3 + 0] = r;
                        cached_background_rgb[i * 3 + 1] = g;
                        cached_background_rgb[i * 3 + 2] = b;
                    }
                }
            }
        } else if (bg[0] != '\0') {
            /* image path */
            char full[PD_CONTENT_MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", content_base, bg);
            unsigned w, h;
            uint8_t *rgba = decode_png_rgba(full, &w, &h);
            if (rgba && (int)w == width && (int)h == height) {
                cached_background_rgb = malloc(width * height * 3);
                if (cached_background_rgb) {
                    for (int i = 0; i < width * height; i++) {
                        cached_background_rgb[i * 3 + 0] = rgba[i * 4 + 0];
                        cached_background_rgb[i * 3 + 1] = rgba[i * 4 + 1];
                        cached_background_rgb[i * 3 + 2] = rgba[i * 4 + 2];
                    }
                }
            }
            free(rgba);
        }
    }

    /* update overlay cache if changed */
    const char *ov = content_config.overlay;
    if (strcmp(ov, cached_overlay_path) != 0) {
        free(cached_overlay_rgba);
        cached_overlay_rgba = NULL;
        overlay_is_seq = false;
        overlay_total_frames = 0;
        strlcpy(cached_overlay_path, ov, sizeof(cached_overlay_path));

        if (ov[0] != '\0') {
            char full[PD_CONTENT_MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", content_base, ov);

            if (path_is_dir(full)) {
                /* animated overlay sequence */
                overlay_is_seq = true;
                strlcpy(overlay_base_path, full, sizeof(overlay_base_path));
                strlcpy(overlay_frame_pattern, "%04d.png", sizeof(overlay_frame_pattern));
                overlay_fps = 12;
                overlay_frame_start = 0;
                overlay_total_frames = 0;
                load_sequence_meta(full, &overlay_fps, NULL, &overlay_total_frames,
                                   overlay_frame_pattern, sizeof(overlay_frame_pattern),
                                   &overlay_frame_start, NULL, 0, NULL, 0, NULL, NULL);
                if (overlay_total_frames == 0) {
                    overlay_total_frames = count_sequence_frames_pattern(
                        full, overlay_frame_pattern, overlay_frame_start);
                }
                if (overlay_total_frames == 0) {
                    int disc_start = 0;
                    int disc_frames = 0;
                    if (discover_sequence_from_dir(full, overlay_frame_pattern,
                                                   sizeof(overlay_frame_pattern),
                                                   &disc_start, &disc_frames)) {
                        overlay_frame_start = disc_start;
                        overlay_total_frames = disc_frames;
                    }
                }
                overlay_frame = overlay_frame_start;
                overlay_last_frame_us = 0;
                content_overlay_pattern_hash = content_fnv1a(overlay_frame_pattern);
                ESP_LOGI(TAG, "animated overlay: %s (%d frames @ %d fps)",
                         ov, overlay_total_frames, overlay_fps);
                content_request_overlay_cache_build();
            } else {
                /* static overlay */
                unsigned w, h;
                cached_overlay_rgba = decode_png_rgba(full, &w, &h);
                if (cached_overlay_rgba && ((int)w != width || (int)h != height)) {
                    free(cached_overlay_rgba);
                    cached_overlay_rgba = NULL;
                    ESP_LOGW(TAG, "overlay size mismatch");
                }
            }
        }
    }

    cached_width = width;
    cached_height = height;
#endif /* PD_CONTENT_COMPOSITING_ENABLED */
}

static bool content_has_active_overlay(void)
{
#if !PD_CONTENT_COMPOSITING_ENABLED
    return false;
#else
    if (content_config.overlay[0] != '\0') return true;
    if (overlay_is_seq && overlay_total_frames > 0) return true;
    if (cached_overlay_rgba) return true;
    return false;
#endif
}

/* True when expand can write RGB directly (no bg/overlay composite). */
static bool content_compositing_is_trivial(void)
{
#if !PD_CONTENT_COMPOSITING_ENABLED
    return true;
#else
    if (content_has_active_overlay()) return false;
    const char *bg = content_config.background;
    if (bg[0] == '\0') return true;
    if (bg[0] == '#') {
        uint8_t r, g, b;
        if (parse_hex_color(bg, &r, &g, &b) && r == 0 && g == 0 && b == 0) return true;
        return false;
    }
    return false;
#endif
}

/* Bounds-limited draws when content is smaller than the matrix and nothing
 * requires a full-canvas composite (overlay / active transition). Full-bleed
 * art and overlays keep the classic full-framebuffer path. */
static bool content_bounds_draw_eligible(int sw, int sh)
{
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    if (sw >= dw && sh >= dh) return false;
    if (content_has_active_overlay()) return false;
    if (content_transition && pd_transition_is_active(content_transition)) return false;
    return true;
}

static void content_sprite_invalidate(void)
{
    content_sprite_last_valid = false;
    content_bounds_logged = false;
}

/* Fill axis-aligned set difference old∖new (up to four strips). */
static void content_erase_rect_diff(int ox, int oy, int ow, int oh,
                                    int nx, int ny, int nw, int nh,
                                    pd_display_color_t bg)
{
    if (ow <= 0 || oh <= 0) return;

    int ox1 = ox + ow;
    int oy1 = oy + oh;
    int nx1 = nx + nw;
    int ny1 = ny + nh;

    int ix0 = ox > nx ? ox : nx;
    int iy0 = oy > ny ? oy : ny;
    int ix1 = ox1 < nx1 ? ox1 : nx1;
    int iy1 = oy1 < ny1 ? oy1 : ny1;

    if (ix0 >= ix1 || iy0 >= iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)oh, bg);
        return;
    }

    if (oy < iy0) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)(iy0 - oy), bg);
    }
    if (oy1 > iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy1, (uint16_t)ow, (uint16_t)(oy1 - iy1), bg);
    }
    if (ox < ix0) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy0, (uint16_t)(ix0 - ox), (uint16_t)(iy1 - iy0), bg);
    }
    if (ox1 > ix1) {
        pd_display_fill((uint16_t)ix1, (uint16_t)iy0, (uint16_t)(ox1 - ix1), (uint16_t)(iy1 - iy0), bg);
    }
}

/* Bounds present at an explicit canvas position (motion-ready). Today callers
 * pass the centered origin; later motion can change x/y each frame. */
static void content_present_bounds_at(const uint8_t *src_rgb, int x, int y, int sw, int sh)
{
    if (!src_rgb || sw <= 0 || sh <= 0) return;

    if (!content_sprite_last_valid) {
        /* Entering bounds (or after full-path): clear leftovers once. */
        pd_display_clear();
        if (!content_bounds_logged) {
            ESP_LOGI(TAG, "draw: bounds/dirty-rect mode (content push + erase on move)");
            content_bounds_logged = true;
        }
    } else if (content_sprite_last_x != x || content_sprite_last_y != y ||
               content_sprite_last_w != sw || content_sprite_last_h != sh) {
        content_erase_rect_diff(content_sprite_last_x, content_sprite_last_y,
                                content_sprite_last_w, content_sprite_last_h,
                                x, y, sw, sh, PD_COLOR_BLACK);
    }

    pd_display_render_rgb_at(x, y, src_rgb, sw, sh);
    content_sprite_last_x = x;
    content_sprite_last_y = y;
    content_sprite_last_w = sw;
    content_sprite_last_h = sh;
    content_sprite_last_valid = true;
}

/* Tiny on-panel FPS HUD (ASCII: "24>6.2" or "24>-"). Drawn after present so
 * it sits on top of the frame; no flip — content path does not flip either. */
static void content_draw_fps_hud(void)
{
    if (!content_config.show_fps_counter || !content_playing || !content_is_seq) {
        return;
    }
    char line[20];
    if (content_achieved_fps > 0.05f) {
        snprintf(line, sizeof(line), "%d>%.1f", content_fps, (double)content_achieved_fps);
    } else {
        snprintf(line, sizeof(line), "%d>-", content_fps);
    }
    /* Dark bar behind glyphs so they stay readable on bright frames. */
    pd_display_fill(0, 0, (uint16_t)(strlen(line) * 4 + 2), 7, PD_COLOR_BLACK);
    pd_display_draw_text_tiny(1, 1, line, PD_COLOR_YELLOW);
}

/* Authoritative on-screen canvas for transition "from" captures.
 * Always kept in sync on present — never point at caller buffers that may
 * be free()'d (that made from≈to and zoom looked like B→B). */
static const uint8_t *content_last_src_rgb = NULL;
static int content_last_src_w = 0;
static int content_last_src_h = 0;
static bool content_fb_in_sync = false;

static void content_sync_fb_from_src(const uint8_t *rgb, int sw, int sh)
{
    if (!content_fb || !rgb || sw <= 0 || sh <= 0) return;
    pd_framebuf_blit_rgb(content_fb, rgb, sw, sh);
    content_fb_in_sync = true;
    content_last_src_rgb = content_fb->data;
    content_last_src_w = content_fb->width;
    content_last_src_h = content_fb->height;
}

static void content_ensure_fb_current(void)
{
    if (content_fb_in_sync || !content_fb) return;
    /* Only trust last_src when it aliases content_fb (post-sync present). */
    if (content_last_src_rgb == content_fb->data &&
        content_last_src_w > 0 && content_last_src_h > 0) {
        content_fb_in_sync = true;
        return;
    }
    if (!content_last_src_rgb || content_last_src_w <= 0 || content_last_src_h <= 0) {
        return;
    }
    pd_framebuf_blit_rgb(content_fb, content_last_src_rgb,
                         content_last_src_w, content_last_src_h);
    content_last_src_rgb = content_fb->data;
    content_last_src_w = content_fb->width;
    content_last_src_h = content_fb->height;
    content_fb_in_sync = true;
}

/* Panel push only. `src_rgb` is composited source-sized RGB when available.
 * When src is provided, keep content_fb in sync for the next transition. */
static void content_present_panel(const uint8_t *src_rgb, int sw, int sh)
{
    if (src_rgb && sw > 0 && sh > 0) {
        content_sync_fb_from_src(src_rgb, sw, sh);
    }

    if (src_rgb && content_bounds_draw_eligible(sw, sh)) {
        int dw = pd_display_get_width();
        int dh = pd_display_get_height();
        int x = (sw < dw) ? (dw - sw) / 2 : 0;
        int y = (sh < dh) ? (dh - sh) / 2 : 0;
        content_present_bounds_at(src_rgb, x, y, sw, sh);
        content_draw_fps_hud();
        return;
    }
    content_sprite_invalidate();
    /* Prefer source RGB (avoids requiring a prior full-FB blit). Fall back
     * to content_fb only when no source was provided. */
    if (src_rgb) {
        pd_display_render_rgb(src_rgb, sw, sh);
    } else if (content_fb) {
        content_fb_in_sync = true;
        content_last_src_rgb = content_fb->data;
        content_last_src_w = content_fb->width;
        content_last_src_h = content_fb->height;
        pd_display_render_framebuf(content_fb->data);
    }
    content_draw_fps_hud();
}

/* Push to the panel; content_fb is synced inside content_present_panel. */
static void content_present_source_rgb(const uint8_t *rgb, int sw, int sh)
{
    if (!rgb || sw <= 0 || sh <= 0) return;
    content_present_panel(rgb, sw, sh);
}

static bool content_prefetch_store_src(const uint8_t *rgb, int w, int h)
{
    if (!rgb || w <= 0 || h <= 0) {
        content_prefetch_src_w = 0;
        content_prefetch_src_h = 0;
        return false;
    }
    size_t need = (size_t)w * (size_t)h * 3;
    if (need > content_prefetch_src_cap) {
        uint8_t *nbuf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nbuf) {
            nbuf = malloc(need);
        }
        if (!nbuf) {
            content_prefetch_src_w = 0;
            content_prefetch_src_h = 0;
            return false;
        }
        free(content_prefetch_src_rgb);
        content_prefetch_src_rgb = nbuf;
        content_prefetch_src_cap = need;
    }
    memcpy(content_prefetch_src_rgb, rgb, need);
    content_prefetch_src_w = w;
    content_prefetch_src_h = h;
    return true;
}

static uint8_t *composite_frame(uint8_t *content_rgba, int width, int height)
{
    size_t pixels = (size_t)width * (size_t)height;
    uint8_t *rgb = malloc(pixels * 3);
    if (!rgb) return NULL;

#if !PD_CONTENT_COMPOSITING_ENABLED
    /* Archived: ignore bg/overlay — flatten content onto black. */
    for (size_t i = 0; i < pixels; i++) {
        uint8_t a = content_rgba[i * 4 + 3];
        if (a == 0) {
            rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = 0;
        } else {
            rgb[i * 3 + 0] = content_rgba[i * 4 + 0];
            rgb[i * 3 + 1] = content_rgba[i * 4 + 1];
            rgb[i * 3 + 2] = content_rgba[i * 4 + 2];
        }
    }
    return rgb;
#else
    update_compositing_cache(width, height);

    /* layer 1: background (or black if none) */
    if (cached_background_rgb) {
        memcpy(rgb, cached_background_rgb, pixels * 3);
    } else {
        memset(rgb, 0, pixels * 3);
    }

    /* layer 2: content with alpha blending.
     * Fast path: fully-opaque content (the overwhelming common case for
     * sprite/marquee art) needs no blend math at all — just copy. This and
     * the >>8 shift below (instead of /255) avoid a per-pixel integer
     * divide, which was the dominant cost of compositing every animation
     * frame and made playback noticeably slower than the configured fps. */
    for (size_t i = 0; i < pixels; i++) {
        uint8_t ca = content_rgba[i * 4 + 3];
        if (ca == 255) {
            rgb[i * 3 + 0] = content_rgba[i * 4 + 0];
            rgb[i * 3 + 1] = content_rgba[i * 4 + 1];
            rgb[i * 3 + 2] = content_rgba[i * 4 + 2];
            continue;
        }
        if (ca == 0) continue;  /* fully transparent — background already in place */
        uint8_t cr = content_rgba[i * 4 + 0];
        uint8_t cg = content_rgba[i * 4 + 1];
        uint8_t cb = content_rgba[i * 4 + 2];
        uint8_t br = rgb[i * 3 + 0];
        uint8_t bg = rgb[i * 3 + 1];
        uint8_t bb = rgb[i * 3 + 2];
        uint8_t inv = 255 - ca;
        rgb[i * 3 + 0] = (uint8_t)((cr * ca + br * inv) >> 8);
        rgb[i * 3 + 1] = (uint8_t)((cg * ca + bg * inv) >> 8);
        rgb[i * 3 + 2] = (uint8_t)((cb * ca + bb * inv) >> 8);
    }

    /* layer 3: overlay with alpha blending */
    uint8_t *overlay_rgba = NULL;
    bool overlay_owned = false;
    pd_seq_cache_slot_t *ov_slot = content_overlay_cache_active;
    if (overlay_is_seq && overlay_total_frames > 0 && ov_slot && ov_slot->live &&
        ov_slot->indices && ov_slot->width == width && ov_slot->height == height) {
        int oidx = overlay_frame - ov_slot->frame_start;
        if (oidx >= 0 && oidx < ov_slot->frame_count) {
            overlay_rgba = content_ov_ensure_rgba_scratch(pixels);
            if (overlay_rgba) {
                const uint8_t *isrc =
                    ov_slot->indices + (size_t)oidx * pixels;
                for (size_t i = 0; i < pixels; i++) {
                    uint8_t pi = isrc[i];
                    if (pi >= (uint8_t)ov_slot->palette_size) pi = 0;
                    overlay_rgba[i * 4 + 0] = ov_slot->palette[pi][0];
                    overlay_rgba[i * 4 + 1] = ov_slot->palette[pi][1];
                    overlay_rgba[i * 4 + 2] = ov_slot->palette[pi][2];
                    overlay_rgba[i * 4 + 3] = ov_slot->palette[pi][3];
                }
                /* Overlay scratch is reused — not freed below. */
                overlay_owned = false;
            }
        }
    }
    if (!overlay_rgba && overlay_is_seq && overlay_total_frames > 0) {
        /* Fallback: PNG decode until overlay palcache is live. */
        char frame_path[PD_CONTENT_MAX_PATH];
        snprintf(frame_path, sizeof(frame_path), "%s/", overlay_base_path);
        size_t base_len = strlen(frame_path);
        snprintf(frame_path + base_len, sizeof(frame_path) - base_len,
                 overlay_frame_pattern, overlay_frame);
        unsigned ow, oh;
        overlay_rgba = decode_png_rgba(frame_path, &ow, &oh);
        if (overlay_rgba && ((int)ow != width || (int)oh != height)) {
            free(overlay_rgba);
            overlay_rgba = NULL;
        } else if (overlay_rgba) {
            overlay_owned = true;
        }
    } else if (!overlay_rgba && cached_overlay_rgba) {
        overlay_rgba = cached_overlay_rgba;
    }

    if (overlay_rgba) {
        for (size_t i = 0; i < pixels; i++) {
            uint8_t oa = overlay_rgba[i * 4 + 3];
            if (oa == 0) continue;
            if (oa == 255) {
                rgb[i * 3 + 0] = overlay_rgba[i * 4 + 0];
                rgb[i * 3 + 1] = overlay_rgba[i * 4 + 1];
                rgb[i * 3 + 2] = overlay_rgba[i * 4 + 2];
                continue;
            }
            uint8_t or_ = overlay_rgba[i * 4 + 0];
            uint8_t og = overlay_rgba[i * 4 + 1];
            uint8_t ob = overlay_rgba[i * 4 + 2];
            uint8_t br = rgb[i * 3 + 0];
            uint8_t bg = rgb[i * 3 + 1];
            uint8_t bb = rgb[i * 3 + 2];
            uint8_t inv = 255 - oa;
            rgb[i * 3 + 0] = (uint8_t)((or_ * oa + br * inv) >> 8);
            rgb[i * 3 + 1] = (uint8_t)((og * oa + bg * inv) >> 8);
            rgb[i * 3 + 2] = (uint8_t)((ob * oa + bb * inv) >> 8);
        }
        if (overlay_owned) {
            free(overlay_rgba);
        }
    }

    return rgb;
#endif /* PD_CONTENT_COMPOSITING_ENABLED */
}

static uint8_t *decode_png_file(const char *path, unsigned *w, unsigned *h)
{
    uint8_t *rgba = decode_png_rgba(path, w, h);
    if (!rgba) return NULL;

    uint8_t *rgb = composite_frame(rgba, (int)*w, (int)*h);
    free(rgba);
    return rgb;
}

/* ---- 2-slot PSRAM cache for static stills (A↔B swaps) ----
 * Sequences keep their own palette/prefetch paths; this only helps single PNGs. */
#define STATIC_RGB_CACHE_SLOTS 2

typedef struct {
    char path[PD_CONTENT_MAX_PATH];
    unsigned w;
    unsigned h;
    uint8_t *rgb;
    uint32_t stamp;
} static_rgb_slot_t;

static static_rgb_slot_t s_static_rgb_cache[STATIC_RGB_CACHE_SLOTS];
static uint32_t s_static_rgb_stamp = 1;

static uint8_t *static_rgb_dup(const uint8_t *rgb, unsigned w, unsigned h)
{
    if (!rgb || w == 0 || h == 0) return NULL;
    size_t n = (size_t)w * (size_t)h * 3u;
    uint8_t *out = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(n);
    if (out) memcpy(out, rgb, n);
    return out;
}

static void static_rgb_cache_invalidate(const char *rel_path)
{
    for (int i = 0; i < STATIC_RGB_CACHE_SLOTS; i++) {
        static_rgb_slot_t *s = &s_static_rgb_cache[i];
        if (!s->rgb) continue;
        if (!rel_path || strcmp(s->path, rel_path) == 0) {
            free(s->rgb);
            memset(s, 0, sizeof(*s));
        }
    }
}

static void static_rgb_cache_put(const char *rel_path, const uint8_t *rgb,
                                 unsigned w, unsigned h)
{
    if (!rel_path || !rel_path[0] || !rgb || w == 0 || h == 0) return;

    int slot = -1;
    for (int i = 0; i < STATIC_RGB_CACHE_SLOTS; i++) {
        if (s_static_rgb_cache[i].rgb &&
            strcmp(s_static_rgb_cache[i].path, rel_path) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_static_rgb_cache[i].rgb) slot = i;
    }
    if (slot < 0) {
        /* Evict least-recently used. */
        slot = 0;
        for (int i = 1; i < STATIC_RGB_CACHE_SLOTS; i++) {
            if (s_static_rgb_cache[i].stamp < s_static_rgb_cache[slot].stamp) {
                slot = i;
            }
        }
    }

    static_rgb_slot_t *s = &s_static_rgb_cache[slot];
    free(s->rgb);
    s->rgb = static_rgb_dup(rgb, w, h);
    if (!s->rgb) {
        memset(s, 0, sizeof(*s));
        return;
    }
    strlcpy(s->path, rel_path, sizeof(s->path));
    s->w = w;
    s->h = h;
    s->stamp = ++s_static_rgb_stamp;
}

/* Caller owns the returned buffer (free after present). */
static uint8_t *static_rgb_cache_get(const char *rel_path, unsigned *w, unsigned *h)
{
    if (!rel_path || !rel_path[0]) return NULL;
    for (int i = 0; i < STATIC_RGB_CACHE_SLOTS; i++) {
        static_rgb_slot_t *s = &s_static_rgb_cache[i];
        if (s->rgb && strcmp(s->path, rel_path) == 0) {
            s->stamp = ++s_static_rgb_stamp;
            *w = s->w;
            *h = s->h;
            return static_rgb_dup(s->rgb, s->w, s->h);
        }
    }
    return NULL;
}

/* Decode a static PNG, preferring the still cache. */
static uint8_t *decode_static_png_cached(const char *rel_path, const char *full_path,
                                         unsigned *w, unsigned *h)
{
    uint8_t *rgb = static_rgb_cache_get(rel_path, w, h);
    if (rgb) {
        return rgb;
    }
    rgb = decode_png_file(full_path, w, h);
    if (rgb) {
        static_rgb_cache_put(rel_path, rgb, *w, *h);
    }
    return rgb;
}

/* ---- play supersede: drop stale present/fade when a newer play is queued ---- */
static volatile uint32_t s_play_gen = 0;
static uint32_t s_play_active_gen = 0;

static bool content_play_superseded(void)
{
    return s_play_active_gen != 0 && s_play_active_gen != s_play_gen;
}

static void content_abort_active_transition(void)
{
    if (content_transition && content_transition->active) {
        content_transition->active = false;
    }
}

/* ---- background decode pipeline (see content_prefetch_fb comment above) ---- */

static void content_decode_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(content_decode_request_sem, portMAX_DELAY);

        uint32_t epoch = content_epoch;
        int frame = content_decode_req_frame;
        char frame_path[PD_CONTENT_MAX_PATH];
        snprintf(frame_path, sizeof(frame_path), "%s/", content_decode_req_base);
        size_t base_len = strlen(frame_path);
        snprintf(frame_path + base_len, sizeof(frame_path) - base_len, content_decode_req_pattern, frame);

        unsigned w, h;
        uint8_t *rgb = decode_png_file(frame_path, &w, &h);

        /* discard if playback moved on (new play()/stop() call) while we
         * were decoding — publishing now would corrupt the new sequence's
         * display with a stale frame from the old one. */
        if (rgb && epoch == content_epoch && content_prefetch_fb) {
            content_prefetch_store_src(rgb, (int)w, (int)h);
            pd_framebuf_blit_rgb(content_prefetch_fb, rgb, (int)w, (int)h);
            content_prefetch_frame = frame;
            content_prefetch_valid = true;
        }
        free(rgb);
        content_decode_busy = false;
    }
}

/* Ask the background task to decode `frame` of the currently-playing
 * sequence ahead of time. No-op if the task is still busy with a previous
 * request — the caller will simply retry on a later tick once it frees up,
 * so playback always falls back to a synchronous decode rather than
 * blocking on the request slot. */
static void content_request_prefetch(int frame)
{
    if (!content_decode_task || content_decode_busy || !content_is_seq) return;

    content_decode_busy = true;
    strlcpy(content_decode_req_base, content_current, sizeof(content_decode_req_base));
    strlcpy(content_decode_req_pattern, content_frame_pattern, sizeof(content_decode_req_pattern));
    content_decode_req_frame = frame;
    xSemaphoreGive(content_decode_request_sem);
}

/* Invalidate any in-flight/queued prefetch — call whenever playback target
 * changes (new play(), stop(), transition) so a decode result for the old
 * content can never land in the new content's frame sequence. */
static void content_invalidate_prefetch(void)
{
    content_epoch++;
    content_prefetch_valid = false;
    content_prefetch_frame = -1;
    content_prefetch_src_w = 0;
    content_prefetch_src_h = 0;
}

/* ---- per-sequence palette cache helpers ---- */

static void content_pal_slot_clear(pd_seq_cache_slot_t *slot)
{
    if (!slot) return;
    if (slot->indices) {
        heap_caps_free(slot->indices);
        slot->indices = NULL;
    }
    memset(slot, 0, sizeof(*slot));
}

static void content_seq_cache_deactivate(void)
{
    content_seq_cache_active = NULL;
    content_cache_building = false;
    content_cache_fallback = false;
    s_pal_back_ready = false;
    s_pal_ready_frame = -1;
    s_pal_expand_req_frame = -1;
    /* Ping-pong fronts may be freed/reused — drop dangling present ptr. */
    content_last_src_rgb = NULL;
    content_last_src_w = 0;
    content_last_src_h = 0;
    content_fb_in_sync = false;
}

static uint8_t content_nearest_palette_index(const uint8_t palette[][4], int palette_size,
                                            uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int best = 0;
    int best_d = INT_MAX;
    for (int i = 0; i < palette_size; i++) {
        int dr = (int)r - (int)palette[i][0];
        int dg = (int)g - (int)palette[i][1];
        int db = (int)b - (int)palette[i][2];
        int da = (int)a - (int)palette[i][3];
        int d = dr * dr + dg * dg + db * db + da * da;
        if (d < best_d) {
            best_d = d;
            best = i;
            if (d == 0) break;
        }
    }
    return (uint8_t)best;
}

static pd_seq_cache_slot_t *content_pal_find_slot(const char *seq_path, int sw, int sh,
                                                  int frames, int start, uint32_t phash)
{
    for (int i = 0; i < PD_PAL_CACHE_SLOTS; i++) {
        pd_seq_cache_slot_t *s = &content_pal_slots[i];
        if (!s->in_use || !s->live || !s->indices) continue;
        if (s->width == sw && s->height == sh &&
            s->frame_count == frames && s->frame_start == start &&
            s->pattern_hash == phash &&
            strcmp(s->seq_path, seq_path) == 0) {
            return s;
        }
    }
    return NULL;
}

static pd_seq_cache_slot_t *content_pal_acquire_slot(void)
{
    /* Prefer empty slot. */
    for (int i = 0; i < PD_PAL_CACHE_SLOTS; i++) {
        if (!content_pal_slots[i].in_use) {
            content_pal_slot_clear(&content_pal_slots[i]);
            content_pal_slots[i].in_use = true;
            return &content_pal_slots[i];
        }
    }
    /* Evict least-recently used that is not content or overlay active. */
    pd_seq_cache_slot_t *victim = NULL;
    for (int i = 0; i < PD_PAL_CACHE_SLOTS; i++) {
        pd_seq_cache_slot_t *s = &content_pal_slots[i];
        if (s == content_seq_cache_active) continue;
        if (s == content_overlay_cache_active) continue;
        if (!victim || s->last_used < victim->last_used) victim = s;
    }
    if (!victim) victim = &content_pal_slots[0];
    content_pal_slot_clear(victim);
    victim->in_use = true;
    return victim;
}

static void content_pal_activate(pd_seq_cache_slot_t *slot)
{
    if (!slot) return;
    slot->last_used = content_pal_lru_clock++;
    content_seq_cache_active = slot;
    content_cache_building = false;
    content_cache_fallback = false;
    content_prefetch_valid = false;
    content_prefetch_frame = -1;
    s_pal_back_ready = false;
    s_pal_ready_frame = -1;
    s_pal_expand_req_frame = -1;
}

static void content_overlay_pal_activate(pd_seq_cache_slot_t *slot)
{
    if (!slot) return;
    slot->last_used = content_pal_lru_clock++;
    content_overlay_cache_active = slot;
    content_overlay_cache_building = false;
}

/* Simple median-cut over a sampled RGBA color list → up to PD_PALETTE_MAX. */
typedef struct {
    uint8_t r, g, b, a;
} pd_rgba_sample_t;

static int pd_sample_channel_range(const pd_rgba_sample_t *s, int n, int ch,
                                   uint8_t *out_min, uint8_t *out_max)
{
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < n; i++) {
        uint8_t v = (ch == 0) ? s[i].r : (ch == 1) ? s[i].g : (ch == 2) ? s[i].b : s[i].a;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *out_min = lo;
    *out_max = hi;
    return (int)hi - (int)lo;
}

static int pd_sample_cmp_channel;
static int pd_sample_cmp(const void *a, const void *b)
{
    const pd_rgba_sample_t *sa = a;
    const pd_rgba_sample_t *sb = b;
    uint8_t va = (pd_sample_cmp_channel == 0) ? sa->r :
                 (pd_sample_cmp_channel == 1) ? sa->g :
                 (pd_sample_cmp_channel == 2) ? sa->b : sa->a;
    uint8_t vb = (pd_sample_cmp_channel == 0) ? sb->r :
                 (pd_sample_cmp_channel == 1) ? sb->g :
                 (pd_sample_cmp_channel == 2) ? sb->b : sb->a;
    return (int)va - (int)vb;
}

typedef struct {
    pd_rgba_sample_t *samples;
    int count;
} pd_mc_box_t;

static void pd_median_cut_palette(pd_rgba_sample_t *samples, int sample_count,
                                  uint8_t palette[][4], int *palette_size)
{
    if (sample_count <= 0) {
        *palette_size = 1;
        palette[0][0] = palette[0][1] = palette[0][2] = 0;
        palette[0][3] = 255;
        return;
    }
    if (sample_count <= PD_PALETTE_MAX) {
        /* Deduplicate by exact RGBA match while copying. */
        int n = 0;
        for (int i = 0; i < sample_count && n < PD_PALETTE_MAX; i++) {
            bool found = false;
            for (int j = 0; j < n; j++) {
                if (palette[j][0] == samples[i].r && palette[j][1] == samples[i].g &&
                    palette[j][2] == samples[i].b && palette[j][3] == samples[i].a) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                palette[n][0] = samples[i].r;
                palette[n][1] = samples[i].g;
                palette[n][2] = samples[i].b;
                palette[n][3] = samples[i].a;
                n++;
            }
        }
        *palette_size = n > 0 ? n : 1;
        return;
    }

    pd_mc_box_t boxes[PD_PALETTE_MAX];
    boxes[0].samples = samples;
    boxes[0].count = sample_count;
    int box_count = 1;

    while (box_count < PD_PALETTE_MAX) {
        int best = -1;
        int best_range = -1;
        int best_ch = 0;
        for (int i = 0; i < box_count; i++) {
            if (boxes[i].count < 2) continue;
            for (int ch = 0; ch < 4; ch++) {
                uint8_t lo, hi;
                int range = pd_sample_channel_range(boxes[i].samples, boxes[i].count, ch, &lo, &hi);
                if (range > best_range) {
                    best_range = range;
                    best = i;
                    best_ch = ch;
                }
            }
        }
        if (best < 0 || best_range <= 0) break;

        pd_sample_cmp_channel = best_ch;
        qsort(boxes[best].samples, (size_t)boxes[best].count, sizeof(pd_rgba_sample_t), pd_sample_cmp);
        int mid = boxes[best].count / 2;
        if (mid <= 0) mid = 1;
        if (mid >= boxes[best].count) mid = boxes[best].count - 1;

        boxes[box_count].samples = boxes[best].samples + mid;
        boxes[box_count].count = boxes[best].count - mid;
        boxes[best].count = mid;
        box_count++;
    }

    for (int i = 0; i < box_count; i++) {
        uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
        for (int j = 0; j < boxes[i].count; j++) {
            sr += boxes[i].samples[j].r;
            sg += boxes[i].samples[j].g;
            sb += boxes[i].samples[j].b;
            sa += boxes[i].samples[j].a;
        }
        int n = boxes[i].count > 0 ? boxes[i].count : 1;
        palette[i][0] = (uint8_t)(sr / (uint32_t)n);
        palette[i][1] = (uint8_t)(sg / (uint32_t)n);
        palette[i][2] = (uint8_t)(sb / (uint32_t)n);
        palette[i][3] = (uint8_t)(sa / (uint32_t)n);
    }
    *palette_size = box_count;
}

/* Load one sequence frame as source-sized RGBA (no compositing, no letterbox). */
static uint8_t *content_load_seq_frame_rgba(const char *base_path, const char *pattern,
                                            int frame_num, int *out_w, int *out_h)
{
    if (!base_path || !pattern) return NULL;
    char frame_path[PD_CONTENT_MAX_PATH];
    snprintf(frame_path, sizeof(frame_path), "%s/", base_path);
    size_t base_len = strlen(frame_path);
    snprintf(frame_path + base_len, sizeof(frame_path) - base_len, pattern, frame_num);

    unsigned w = 0, h = 0;
    pd_decoded_frame_t decoded;
    if (!decode_png_indexed_or_rgba(frame_path, &w, &h, &decoded)) {
        return NULL;
    }

    uint8_t *src_rgba = NULL;
    if (decoded.is_indexed && decoded.indices) {
        src_rgba = malloc((size_t)w * h * 4);
        if (src_rgba) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                uint8_t pi = decoded.indices[i];
                if (pi >= (uint8_t)decoded.palette_size) pi = 0;
                src_rgba[i * 4 + 0] = decoded.palette[pi][0];
                src_rgba[i * 4 + 1] = decoded.palette[pi][1];
                src_rgba[i * 4 + 2] = decoded.palette[pi][2];
                src_rgba[i * 4 + 3] = decoded.palette[pi][3];
            }
        }
    } else {
        src_rgba = decoded.rgba;
        decoded.rgba = NULL;
    }
    free_decoded_frame(&decoded);
    if (!src_rgba) return NULL;
    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    return src_rgba;
}

static uint8_t *content_load_frame_rgba_source(int frame_num, int *out_w, int *out_h)
{
    return content_load_seq_frame_rgba(content_current, content_frame_pattern,
                                       frame_num, out_w, out_h);
}

static bool content_probe_source_size(int *sw, int *sh)
{
    if (*sw > 0 && *sh > 0) return true;
    int w = 0, h = 0;
    uint8_t *rgba = content_load_frame_rgba_source(content_frame_start, &w, &h);
    if (!rgba) return false;
    free(rgba);
    *sw = w;
    *sh = h;
    return w > 0 && h > 0;
}

static bool content_pal_ensure_rgb_bufs(int sw, int sh)
{
    if (sw <= 0 || sh <= 0) return false;
    size_t need = (size_t)sw * (size_t)sh * 3;
    if (s_pal_rgb[0] && s_pal_rgb[1] && s_pal_rgb_cap >= need &&
        s_pal_rgb_w == sw && s_pal_rgb_h == sh) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        if (s_pal_rgb[i]) {
            heap_caps_free(s_pal_rgb[i]);
            s_pal_rgb[i] = NULL;
        }
        s_pal_rgb[i] = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pal_rgb[i]) {
            for (int j = 0; j <= i; j++) {
                heap_caps_free(s_pal_rgb[j]);
                s_pal_rgb[j] = NULL;
            }
            s_pal_rgb_cap = 0;
            s_pal_rgb_w = s_pal_rgb_h = 0;
            return false;
        }
    }
    s_pal_rgb_cap = need;
    s_pal_rgb_w = sw;
    s_pal_rgb_h = sh;
    s_pal_front = 0;
    s_pal_back_ready = false;
    s_pal_ready_frame = -1;
    return true;
}

static uint8_t *content_pal_ensure_scratch(uint8_t **buf, size_t *cap, size_t pixels)
{
    size_t need = pixels * 4;
    if (*buf && *cap >= need) return *buf;
    uint8_t *n = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n) return NULL;
    heap_caps_free(*buf);
    *buf = n;
    *cap = need;
    return *buf;
}

static uint8_t *content_pal_ensure_rgba_scratch(size_t pixels)
{
    return content_pal_ensure_scratch(&s_pal_rgba_scratch, &s_pal_rgba_scratch_cap, pixels);
}

#if PD_CONTENT_COMPOSITING_ENABLED
static uint8_t *content_ov_ensure_rgba_scratch(size_t pixels)
{
    return content_pal_ensure_scratch(&s_ov_rgba_scratch, &s_ov_rgba_scratch_cap, pixels);
}
#endif

/* Expand slot indices for `frame_number` into dst_rgb (source-sized RGB).
 * Skips composite_frame when bg/overlay are trivial. */
static bool content_pal_expand_frame_to_rgb(pd_seq_cache_slot_t *slot, int frame_number,
                                           uint8_t *dst_rgb)
{
    if (!slot || !slot->live || !slot->indices || !dst_rgb) return false;
    int idx = frame_number - slot->frame_start;
    if (idx < 0 || idx >= slot->frame_count) return false;

    int sw = slot->width;
    int sh = slot->height;
    size_t pixels = (size_t)sw * (size_t)sh;
    const uint8_t *src = slot->indices + (size_t)idx * pixels;

    if (content_compositing_is_trivial()) {
        for (size_t i = 0; i < pixels; i++) {
            uint8_t pi = src[i];
            if (pi >= (uint8_t)slot->palette_size) pi = 0;
            uint8_t a = slot->palette[pi][3];
            if (a == 0) {
                dst_rgb[i * 3 + 0] = 0;
                dst_rgb[i * 3 + 1] = 0;
                dst_rgb[i * 3 + 2] = 0;
            } else {
                dst_rgb[i * 3 + 0] = slot->palette[pi][0];
                dst_rgb[i * 3 + 1] = slot->palette[pi][1];
                dst_rgb[i * 3 + 2] = slot->palette[pi][2];
            }
        }
        return true;
    }

    uint8_t *rgba = content_pal_ensure_rgba_scratch(pixels);
    if (!rgba) return false;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t pi = src[i];
        if (pi >= (uint8_t)slot->palette_size) pi = 0;
        rgba[i * 4 + 0] = slot->palette[pi][0];
        rgba[i * 4 + 1] = slot->palette[pi][1];
        rgba[i * 4 + 2] = slot->palette[pi][2];
        rgba[i * 4 + 3] = slot->palette[pi][3];
    }

    uint8_t *rgb = composite_frame(rgba, sw, sh);
    if (!rgb) return false;
    memcpy(dst_rgb, rgb, pixels * 3);
    free(rgb);
    return true;
}

/* Expand one cached indexed frame and present (sync path / first frame). */
static void content_seq_cache_expand_to_fb(int frame_number)
{
    pd_seq_cache_slot_t *slot = content_seq_cache_active;
    if (!slot || !slot->live || !slot->indices) return;
    if (!content_pal_ensure_rgb_bufs(slot->width, slot->height)) {
        /* Fallback: one-shot heap path. */
        int idx = frame_number - slot->frame_start;
        if (idx < 0 || idx >= slot->frame_count) return;
        size_t pixels = (size_t)slot->width * (size_t)slot->height;
        const uint8_t *src = slot->indices + (size_t)idx * pixels;
        uint8_t *rgba = malloc(pixels * 4);
        if (!rgba) return;
        for (size_t i = 0; i < pixels; i++) {
            uint8_t pi = src[i];
            if (pi >= (uint8_t)slot->palette_size) pi = 0;
            rgba[i * 4 + 0] = slot->palette[pi][0];
            rgba[i * 4 + 1] = slot->palette[pi][1];
            rgba[i * 4 + 2] = slot->palette[pi][2];
            rgba[i * 4 + 3] = slot->palette[pi][3];
        }
        uint8_t *rgb = composite_frame(rgba, slot->width, slot->height);
        free(rgba);
        if (!rgb) return;
        content_present_source_rgb(rgb, slot->width, slot->height);
        free(rgb);
        return;
    }
    uint8_t *front = s_pal_rgb[s_pal_front];
    if (!content_pal_expand_frame_to_rgb(slot, frame_number, front)) return;
    content_present_source_rgb(front, slot->width, slot->height);
}

static void content_palcache_sidecar_path_for(const char *seq_dir, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", seq_dir, PD_PAL_CACHE_FILENAME);
}

static bool content_palcache_write_sidecar(const pd_seq_cache_slot_t *slot)
{
    if (!slot || !slot->indices || !slot->live) return false;
    char path[PD_CONTENT_MAX_PATH];
    content_palcache_sidecar_path_for(slot->seq_path, path, sizeof(path));

    pd_palcache_hdr_t hdr = {0};
    hdr.magic = PD_PAL_CACHE_MAGIC;
    hdr.version = PD_PAL_CACHE_VERSION;
    hdr.width = (uint16_t)slot->width;
    hdr.height = (uint16_t)slot->height;
    hdr.frame_count = (uint16_t)slot->frame_count;
    hdr.frame_start = (int16_t)slot->frame_start;
    hdr.palette_size = (uint16_t)slot->palette_size;
    hdr.pattern_hash = slot->pattern_hash;
    memcpy(hdr.palette, slot->palette, sizeof(hdr.palette));

    size_t slab = (size_t)slot->width * (size_t)slot->height * (size_t)slot->frame_count;
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "palette cache: cannot write sidecar %s", path);
        return false;
    }
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(slot->indices, 1, slab, f) == slab;
    fclose(f);
    if (!ok) {
        unlink(path);
        ESP_LOGW(TAG, "palette cache: sidecar write failed (%s)", path);
        return false;
    }
    ESP_LOGI(TAG, "palette cache: wrote sidecar %s (%u KB)", path, (unsigned)(slab / 1024));
    return true;
}

static pd_seq_cache_slot_t *content_palcache_try_load_sidecar_at(
    const char *seq_path, int sw, int sh, int frames, int start, uint32_t phash)
{
    if (!seq_path || sw <= 0 || sh <= 0 || frames <= 0) return NULL;
    char path[PD_CONTENT_MAX_PATH];
    content_palcache_sidecar_path_for(seq_path, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    pd_palcache_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    if (hdr.magic != PD_PAL_CACHE_MAGIC || hdr.version != PD_PAL_CACHE_VERSION ||
        hdr.width != (uint16_t)sw || hdr.height != (uint16_t)sh ||
        hdr.frame_count != (uint16_t)frames ||
        hdr.frame_start != (int16_t)start ||
        hdr.pattern_hash != phash ||
        hdr.palette_size == 0 || hdr.palette_size > PD_PALETTE_MAX) {
        fclose(f);
        ESP_LOGI(TAG, "palette cache: sidecar stale/invalid — will rebuild");
        unlink(path);
        return NULL;
    }

    size_t slab = (size_t)hdr.width * (size_t)hdr.height * (size_t)hdr.frame_count;
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (slab + 65536 > free_spiram) {
        fclose(f);
        ESP_LOGW(TAG, "palette cache: sidecar needs %u SPIRAM, only %u free",
                 (unsigned)slab, (unsigned)free_spiram);
        return NULL;
    }
    uint8_t *indices = heap_caps_malloc(slab, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!indices) {
        fclose(f);
        ESP_LOGW(TAG, "palette cache: sidecar SPIRAM alloc failed");
        return NULL;
    }
    if (fread(indices, 1, slab, f) != slab) {
        fclose(f);
        heap_caps_free(indices);
        unlink(path);
        return NULL;
    }
    fclose(f);

    pd_seq_cache_slot_t *slot = content_pal_acquire_slot();
    if (!slot) {
        heap_caps_free(indices);
        return NULL;
    }
    strlcpy(slot->seq_path, seq_path, sizeof(slot->seq_path));
    slot->pattern_hash = hdr.pattern_hash;
    slot->width = hdr.width;
    slot->height = hdr.height;
    slot->frame_count = hdr.frame_count;
    slot->frame_start = hdr.frame_start;
    slot->palette_size = hdr.palette_size;
    memcpy(slot->palette, hdr.palette, sizeof(slot->palette));
    slot->indices = indices;
    slot->live = true;
    ESP_LOGI(TAG, "palette cache: loaded sidecar %s (%d frames %dx%d, %u KB)",
             path, slot->frame_count, slot->width, slot->height, (unsigned)(slab / 1024));
    return slot;
}

static pd_seq_cache_slot_t *content_palcache_try_load_sidecar(void)
{
    return content_palcache_try_load_sidecar_at(
        content_current, content_src_w, content_src_h,
        content_total_frames, content_frame_start, content_pattern_hash);
}

/* Build PSRAM indexed cache at SOURCE size for content or overlay sequence. */
static bool content_seq_cache_build_ex(uint32_t epoch, bool for_overlay)
{
    const char *seq_path;
    const char *pattern;
    int frame_count;
    int frame_start;
    uint32_t phash;
    int sw, sh;

    if (for_overlay) {
        content_overlay_cache_building = true;
        if (!content_config.auto_quantize_palette || !overlay_is_seq ||
            overlay_total_frames <= 0 || overlay_base_path[0] == '\0') {
            content_overlay_cache_building = false;
            return false;
        }
        seq_path = overlay_base_path;
        pattern = overlay_frame_pattern;
        frame_count = overlay_total_frames;
        frame_start = overlay_frame_start;
        phash = content_overlay_pattern_hash;
        sw = 0;
        sh = 0;
        uint8_t *probe = content_load_seq_frame_rgba(seq_path, pattern, frame_start, &sw, &sh);
        free(probe);
        if (sw <= 0 || sh <= 0) {
            content_log_line("overlay cache fallback: probe size failed");
            content_overlay_cache_building = false;
            return false;
        }
    } else {
        content_cache_building = true;
        content_cache_fallback = false;
        if (!content_config.auto_quantize_palette || !content_is_seq ||
            content_total_frames <= 0) {
            snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                     "skip aq=%d seq=%d frames=%d",
                     (int)content_config.auto_quantize_palette, (int)content_is_seq,
                     content_total_frames);
            content_log_line("palette cache fallback: %s", content_cache_fail_reason);
            content_cache_building = false;
            content_cache_fallback = true;
            return false;
        }
        seq_path = content_current;
        pattern = content_frame_pattern;
        frame_count = content_total_frames;
        frame_start = content_frame_start;
        phash = content_pattern_hash;
        sw = content_src_w;
        sh = content_src_h;
        if (!content_probe_source_size(&sw, &sh)) {
            snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                     "probe size failed");
            content_log_line("palette cache fallback: %s for %s", content_cache_fail_reason,
                             content_rel_path[0] ? content_rel_path : content_current);
            content_cache_building = false;
            content_cache_fallback = true;
            return false;
        }
        content_src_w = sw;
        content_src_h = sh;
    }

    if (epoch != content_epoch) {
        content_cache_building = false;
        content_overlay_cache_building = false;
        return false;
    }

    size_t pixels = (size_t)sw * (size_t)sh;
    size_t slab_bytes = pixels * (size_t)frame_count;
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (slab_bytes + 65536 > free_spiram) {
        for (int i = 0; i < PD_PAL_CACHE_SLOTS; i++) {
            pd_seq_cache_slot_t *s = &content_pal_slots[i];
            if (s != content_seq_cache_active && s != content_overlay_cache_active &&
                s->in_use) {
                content_pal_slot_clear(s);
            }
        }
        free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    if (slab_bytes + 65536 > free_spiram) {
        content_log_line("%s cache fallback: spiram need=%u free=%u %dx%dx%d",
                         for_overlay ? "overlay" : "palette",
                         (unsigned)slab_bytes, (unsigned)free_spiram, sw, sh, frame_count);
        if (!for_overlay) {
            snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                     "spiram need=%u free=%u %dx%dx%d",
                     (unsigned)slab_bytes, (unsigned)free_spiram, sw, sh, frame_count);
            content_cache_fallback = true;
            content_cache_building = false;
        } else {
            content_overlay_cache_building = false;
        }
        return false;
    }

    uint8_t *slab = heap_caps_malloc(slab_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!slab) {
        content_log_line("%s cache fallback: spiram alloc %u failed",
                         for_overlay ? "overlay" : "palette", (unsigned)slab_bytes);
        if (!for_overlay) {
            content_cache_fallback = true;
            content_cache_building = false;
        } else {
            content_overlay_cache_building = false;
        }
        return false;
    }

    pd_rgba_sample_t *samples = heap_caps_malloc(sizeof(pd_rgba_sample_t) * PD_QUANT_SAMPLES_MAX,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!samples) {
        heap_caps_free(slab);
        content_log_line("%s cache fallback: sample buffer alloc failed",
                         for_overlay ? "overlay" : "palette");
        if (!for_overlay) {
            content_cache_fallback = true;
            content_cache_building = false;
        } else {
            content_overlay_cache_building = false;
        }
        return false;
    }

    uint8_t palette[PD_PALETTE_MAX][4];
    int palette_size = 0;
    int sample_count = 0;
    bool used_source_palette = false;
    int64_t t0 = esp_timer_get_time();

    for (int f = 0; f < frame_count; f++) {
        if (epoch != content_epoch) {
            heap_caps_free(samples);
            heap_caps_free(slab);
            content_cache_building = false;
            content_overlay_cache_building = false;
            return false;
        }

        int frame_num = frame_start + f;

        if (f == 0 && !used_source_palette) {
            char frame_path[PD_CONTENT_MAX_PATH];
            snprintf(frame_path, sizeof(frame_path), "%s/", seq_path);
            size_t base_len = strlen(frame_path);
            snprintf(frame_path + base_len, sizeof(frame_path) - base_len,
                     pattern, frame_num);
            unsigned w = 0, h = 0;
            pd_decoded_frame_t decoded;
            if (decode_png_indexed_or_rgba(frame_path, &w, &h, &decoded)) {
                if (decoded.is_indexed && decoded.palette_size > 0 &&
                    decoded.palette_size <= PD_PALETTE_MAX) {
                    palette_size = decoded.palette_size;
                    memcpy(palette, decoded.palette, (size_t)decoded.palette_size * 4);
                    used_source_palette = true;
                }
                free_decoded_frame(&decoded);
            }
        }

        if (!used_source_palette) {
            int fw = 0, fh = 0;
            uint8_t *rgba = content_load_seq_frame_rgba(seq_path, pattern, frame_num, &fw, &fh);
            if (rgba && fw == sw && fh == sh) {
                size_t step = pixels / PD_QUANT_SAMPLES_MAX;
                if (step < 1) step = 1;
                for (size_t i = 0; i < pixels && sample_count < PD_QUANT_SAMPLES_MAX; i += step) {
                    if (rgba[i * 4 + 3] == 0) continue;
                    samples[sample_count].r = rgba[i * 4 + 0];
                    samples[sample_count].g = rgba[i * 4 + 1];
                    samples[sample_count].b = rgba[i * 4 + 2];
                    samples[sample_count].a = rgba[i * 4 + 3];
                    sample_count++;
                }
            }
            free(rgba);
        } else {
            break;
        }

        vTaskDelay(1);
    }

    if (!used_source_palette) {
        pd_median_cut_palette(samples, sample_count, palette, &palette_size);
    }
    heap_caps_free(samples);
    samples = NULL;

    if (palette_size <= 0) {
        palette_size = 1;
        palette[0][0] = palette[0][1] = palette[0][2] = palette[0][3] = 0;
    }
    int transparent_idx = -1;
    for (int i = 0; i < palette_size; i++) {
        if (palette[i][3] == 0) {
            transparent_idx = i;
            break;
        }
    }
    if (transparent_idx < 0 && palette_size < PD_PALETTE_MAX) {
        transparent_idx = palette_size++;
        palette[transparent_idx][0] = 0;
        palette[transparent_idx][1] = 0;
        palette[transparent_idx][2] = 0;
        palette[transparent_idx][3] = 0;
    }
    if (transparent_idx < 0) transparent_idx = 0;

    for (int f = 0; f < frame_count; f++) {
        if (epoch != content_epoch) {
            heap_caps_free(slab);
            content_cache_building = false;
            content_overlay_cache_building = false;
            return false;
        }

        int frame_num = frame_start + f;
        int fw = 0, fh = 0;
        uint8_t *rgba = content_load_seq_frame_rgba(seq_path, pattern, frame_num, &fw, &fh);
        uint8_t *dst = slab + (size_t)f * pixels;
        if (!rgba || fw != sw || fh != sh) {
            memset(dst, (uint8_t)transparent_idx, pixels);
            free(rgba);
            vTaskDelay(1);
            continue;
        }
        for (size_t i = 0; i < pixels; i++) {
            uint8_t a = rgba[i * 4 + 3];
            if (a == 0) {
                dst[i] = (uint8_t)transparent_idx;
                continue;
            }
            dst[i] = content_nearest_palette_index(palette, palette_size,
                                                   rgba[i * 4 + 0], rgba[i * 4 + 1],
                                                   rgba[i * 4 + 2], a);
        }
        free(rgba);
        vTaskDelay(1);
    }

    if (epoch != content_epoch) {
        heap_caps_free(slab);
        content_cache_building = false;
        content_overlay_cache_building = false;
        return false;
    }

    pd_seq_cache_slot_t *slot = content_pal_acquire_slot();
    if (!slot) {
        heap_caps_free(slab);
        content_log_line("%s cache fallback: no LRU slot", for_overlay ? "overlay" : "palette");
        if (!for_overlay) {
            content_cache_fallback = true;
            content_cache_building = false;
        } else {
            content_overlay_cache_building = false;
        }
        return false;
    }
    strlcpy(slot->seq_path, seq_path, sizeof(slot->seq_path));
    slot->pattern_hash = phash;
    slot->width = sw;
    slot->height = sh;
    slot->frame_count = frame_count;
    slot->frame_start = frame_start;
    slot->palette_size = palette_size;
    memcpy(slot->palette, palette, sizeof(palette));
    slot->indices = slab;
    slot->live = true;

    if (epoch == content_epoch) {
        if (for_overlay) {
            content_overlay_pal_activate(slot);
        } else {
            content_pal_activate(slot);
        }
        (void)content_palcache_write_sidecar(slot);
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (!for_overlay) content_cache_fail_reason[0] = '\0';
    content_log_line("%s cache live: %d frames %dx%d %d colors %uKB %lldms",
                     for_overlay ? "overlay" : "palette",
                     frame_count, sw, sh, palette_size,
                     (unsigned)(slab_bytes / 1024), (long long)ms);
    content_cache_building = false;
    content_overlay_cache_building = false;
    return true;
}

static bool content_seq_cache_build(uint32_t epoch)
{
    return content_seq_cache_build_ex(epoch, false);
}

static void content_cache_notify(pd_palcache_job_t job)
{
    if (!content_cache_task) return;
    content_cache_job = job;
    content_cache_req_epoch = content_epoch;
    xTaskNotifyGive(content_cache_task);
}

static void content_request_pal_expand(int frame)
{
    if (!content_cache_task || !content_seq_cache_active || !content_seq_cache_active->live) {
        return;
    }
    if (frame < 0) return;
    /* Don't steal the worker while a full content/overlay rebuild is running. */
    if (content_cache_building || content_overlay_cache_building) return;
    if (s_pal_expand_req_frame == frame && !s_pal_back_ready) return;
    s_pal_expand_req_frame = frame;
    content_cache_notify(PD_PALCACHE_JOB_EXPAND);
}

static void content_cache_run_expand(uint32_t epoch)
{
    int frame = s_pal_expand_req_frame;
    pd_seq_cache_slot_t *slot = content_seq_cache_active;
    if (epoch != content_epoch || !slot || !slot->live || frame < 0) return;
    if (!content_pal_ensure_rgb_bufs(slot->width, slot->height)) return;
    int back = 1 - s_pal_front;
    if (!content_pal_expand_frame_to_rgb(slot, frame, s_pal_rgb[back])) return;
    if (epoch != content_epoch) return;
    s_pal_ready_frame = frame;
    s_pal_back_ready = true;
}

static void content_cache_run_overlay(uint32_t epoch)
{
    if (!content_config.auto_quantize_palette || !overlay_is_seq ||
        overlay_total_frames <= 0 || overlay_base_path[0] == '\0') {
        content_overlay_cache_building = false;
        return;
    }
    if (epoch != content_epoch) {
        content_overlay_cache_building = false;
        return;
    }

    int sw = 0, sh = 0;
    uint8_t *probe = content_load_seq_frame_rgba(
        overlay_base_path, overlay_frame_pattern, overlay_frame_start, &sw, &sh);
    free(probe);
    if (sw <= 0 || sh <= 0) {
        content_overlay_cache_building = false;
        return;
    }
    pd_seq_cache_slot_t *warm = content_pal_find_slot(
        overlay_base_path, sw, sh,
        overlay_total_frames, overlay_frame_start,
        content_overlay_pattern_hash);
    if (warm) {
        content_overlay_pal_activate(warm);
        content_log_line("overlay cache RAM hit (%d frames)", overlay_total_frames);
        return;
    }
    warm = content_palcache_try_load_sidecar_at(
        overlay_base_path, sw, sh, overlay_total_frames, overlay_frame_start,
        content_overlay_pattern_hash);
    if (warm && epoch == content_epoch) {
        content_overlay_pal_activate(warm);
        content_log_line("overlay cache sidecar hit (%d frames)", overlay_total_frames);
        return;
    }

    content_log_line("overlay cache build start %d frames %dx%d spiram_free=%u",
                     overlay_total_frames, sw, sh,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    (void)content_seq_cache_build_ex(epoch, true);
}

static void content_cache_run_content(uint32_t epoch)
{
    if (!content_config.auto_quantize_palette || !content_is_seq) {
        content_cache_building = false;
        return;
    }
    if (epoch != content_epoch) {
        if (!content_seq_cache_active) {
            content_cache_building = false;
        }
        return;
    }
    pd_seq_cache_slot_t *warm = content_pal_find_slot(
        content_current, content_src_w, content_src_h,
        content_total_frames, content_frame_start, content_pattern_hash);
    if (warm) {
        content_pal_activate(warm);
        content_cache_fail_reason[0] = '\0';
        content_log_line("palette cache RAM hit for %s",
                         content_rel_path[0] ? content_rel_path : content_current);
#if PD_CONTENT_COMPOSITING_ENABLED
        if (overlay_is_seq && overlay_total_frames > 0 &&
            !(content_overlay_cache_active && content_overlay_cache_active->live)) {
            content_request_overlay_cache_build();
        }
#endif
        return;
    }
    warm = content_palcache_try_load_sidecar();
    if (warm && epoch == content_epoch) {
        content_pal_activate(warm);
        content_cache_fail_reason[0] = '\0';
        content_log_line("palette cache sidecar hit for %s",
                         content_rel_path[0] ? content_rel_path : content_current);
#if PD_CONTENT_COMPOSITING_ENABLED
        if (overlay_is_seq && overlay_total_frames > 0 &&
            !(content_overlay_cache_active && content_overlay_cache_active->live)) {
            content_request_overlay_cache_build();
        }
#endif
        return;
    }
    content_log_line("palette cache build start epoch=%u %d frames %dx%d spiram_free=%u",
                     (unsigned)epoch, content_total_frames, content_src_w, content_src_h,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!content_seq_cache_build(epoch) && epoch == content_epoch) {
        if (!content_cache_fail_reason[0]) {
            snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                     "build failed");
        }
        content_log_line("palette cache fallback: %s", content_cache_fail_reason);
        content_cache_fallback = true;
        content_cache_building = false;
        return;
    }
#if PD_CONTENT_COMPOSITING_ENABLED
    if (epoch == content_epoch && overlay_is_seq && overlay_total_frames > 0 &&
        !(content_overlay_cache_active && content_overlay_cache_active->live)) {
        content_request_overlay_cache_build();
    }
#endif
}

static void content_cache_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t epoch = content_cache_req_epoch;
        pd_palcache_job_t job = content_cache_job;
        if (job == PD_PALCACHE_JOB_EXPAND) {
            content_cache_run_expand(epoch);
        } else if (job == PD_PALCACHE_JOB_OVERLAY) {
            content_cache_run_overlay(epoch);
        } else {
            content_cache_run_content(epoch);
        }
    }
}

/* Resolve palette cache for the sequence that just started playing:
 * RAM hit → sidecar → background build. */
static void content_request_palette_cache_build(void)
{
    content_seq_cache_deactivate();
    if (!content_config.auto_quantize_palette || !content_is_seq ||
        content_total_frames <= 0) {
        return;
    }

    if (!content_probe_source_size(&content_src_w, &content_src_h)) {
        snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                 "source size unknown");
        content_log_line("palette cache fallback: %s", content_cache_fail_reason);
        content_cache_fallback = true;
        return;
    }

    pd_seq_cache_slot_t *warm = content_pal_find_slot(
        content_current, content_src_w, content_src_h,
        content_total_frames, content_frame_start, content_pattern_hash);
    if (warm) {
        content_pal_activate(warm);
        ESP_LOGI(TAG, "palette cache: RAM hit for %s",
                 content_rel_path[0] ? content_rel_path : content_current);
        return;
    }

    warm = content_palcache_try_load_sidecar();
    if (warm) {
        content_pal_activate(warm);
        return;
    }

    if (content_cache_task == NULL) {
        /* Must be created in pd_content_init (before BLE). By first play,
         * internal heap is often <4 KiB and a late create always fails. */
        snprintf(content_cache_fail_reason, sizeof(content_cache_fail_reason),
                 "task missing (init failed)");
        content_log_line("palette cache fallback: %s int_free=%u",
                         content_cache_fail_reason,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        content_cache_fallback = true;
        return;
    }
    content_cache_building = true;
    content_cache_fallback = false;
    content_cache_notify(PD_PALCACHE_JOB_CONTENT);
}

#if PD_CONTENT_COMPOSITING_ENABLED
static void content_request_overlay_cache_build(void)
{
    if (!content_config.auto_quantize_palette || !overlay_is_seq ||
        overlay_total_frames <= 0 || overlay_base_path[0] == '\0') {
        return;
    }
    if (content_overlay_cache_active && content_overlay_cache_active->live &&
        strcmp(content_overlay_cache_active->seq_path, overlay_base_path) == 0 &&
        content_overlay_cache_active->frame_count == overlay_total_frames &&
        content_overlay_cache_active->frame_start == overlay_frame_start &&
        content_overlay_cache_active->pattern_hash == content_overlay_pattern_hash) {
        return;
    }
    if (content_cache_task == NULL) {
        content_log_line("overlay cache fallback: task missing");
        return;
    }
    /* Defer if content cache is still building — content_cache_run_content
     * will re-request when it finishes. */
    if (content_cache_building) return;

    content_overlay_cache_building = true;
    content_cache_notify(PD_PALCACHE_JOB_OVERLAY);
}
#endif /* PD_CONTENT_COMPOSITING_ENABLED */

/* Static TCB/stack so the worker still exists after WiFi/BLE fragment the
 * internal heap (late xTaskCreate was failing with int_free≈3 KiB). Stack
 * stays in internal DRAM — LittleFS/lodepng are not safe on a PSRAM stack. */
#define PD_PALCACHE_STACK_BYTES 16384
static StackType_t s_palcache_stack[PD_PALCACHE_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_palcache_tcb;

static bool content_palcache_ensure_task(void)
{
    if (content_cache_task != NULL) {
        eTaskState st = eTaskGetState(content_cache_task);
        if (st != eDeleted) {
            return true;
        }
        content_cache_task = NULL;
    }
    content_cache_task = xTaskCreateStaticPinnedToCore(
        content_cache_task_fn, "pd_palcache",
        PD_PALCACHE_STACK_BYTES / sizeof(StackType_t),
        NULL, tskIDLE_PRIORITY + 1,
        s_palcache_stack, &s_palcache_tcb, 1);
    if (content_cache_task == NULL) {
        ESP_LOGW(TAG, "palette cache static task create failed");
        return false;
    }
    ESP_LOGI(TAG, "palette cache task ready (static %d bytes stack)",
             PD_PALCACHE_STACK_BYTES);
    return true;
}

/* ---- public API ---- */

static bool content_play_ensure_worker(void);

esp_err_t pd_content_init(const char *base_path)
{
    snprintf(content_base, sizeof(content_base), "%s/content", base_path);
    ensure_dir(content_base);

    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/images", content_base);
    ensure_dir(path);

    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    if (dw > 0 && dh > 0) {
        content_transition = pd_transition_create(dw, dh);
        content_fb = pd_framebuf_create(dw, dh);
        content_prefetch_fb = pd_framebuf_create(dw, dh);
        if (!content_transition || !content_fb || !content_prefetch_fb) {
            ESP_LOGW(TAG, "failed to allocate transition engine");
        }
    }

    content_decode_request_sem = xSemaphoreCreateBinary();
    if (content_decode_request_sem && content_prefetch_fb) {
        /* pinned to core 1 (main task/network/wizard/render all run on core
         * 0, see CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0) so the decode actually
         * overlaps with rendering instead of just interleaving on the same
         * core. Same stack size as the main task needed for this call chain
         * (see CONFIG_ESP_MAIN_TASK_STACK_SIZE) — decode_png_file() ->
         * LittleFS/lodepng is stack-hungry. */
        BaseType_t ok = xTaskCreatePinnedToCore(content_decode_task_fn, "pd_decode", 8192, NULL,
                                                 tskIDLE_PRIORITY + 1, &content_decode_task, 1);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "failed to create background decode task — falling back to synchronous decode");
            content_decode_task = NULL;
        }
    } else {
        ESP_LOGW(TAG, "failed to allocate decode pipeline resources — falling back to synchronous decode");
    }

    load_config();

    /* Create the async play worker before WiFi/BLE eat internal RAM.
     * BLE play used to fall back to sync decode on the NimBLE task (WDT). */
    if (!content_play_ensure_worker()) {
        ESP_LOGW(TAG, "play worker unavailable at init — first play may fail under BLE");
    }

    /* Same for palette-cache worker: must exist before pd_ble_start(). */
    if (!content_palcache_ensure_task()) {
        ESP_LOGW(TAG, "palette cache worker unavailable — sequences stay on truecolor path");
    }

#if PD_CONTENT_COMPOSITING_ENABLED
    ESP_LOGI(TAG, "content initialized at %s (compositing on)", content_base);
#else
    ESP_LOGI(TAG, "content initialized at %s (compositing archived)", content_base);
#endif
    return ESP_OK;
}

int pd_content_list_images(pd_content_entry_t *entries, int max_entries)
{
    char img_dir[PD_CONTENT_MAX_PATH];
    snprintf(img_dir, sizeof(img_dir), "%s/images", content_base);

    DIR *d = opendir(img_dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", img_dir);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        if (ent->d_name[0] == '.') continue;

        char full[PD_CONTENT_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", img_dir, ent->d_name);

        pd_content_entry_t *e = &entries[count];
        snprintf(e->path, sizeof(e->path), "images/%s", ent->d_name);
        strlcpy(e->name, ent->d_name, sizeof(e->name));

        if (path_is_dir(full)) {
            e->is_sequence = true;
            e->fps = 12;
            e->frame_count = 0;
            if (pd_sprite_scene_detect(full)) {
                e->frame_count = pd_sprite_scene_count_slots(full);
                e->fps = 24;
                load_sequence_meta(full, &e->fps, NULL, NULL, NULL, 0, NULL, NULL, 0, NULL, 0,
                                   NULL, NULL);
            } else {
                load_sequence_meta(full, &e->fps, NULL, &e->frame_count, NULL, 0, NULL, NULL, 0, NULL, 0,
                                   NULL, NULL);
            }
        } else if (is_png(ent->d_name)) {
            e->is_sequence = false;
            e->fps = 0;
            e->frame_count = 1;
        } else {
            continue;
        }
        count++;
    }
    closedir(d);
    return count;
}

/* helper: decode first frame of content into a framebuffer */
static bool content_decode_first_frame(const char *full_path, pd_framebuf_t *fb)
{
    if (path_is_dir(full_path)) {
        char pattern[64] = "%04d.png";
        int start = 1;
        load_sequence_meta(full_path, NULL, NULL, NULL, pattern, sizeof(pattern), &start, NULL, 0, NULL, 0,
                           NULL, NULL);

        char frame_path[PD_CONTENT_MAX_PATH];
        snprintf(frame_path, sizeof(frame_path), "%s/", full_path);
        size_t base_len = strlen(frame_path);
        snprintf(frame_path + base_len, sizeof(frame_path) - base_len, pattern, start);

        unsigned w, h;
        uint8_t *rgb = decode_png_file(frame_path, &w, &h);
        if (!rgb) return false;
        pd_framebuf_blit_rgb(fb, rgb, (int)w, (int)h);
        free(rgb);
        return true;
    } else if (path_exists(full_path) && is_png(full_path)) {
        unsigned w, h;
        const char *rel = content_rel_path[0] ? content_rel_path : NULL;
        uint8_t *rgb = rel
            ? decode_static_png_cached(rel, full_path, &w, &h)
            : decode_png_file(full_path, &w, &h);
        if (!rgb) return false;
        if (content_play_superseded()) {
            free(rgb);
            return false;
        }
        pd_framebuf_blit_rgb(fb, rgb, (int)w, (int)h);
        free(rgb);
        return true;
    }
    return false;
}

/* helper: set up playback state for a content path */
static esp_err_t content_setup_playback(const char *path, const char *full)
{
    /* New content: re-clear margins if we enter bounds mode again. */
    content_sprite_invalidate();
    content_is_sprite_scene = false;
    content_sprite_pending_assembled = false;
    pd_sprite_scene_unload();

    /* Always start from persisted global defaults, then apply per-item
     * overrides. Fully reset compositing caches so a previous item's
     * animated overlay (e.g. lizard leaves) cannot leak onto the next play. */
    strlcpy(content_config.background, saved_background, sizeof(content_config.background));
    strlcpy(content_config.overlay, saved_overlay, sizeof(content_config.overlay));
    content_reset_compositing_state();

    if (path_is_dir(full) && pd_sprite_scene_detect(full)) {
        strlcpy(content_current, full, sizeof(content_current));
        content_is_seq = false;
        content_is_sprite_scene = true;
        content_fps = pd_sprite_scene_fps();
        content_reset_achieved_fps();
        content_loop = true;
        content_total_frames = 0;
        content_frame = 0;
        content_last_frame_us = 0;
        content_src_w = 0;
        content_src_h = 0;
        content_pattern_hash = 0;
        content_log_line("play sprite-scene %s", path);
        return ESP_OK;
    }

    if (path_is_dir(full)) {
        int fps = 12;
        bool loop = true;
        int frames = 0;
        char pattern[64] = "%04d.png";
        int start = 1;
        char item_bg[PD_CONTENT_MAX_PATH] = "";
        char item_ov[PD_CONTENT_MAX_PATH] = "";
        int meta_w = 0, meta_h = 0;
        bool have_meta = load_sequence_meta(full, &fps, &loop, &frames, pattern, sizeof(pattern),
                                            &start, item_bg, sizeof(item_bg), item_ov,
                                            sizeof(item_ov), &meta_w, &meta_h);
        if (!have_meta || frames <= 0) {
            /* meta missing/corrupt, or frames omitted — discover from directory */
            int disc_start = 1;
            int disc_frames = 0;
            char disc_pattern[64] = "%04d.png";
            if (discover_sequence_from_dir(full, disc_pattern, sizeof(disc_pattern),
                                           &disc_start, &disc_frames) &&
                disc_frames > 0) {
                if (!have_meta || pattern[0] == '\0' || strcmp(pattern, "%04d.png") == 0) {
                    strlcpy(pattern, disc_pattern, sizeof(pattern));
                    start = disc_start;
                }
                if (frames <= 0) frames = disc_frames;
            }
        }

        if (frames == 0) {
            ESP_LOGW(TAG, "no frames in %s", full);
            return ESP_ERR_NOT_FOUND;
        }
        if (fps < 1 || fps > 120) fps = 12;

#if PD_CONTENT_COMPOSITING_ENABLED
        /* apply per-item background/overlay if specified (play-time only) */
        if (item_bg[0] != '\0') {
            strlcpy(content_config.background, item_bg, sizeof(content_config.background));
        }
        if (item_ov[0] != '\0') {
            strlcpy(content_config.overlay, item_ov, sizeof(content_config.overlay));
        }
#else
        (void)item_bg;
        (void)item_ov;
#endif

        strlcpy(content_current, full, sizeof(content_current));
        strlcpy(content_frame_pattern, pattern, sizeof(content_frame_pattern));
        content_frame_start = start;
        content_is_seq = true;
        content_fps = fps > 0 ? fps : 12;
        content_reset_achieved_fps();
        content_loop = loop;
        content_total_frames = frames;
        content_frame = start;
        content_last_frame_us = 0;
        content_src_w = meta_w;
        content_src_h = meta_h;
        content_pattern_hash = content_fnv1a(pattern);
        /* content_playing set by caller after setup complete */

        content_log_line("play seq %s frames=%d fps=%d", path, frames, content_fps);
    } else if (path_exists(full) && is_png(full)) {
        strlcpy(content_current, full, sizeof(content_current));
        content_is_seq = false;
        content_frame = 0;
        content_total_frames = 1;
        content_src_w = 0;
        content_src_h = 0;
        content_pattern_hash = 0;
        /* content_playing set by caller after setup complete */

        ESP_LOGI(TAG, "displaying static: %s", path);
    } else {
        ESP_LOGW(TAG, "content not found: %s", full);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t pd_content_play(const char *path)
{
    /* cancel any active status overlay — explicit play request takes priority */
    if (status_overlay_until_us > 0) {
        status_overlay_until_us = 0;
        status_resume_path[0] = '\0';
        status_resume_was_playing = false;
        status_overlay_just_expired = false;
    }

    /* stop current playback immediately so tick() won't block us.
     * Do NOT wipe LRU palette slots — only deactivate the active one. */
    content_playing = false;
    content_abort_active_transition();
    content_invalidate_prefetch();
    content_seq_cache_deactivate();
    strlcpy(content_rel_path, path, sizeof(content_rel_path));

    /* Handle special system paths */
    if (strcmp(path, "system/default") == 0) {
        pd_display_render_default_marquee();
        strlcpy(content_current, "system/default", sizeof(content_current));
        content_is_seq = false;
        content_frame = 0;
        content_total_frames = 1;
        content_playing = true;
        ESP_LOGI(TAG, "displaying default marquee");
        return ESP_OK;
    }
    if (strcmp(path, "system/idle") == 0) {
        /* Display panel configuration screen - render with current settings */
        const char *ip = pd_network_get_ip();
        int dw = pd_display_get_width();
        int dh = pd_display_get_height();
        /* Call render_idle with current display dimensions and IP */
        pd_display_render_idle(
            "pixel-dumpster",  /* default name */
            dw, dh,
            0,  /* orientation */
            0,  /* scan */
            ip ? ip : "DHCP"
        );
        /* Mark as "playing" so status shows correctly */
        strlcpy(content_current, "system/idle", sizeof(content_current));
        content_is_seq = false;
        content_frame = 0;
        content_total_frames = 1;
        content_playing = true;
        ESP_LOGI(TAG, "displaying system idle screen");
        return ESP_OK;
    }

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    /* set up playback state first (applies per-item bg/overlay) */
    esp_err_t err = content_setup_playback(path, full);
    if (err != ESP_OK) {
        return err;
    }

    if (content_is_sprite_scene) {
        err = pd_sprite_scene_load(full);
        if (err != ESP_OK) {
            content_is_sprite_scene = false;
            return err;
        }
        content_total_frames = pd_sprite_scene_max_frames();
        content_fps = pd_sprite_scene_fps();
        if (content_play_superseded()) {
            pd_sprite_scene_unload();
            content_is_sprite_scene = false;
            content_playing = false;
            return ESP_OK;
        }
        pd_sprite_scene_start_bump();
        content_playing = true;
        return ESP_OK;
    }

    /* preload compositing cache before first frame */
    update_compositing_cache(pd_display_get_width(), pd_display_get_height());

    /* Start on the truecolor/decode path immediately so the sequence is
     * visible while the optional palette cache builds in the background. */
    {
        unsigned w = 0, h = 0;
        uint8_t *rgb = NULL;
        if (path_is_dir(full)) {
            char fp[PD_CONTENT_MAX_PATH];
            snprintf(fp, sizeof(fp), "%s/", full);
            size_t base_len = strlen(fp);
            snprintf(fp + base_len, sizeof(fp) - base_len, content_frame_pattern, content_frame_start);
            rgb = decode_png_file(fp, &w, &h);
        } else {
            rgb = decode_static_png_cached(path, full, &w, &h);
        }
        if (!rgb) {
            content_playing = false;
            return ESP_ERR_NOT_FOUND;
        }
        if (content_play_superseded()) {
            free(rgb);
            content_playing = false;
            return ESP_OK;
        }
        if (content_src_w <= 0 || content_src_h <= 0) {
            content_src_w = (int)w;
            content_src_h = (int)h;
        }
        content_present_source_rgb(rgb, (int)w, (int)h);
        free(rgb);
    }

    if (content_play_superseded()) {
        content_playing = false;
        return ESP_OK;
    }

    /* now safe to enable playback - everything is ready */
    content_playing = true;

    if (content_is_seq && content_total_frames > 1) {
        content_request_prefetch(content_frame_start + 1);
    }
    content_request_palette_cache_build();
    return ESP_OK;
}

esp_err_t pd_content_play_with_transition(const char *path, const char *transition,
                                          int duration_ms)
{
    /* Handle special system paths - they don't support transitions */
    if (strncmp(path, "system/", 7) == 0) {
        return pd_content_play(path);
    }

    if (!content_transition || !content_fb) {
        return pd_content_play(path);
    }

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    pd_transition_type_t type = pd_transition_type_from_name(transition);
    if (type == PD_TRANS_NONE) {
        return pd_content_play(path);
    }

    bool dest_is_sprite = pd_sprite_scene_detect(full);

    /* sprite-bump on a non-sprite destination → classic slide-left fallback. */
    if (type == PD_TRANS_SPRITE_BUMP_LEFT && !dest_is_sprite) {
        type = PD_TRANS_SLIDE_LEFT;
    }

    /* Capture frozen outgoing canvas before tearing down A.
     * Sprite scenes never push through content_present_*; rasterize the
     * live outgoing+slots into content_fb so from is panel-true Graphic A. */
    if (pd_sprite_scene_active() && content_fb) {
        if (pd_sprite_scene_capture_to(content_fb->data,
                                       content_fb->width, content_fb->height)) {
            content_fb_in_sync = true;
            content_last_src_rgb = content_fb->data;
            content_last_src_w = content_fb->width;
            content_last_src_h = content_fb->height;
        }
    } else {
        content_ensure_fb_current();
    }
    pd_framebuf_copy(content_transition->from, content_fb);
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();

    /* ---- sprite-bump-left: content-side push, not full-FB ---- */
    if (type == PD_TRANS_SPRITE_BUMP_LEFT) {
        content_playing = false;
        content_abort_active_transition();
        content_invalidate_prefetch();
        content_seq_cache_deactivate();
        content_sprite_pending_assembled = false;
        strlcpy(content_rel_path, path, sizeof(content_rel_path));

        esp_err_t err = content_setup_playback(path, full);
        if (err != ESP_OK) return err;

        err = pd_sprite_scene_load(full);
        if (err != ESP_OK) {
            content_is_sprite_scene = false;
            return err;
        }
        content_total_frames = pd_sprite_scene_max_frames();
        content_fps = pd_sprite_scene_fps();
        if (content_play_superseded()) {
            pd_sprite_scene_unload();
            content_is_sprite_scene = false;
            content_playing = false;
            return ESP_OK;
        }
        /* Outgoing = frozen full canvas of A (destination-owned bump). */
        (void)pd_sprite_scene_set_outgoing(content_transition->from->data, dw, dh, 0, 0);
        pd_sprite_scene_start_bump();
        content_playing = true;
        content_log_line("play sprite-bump-left %s (push outgoing)", path);
        return ESP_OK;
    }

    /* stop current playback so tick() won't interfere */
    content_playing = false;
    content_abort_active_transition();
    content_invalidate_prefetch();
    content_seq_cache_deactivate();
    content_sprite_pending_assembled = false;
    strlcpy(content_rel_path, path, sizeof(content_rel_path));

    /* set up playback state (applies per-item bg/overlay) */
    esp_err_t err = content_setup_playback(path, full);
    if (err != ESP_OK) {
        return err;
    }

    if (content_is_sprite_scene) {
        /* Classic FB into a sprite scene: rasterize centered final stack → to. */
        err = pd_sprite_scene_load(full);
        if (err != ESP_OK) {
            content_is_sprite_scene = false;
            return err;
        }
        content_total_frames = pd_sprite_scene_max_frames();
        content_fps = pd_sprite_scene_fps();
        if (content_play_superseded()) {
            pd_sprite_scene_unload();
            content_is_sprite_scene = false;
            content_playing = false;
            return ESP_OK;
        }
        if (!pd_sprite_scene_rasterize_stack(content_transition->to->data, dw, dh)) {
            pd_sprite_scene_unload();
            content_is_sprite_scene = false;
            return ESP_ERR_NO_MEM;
        }
        content_sprite_pending_assembled = true;
        pd_transition_start(content_transition, type, duration_ms);
        content_playing = true;
        content_log_line("play classic→sprite %s via %s", path, transition);
        return ESP_OK;
    }

    /* preload compositing cache for new content */
    update_compositing_cache(dw, dh);

    /* decode new content into "to" on the truecolor path; palette cache
     * builds in the background and tick will switch when ready. */
    if (!content_decode_first_frame(full, content_transition->to)) {
        return content_play_superseded() ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    if (content_play_superseded()) {
        content_playing = false;
        return ESP_OK;
    }

    /* start the transition */
    pd_transition_start(content_transition, type, duration_ms);

    /* now safe to enable playback - everything is ready */
    content_playing = true;

    if (content_is_seq && content_total_frames > 1) {
        content_request_prefetch(content_frame_start + 1);
    }
    content_request_palette_cache_build();
    return ESP_OK;
}

esp_err_t pd_content_stop(void)
{
    content_playing = false;
    content_invalidate_prefetch();
    content_seq_cache_deactivate();
    pd_sprite_scene_unload();
    content_is_sprite_scene = false;
    content_sprite_pending_assembled = false;
    content_current[0] = '\0';
    content_rel_path[0] = '\0';
    content_sprite_invalidate();
    content_reset_achieved_fps();
    /* Drop play-time compositing overrides so Stop doesn't leave lizard
     * leaves/bg armed for the next unrelated play or config save. */
    strlcpy(content_config.background, saved_background, sizeof(content_config.background));
    strlcpy(content_config.overlay, saved_overlay, sizeof(content_config.overlay));
    content_reset_compositing_state();
    pd_display_clear();
    content_log_line("stop");
    return ESP_OK;
}

pd_content_status_t pd_content_get_status(void)
{
    pd_content_status_t s = {0};
    s.playing = content_playing;
    strlcpy(s.cache, "off", sizeof(s.cache));
    if (content_playing && content_config.auto_quantize_palette && content_is_seq) {
        if (content_seq_cache_active && content_seq_cache_active->live) {
            strlcpy(s.cache, "live", sizeof(s.cache));
        } else if (content_cache_building) {
            strlcpy(s.cache, "building", sizeof(s.cache));
        } else if (content_cache_fallback) {
            strlcpy(s.cache, "fallback", sizeof(s.cache));
        } else {
            strlcpy(s.cache, "building", sizeof(s.cache));
        }
    }
    if (content_playing) {
        /* Prefer relative path for API consumers. */
        if (content_rel_path[0]) {
            strlcpy(s.current_path, content_rel_path, sizeof(s.current_path));
        } else {
            strlcpy(s.current_path, content_current, sizeof(s.current_path));
        }
        s.is_sequence = content_is_seq || content_is_sprite_scene;
        s.current_frame = content_frame;
        s.total_frames = content_is_sprite_scene
            ? pd_sprite_scene_slot_count()
            : content_total_frames;
        /* Static images must not advertise leftover sequence fps/achieved. */
        if (content_is_seq || content_is_sprite_scene) {
            s.fps = content_fps;
            s.achieved_fps = content_achieved_fps;
        } else {
            s.fps = 0;
            s.achieved_fps = 0.0f;
        }
    }
    return s;
}

/* Achieved-FPS window state — file-scope so play/stop/set_meta can reset it. */
static int content_fps_frames_since_log = 0;
static int64_t content_fps_window_start_us = 0;

static void content_reset_achieved_fps(void)
{
    content_achieved_fps = 0.0f;
    content_fps_frames_since_log = 0;
    content_fps_window_start_us = 0;
}

/* Lightweight achieved-FPS diagnostic: logs actual rendered frame rate once
 * per second while a sequence is playing, so playback performance can be
 * verified on-device without extra tooling. Negligible overhead (one
 * counter increment per frame, one log per second). */
static void pd_content_note_frame_rendered(int64_t now_us)
{
    if (content_fps_window_start_us == 0) {
        content_fps_window_start_us = now_us;
    }
    content_fps_frames_since_log++;

    int64_t elapsed = now_us - content_fps_window_start_us;
    if (elapsed >= 1000000) {
        content_achieved_fps = (float)(content_fps_frames_since_log * 1000000.0 / (double)elapsed);
        ESP_LOGI(TAG, "playback: achieved %.1f fps (target %d fps)",
                 (double)content_achieved_fps, content_fps);
        content_fps_frames_since_log = 0;
        content_fps_window_start_us = now_us;
    }
}

int pd_content_ms_until_next_frame(void)
{
    if (!content_playing) {
        return -1;
    }
    if (content_is_sprite_scene) {
        return pd_sprite_scene_ms_until_next();
    }
    if (!content_is_seq) {
        return -1;
    }
    /* Transitions drive their own cadence; don't pad the main loop. */
    if (content_transition && pd_transition_is_active(content_transition)) {
        return 0;
    }
    if (content_last_frame_us == 0) {
        return 0;
    }
    int fps = content_fps > 0 ? content_fps : 24;
    if (fps < 1) fps = 1;
    int64_t interval = 1000000 / fps;
    int64_t now = esp_timer_get_time();
    int64_t due = content_last_frame_us + interval;
    if (now >= due) {
        return 0;
    }
    int64_t remain_ms = (due - now + 999) / 1000;
    if (remain_ms > 10) remain_ms = 10;
    return (int)remain_ms;
}

void pd_content_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* handle on-demand source-status overlay */
    if (status_overlay_until_us > 0) {
        if (now < status_overlay_until_us) {
            /* still showing - re-render every 500ms to catch discovery state updates */
            if (now - status_last_render_us > 500000) {
                pd_content_render_source_status();
                status_last_render_us = now;
            }
            return;
        }
        /* timer expired - restore previous state */
        status_overlay_until_us = 0;
        if (status_resume_was_playing && status_resume_path[0]) {
            char path_copy[PD_CONTENT_MAX_PATH];
            strlcpy(path_copy, status_resume_path, sizeof(path_copy));
            status_resume_path[0] = '\0';
            status_resume_was_playing = false;
            pd_content_play(path_copy);
            return;
        }
        status_resume_path[0] = '\0';
        status_resume_was_playing = false;
        /* signal app-main to render idle screen */
        status_overlay_just_expired = true;
        /* fall through to normal tick (no-op since not playing) */
    }

    /* drive active transition — always full-canvas; frames frozen */
    if (content_transition && pd_transition_is_active(content_transition)) {
        content_sprite_invalidate();
        bool still_going = pd_transition_tick(content_transition);
        pd_display_render_framebuf(content_transition->out->data);
        if (!still_going) {
            pd_framebuf_copy(content_fb, content_transition->to);
            if (content_is_sprite_scene && content_sprite_pending_assembled) {
                content_sprite_pending_assembled = false;
                pd_sprite_scene_enter_assembled();
            }
        }
        return;  /* don't advance sequence frames during transition */
    }

    if (!content_playing) return;

    if (content_is_sprite_scene) {
        if (pd_sprite_scene_tick(now)) {
            pd_content_note_frame_rendered(now);
        }
        return;
    }

    /* use content fps for timing, default 24 for static content */
    int effective_fps = content_is_seq ? content_fps : 24;
    int64_t frame_interval = 1000000 / effective_fps;

#if PD_CONTENT_COMPOSITING_ENABLED
    /* advance animated overlay on its own fps (from overlay meta.json) */
    if (overlay_is_seq && overlay_total_frames > 0) {
        int ov_fps = overlay_fps > 0 ? overlay_fps : effective_fps;
        int64_t ov_interval = 1000000 / ov_fps;
        if (overlay_last_frame_us == 0) {
            overlay_last_frame_us = now;
        } else if ((now - overlay_last_frame_us) >= ov_interval) {
            overlay_last_frame_us = now;
            int next_ov = overlay_frame + 1;
            int last_ov = overlay_frame_start + overlay_total_frames - 1;
            if (next_ov > last_ov) {
                next_ov = overlay_frame_start;  /* loop overlay */
            }
            overlay_frame = next_ov;

            /* for static content, re-render with updated overlay */
            if (!content_is_seq) {
                unsigned w, h;
                uint8_t *rgb = decode_png_file(content_current, &w, &h);
                if (rgb) {
                    content_present_source_rgb(rgb, (int)w, (int)h);
                    free(rgb);
                }
            }
        }
    }
#endif /* PD_CONTENT_COMPOSITING_ENABLED */

    if (!content_is_seq) return;

    /* sequence frame timing (frame_interval already computed above).
     * Advance the schedule by one interval (not wall-clock now) so a slow
     * decode does not permanently stretch subsequent gaps — and so a target
     * of 4fps cannot collapse toward the decode-limited ~12fps cadence. */
    if (content_last_frame_us == 0) {
        content_last_frame_us = now;
        return;
    }

    if ((now - content_last_frame_us) < frame_interval) return;

    content_last_frame_us += frame_interval;
    /* If we fell more than one frame behind, resync to now so we don't burst. */
    if ((now - content_last_frame_us) > frame_interval) {
        content_last_frame_us = now;
    }
    int next = content_frame + 1;

    int last_frame = content_frame_start + content_total_frames - 1;
    if (next > last_frame) {
        if (content_loop) {
            next = content_frame_start;
        } else {
            return;
        }
    }

    bool displayed = false;

    /* Palette-cache fast path: expand pre-quantized indices from PSRAM.
     * Ping-pong: present front while the cache worker expands lookahead
     * into the back buffer (overlaps with blocking draw_pixels). */
    if (content_seq_cache_active && content_seq_cache_active->live) {
        pd_seq_cache_slot_t *slot = content_seq_cache_active;
        int last_fr = content_frame_start + content_total_frames - 1;
        int lookahead = next + 1;
        if (lookahead > last_fr) {
            lookahead = content_loop ? content_frame_start : -1;
        }

        if (content_pal_ensure_rgb_bufs(slot->width, slot->height)) {
            if (s_pal_back_ready && s_pal_ready_frame == next) {
                s_pal_front = 1 - s_pal_front;
                s_pal_back_ready = false;
                s_pal_ready_frame = -1;
            } else if (!content_pal_expand_frame_to_rgb(slot, next, s_pal_rgb[s_pal_front])) {
                content_seq_cache_expand_to_fb(next);
                content_frame = next;
                pd_content_note_frame_rendered(now);
                return;
            }
            content_present_source_rgb(s_pal_rgb[s_pal_front], slot->width, slot->height);
            if (lookahead >= 0) {
                content_request_pal_expand(lookahead);
            }
        } else {
            content_seq_cache_expand_to_fb(next);
        }
        content_frame = next;
        pd_content_note_frame_rendered(now);
        return;
    }

    /* figure out the frame after `next` up front so we can kick off its
     * decode *before* the (blocking) render call below — that's what
     * actually gets decode(next+1) running on the other core concurrently
     * with render(next) on this one, instead of the two running back to
     * back. Requesting after render would just move the wait, not remove
     * it. */
    int lookahead = next + 1;
    if (lookahead > last_frame) {
        lookahead = content_loop ? content_frame_start : -1;
    }

    if (content_prefetch_valid && content_prefetch_frame == next && content_prefetch_fb && content_fb) {
        /* fast path: the background decode task already prepared this
         * frame while we were busy rendering the previous one — just copy
         * it in, no decode needed on this tick at all. */
        pd_framebuf_copy(content_fb, content_prefetch_fb);
        int sw = content_prefetch_src_w > 0 ? content_prefetch_src_w : content_src_w;
        int sh = content_prefetch_src_h > 0 ? content_prefetch_src_h : content_src_h;
        const uint8_t *src = (content_prefetch_src_w > 0 && content_prefetch_src_rgb)
                             ? content_prefetch_src_rgb : NULL;
        content_prefetch_valid = false;
        /* Present before requesting lookahead — the decode task reuses
         * content_prefetch_src_rgb and must not overwrite it mid-push. */
        content_present_panel(src, sw, sh);
        if (lookahead >= 0) content_request_prefetch(lookahead);
        displayed = true;
    } else {
        /* fallback: prefetch wasn't ready in time (e.g. first couple of
         * frames after play(), or decode ran behind) — decode synchronously
         * as before so we never skip/stall a frame. */
        char frame_path[PD_CONTENT_MAX_PATH];
        snprintf(frame_path, sizeof(frame_path), "%s/", content_current);
        size_t base_len = strlen(frame_path);
        snprintf(frame_path + base_len, sizeof(frame_path) - base_len, content_frame_pattern, next);

        unsigned w, h;
        uint8_t *rgb = decode_png_file(frame_path, &w, &h);
        if (rgb) {
            /* re-check after decode — play handler may have interrupted us */
            if (!content_playing) {
                free(rgb);
                return;
            }
            if (content_src_w <= 0 || content_src_h <= 0) {
                content_src_w = (int)w;
                content_src_h = (int)h;
            }
            content_present_source_rgb(rgb, (int)w, (int)h);
            free(rgb);
            if (lookahead >= 0) content_request_prefetch(lookahead);
            displayed = true;
        }
    }

    if (displayed) {
        content_frame = next;
        pd_content_note_frame_rendered(now);
    }
}

/* ---- file storage ---- */

static void ensure_parent_dirs(const char *full_path)
{
    char dir[PD_CONTENT_MAX_PATH];
    strlcpy(dir, full_path, sizeof(dir));
    char *last_slash = strrchr(dir, '/');
    if (!last_slash) return;
    *last_slash = '\0';
    for (char *p = dir + strlen(content_base) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            ensure_dir(dir);
            *p = '/';
        }
    }
    ensure_dir(dir);
}

esp_err_t pd_content_store_file(const char *rel_path, const uint8_t *data, size_t len)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);
    ensure_parent_dirs(full);

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    fwrite(data, 1, len, f);
    fclose(f);

    static_rgb_cache_invalidate(rel_path);
    ESP_LOGI(TAG, "stored %s (%d bytes)", rel_path, (int)len);
    return ESP_OK;
}

/* ---- streaming upload (serial / BLE) ---- */

static FILE *s_upload_fp = NULL;
static char s_upload_rel[PD_CONTENT_MAX_PATH] = "";
static char s_upload_full[PD_CONTENT_MAX_PATH] = "";
static size_t s_upload_total = 0;
static size_t s_upload_received = 0;

void pd_content_upload_abort(void)
{
    if (s_upload_fp) {
        fclose(s_upload_fp);
        s_upload_fp = NULL;
        if (s_upload_full[0]) {
            remove(s_upload_full);
        }
    }
    s_upload_rel[0] = '\0';
    s_upload_full[0] = '\0';
    s_upload_total = 0;
    s_upload_received = 0;
}

bool pd_content_upload_active(void)
{
    return s_upload_fp != NULL;
}

size_t pd_content_upload_received(void)
{
    return s_upload_received;
}

esp_err_t pd_content_upload_begin(const char *rel_path, size_t total_size)
{
    if (!rel_path || !rel_path[0]) return ESP_ERR_INVALID_ARG;
    if (total_size == 0 || total_size > PD_CONTENT_UPLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_upload_fp) {
        pd_content_upload_abort();
    }

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);
    ensure_parent_dirs(full);

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload_begin: cannot create %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }

    s_upload_fp = f;
    strlcpy(s_upload_rel, rel_path, sizeof(s_upload_rel));
    strlcpy(s_upload_full, full, sizeof(s_upload_full));
    s_upload_total = total_size;
    s_upload_received = 0;
    ESP_LOGI(TAG, "upload_begin: %s (%u bytes)", rel_path, (unsigned)total_size);
    return ESP_OK;
}

esp_err_t pd_content_upload_write(const uint8_t *data, size_t len)
{
    if (!s_upload_fp || !data || len == 0) return ESP_ERR_INVALID_STATE;
    if (s_upload_received + len > s_upload_total) {
        ESP_LOGE(TAG, "upload_write: overflow %u+%u > %u",
                 (unsigned)s_upload_received, (unsigned)len, (unsigned)s_upload_total);
        pd_content_upload_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    size_t written = fwrite(data, 1, len, s_upload_fp);
    if (written != len) {
        ESP_LOGE(TAG, "upload_write: short write");
        pd_content_upload_abort();
        return ESP_FAIL;
    }
    s_upload_received += len;
    return ESP_OK;
}

esp_err_t pd_content_upload_finish(void)
{
    if (!s_upload_fp) return ESP_ERR_INVALID_STATE;
    if (s_upload_received != s_upload_total) {
        ESP_LOGE(TAG, "upload_finish: size mismatch %u/%u",
                 (unsigned)s_upload_received, (unsigned)s_upload_total);
        pd_content_upload_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    fflush(s_upload_fp);
    fclose(s_upload_fp);
    s_upload_fp = NULL;
    ESP_LOGI(TAG, "upload_finish: %s (%u bytes)", s_upload_rel, (unsigned)s_upload_received);
    if (s_upload_rel[0]) {
        static_rgb_cache_invalidate(s_upload_rel);
    }
    s_upload_rel[0] = '\0';
    s_upload_full[0] = '\0';
    s_upload_total = 0;
    s_upload_received = 0;
    return ESP_OK;
}

/* Recursively remove all files/subdirectories under `dir_path`, then the
 * directory itself. Used for deleting sequence content (a directory of
 * numbered frames + meta.json) from a single delete call.
 *
 * Important constraints:
 *  - LittleFS can skip/fail if we unlink while iterating readdir — so close
 *    the DIR before each unlink.
 *  - httpd task stack is only ~8KB; never put large name tables on stack
 *    (pac-ghost has 100+ frames). Delete one entry per open/close pass.
 */
static esp_err_t remove_dir_recursive(const char *dir_path)
{
    for (;;) {
        DIR *d = opendir(dir_path);
        if (!d) {
            ESP_LOGW(TAG, "cannot open dir %s: %s", dir_path, strerror(errno));
            return ESP_FAIL;
        }

        char name[64] = "";
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            /* Skip only . and .. — keep hidden sidecar files like .pd_palcache
             * so rmdir can succeed after the directory is emptied. */
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            strlcpy(name, ent->d_name, sizeof(name));
            break;
        }
        closedir(d);

        if (name[0] == '\0') {
            break; /* empty */
        }

        char child[PD_CONTENT_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", dir_path, name);

        if (path_is_dir(child)) {
            if (remove_dir_recursive(child) != ESP_OK) {
                return ESP_FAIL;
            }
        } else if (remove(child) != 0) {
            ESP_LOGW(TAG, "cannot delete %s: %s", child, strerror(errno));
            return ESP_FAIL;
        }
    }

    if (rmdir(dir_path) != 0) {
        ESP_LOGW(TAG, "cannot rmdir %s: %s", dir_path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool content_rel_path_ok(const char *rel)
{
    if (!rel || !rel[0]) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (strstr(rel, "..") != NULL) return false;
    if (strchr(rel, '\\') != NULL) return false;
    return true;
}

esp_err_t pd_content_delete_file(const char *rel_path)
{
    if (!content_rel_path_ok(rel_path)) return ESP_ERR_INVALID_ARG;

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);

    /* Stop if this path is current (full or relative match). Also stop when
     * deleting a parent directory of the playing path. */
    bool playing_this = false;
    if (content_current[0] && strcmp(content_current, full) == 0) {
        playing_this = true;
    } else if (content_rel_path[0] && strcmp(content_rel_path, rel_path) == 0) {
        playing_this = true;
    } else if (content_current[0] && path_is_dir(full)) {
        size_t n = strlen(full);
        if (strncmp(content_current, full, n) == 0 &&
            (content_current[n] == '/' || content_current[n] == '\0')) {
            playing_this = true;
        }
    }
    if (playing_this) {
        pd_content_stop();
    }

    static_rgb_cache_invalidate(rel_path);
    content_invalidate_prefetch();
    content_seq_cache_deactivate();

    /* Wait briefly for an in-flight PNG decode to finish so LittleFS unlinks
     * are not racing an open file on another core. */
    for (int i = 0; i < 40 && content_decode_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (!path_exists(full) && !path_is_dir(full)) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err;
    if (path_is_dir(full)) {
        err = remove_dir_recursive(full);
    } else {
        err = (remove(full) == 0) ? ESP_OK : ESP_FAIL;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cannot delete %s: %s", full, strerror(errno));
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "deleted %s", rel_path);
    }
    return err;
}

esp_err_t pd_content_rename(const char *from_rel, const char *to_rel)
{
    if (!content_rel_path_ok(from_rel) || !content_rel_path_ok(to_rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(from_rel, to_rel) == 0) return ESP_OK;

    char from_full[PD_CONTENT_MAX_PATH];
    char to_full[PD_CONTENT_MAX_PATH];
    snprintf(from_full, sizeof(from_full), "%s/%s", content_base, from_rel);
    snprintf(to_full, sizeof(to_full), "%s/%s", content_base, to_rel);

    struct stat st;
    if (stat(from_full, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (stat(to_full, &st) == 0) {
        return ESP_ERR_INVALID_STATE; /* destination exists */
    }

    if (content_current[0] && strcmp(content_current, from_full) == 0) {
        pd_content_stop();
    }

    ensure_parent_dirs(to_full);
    if (rename(from_full, to_full) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed: %s", from_rel, to_rel, strerror(errno));
        return ESP_FAIL;
    }

    static_rgb_cache_invalidate(from_rel);
    static_rgb_cache_invalidate(to_rel);
    ESP_LOGI(TAG, "renamed %s -> %s", from_rel, to_rel);
    return ESP_OK;
}

esp_err_t pd_content_set_sequence_fps(const char *rel_path, int fps)
{
    if (!content_rel_path_ok(rel_path)) return ESP_ERR_INVALID_ARG;
    if (fps < 1 || fps > 120) return ESP_ERR_INVALID_ARG;

    char dir_full[PD_CONTENT_MAX_PATH];
    snprintf(dir_full, sizeof(dir_full), "%s/%s", content_base, rel_path);
    if (!path_is_dir(dir_full)) {
        return ESP_ERR_NOT_FOUND;
    }

    char meta_path[PD_CONTENT_MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.json", dir_full);

    cJSON *root = NULL;
    FILE *f = fopen(meta_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz > 0 && sz <= 4096) {
            char *buf = calloc(1, (size_t)sz + 1);
            if (buf) {
                fread(buf, 1, (size_t)sz, f);
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }
    if (!root) {
        root = cJSON_CreateObject();
    }
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_DeleteItemFromObject(root, "fps");
    cJSON_AddNumberToObject(root, "fps", fps);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    f = fopen(meta_path, "w");
    if (!f) {
        free(json);
        ESP_LOGW(TAG, "cannot write %s: %s", meta_path, strerror(errno));
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    /* If this sequence is currently playing, update live fps and reset the
     * frame timer so the new interval applies on the next tick. */
    bool playing_this =
        content_is_seq &&
        ((content_current[0] && strcmp(content_current, dir_full) == 0) ||
         (content_rel_path[0] && strcmp(content_rel_path, rel_path) == 0));
    if (playing_this) {
        content_fps = fps;
        content_last_frame_us = 0;
        overlay_last_frame_us = 0;
        content_reset_achieved_fps();
        content_log_line("set_meta live fps=%d path=%s", fps, rel_path);
    } else {
        content_log_line("set_meta fps=%d path=%s", fps, rel_path);
    }

    return ESP_OK;
}

/* ---- HTTP handlers ---- */

static esp_err_t http_content_list(httpd_req_t *req)
{
    static pd_content_entry_t entries[PD_CONTENT_MAX_LIST];
    int count = pd_content_list_images(entries, PD_CONTENT_MAX_LIST);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "images");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path", entries[i].path);
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddBoolToObject(item, "sequence", entries[i].is_sequence);
        cJSON_AddNumberToObject(item, "frames", entries[i].frame_count);
        if (entries[i].is_sequence) {
            cJSON_AddNumberToObject(item, "fps", entries[i].fps);
        }
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* ---- async HTTP play worker ----
 * Palette-cache builds can take 5–20s. ESP httpd serves requests on a single
 * worker task, so a synchronous play() starved /api/status and made the
 * desktop app report "status timed out". Queue play onto a dedicated task
 * and ACK the HTTP request immediately after a cheap path existence check. */
typedef struct {
    char path[PD_CONTENT_MAX_PATH];
    char transition[32];
    int  duration_ms;
    bool use_transition;
    uint32_t gen;
} content_play_req_t;

static content_play_req_t s_play_req;
static portMUX_TYPE s_play_req_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_play_worker = NULL;
static volatile bool s_play_req_pending = false;

static bool content_path_exists(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    if (strncmp(path, "system/", 7) == 0) {
        return true;
    }
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);
    struct stat st;
    return stat(full, &st) == 0;
}

static void content_play_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            content_play_req_t req;
            portENTER_CRITICAL(&s_play_req_lock);
            if (!s_play_req_pending) {
                portEXIT_CRITICAL(&s_play_req_lock);
                break;
            }
            req = s_play_req;
            s_play_req_pending = false;
            portEXIT_CRITICAL(&s_play_req_lock);

            s_play_active_gen = req.gen;
            /* A newer enqueue may already have bumped s_play_gen — skip work. */
            if (content_play_superseded()) {
                continue;
            }

            esp_err_t err;
            if (req.use_transition) {
                err = pd_content_play_with_transition(req.path, req.transition, req.duration_ms);
            } else {
                err = pd_content_play(req.path);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "async play failed for %s: %s", req.path, esp_err_to_name(err));
            }
        }
    }
}

static bool content_play_ensure_worker(void)
{
    if (s_play_worker != NULL) {
        return true;
    }

    /* Internal stack only — PNG/LittleFS flash ops are unsafe on a PSRAM
     * stack (cache-disabled windows). Create early in pd_content_init so
     * the 12 KiB is reserved before NimBLE/WiFi fragment internal heap. */
    BaseType_t ok = xTaskCreate(content_play_worker, "pd_play", 12288, NULL,
                                 tskIDLE_PRIORITY + 2, &s_play_worker);
    if (ok == pdPASS && s_play_worker != NULL) {
        ESP_LOGI(TAG, "play worker created");
        return true;
    }
    s_play_worker = NULL;
    ESP_LOGE(TAG, "failed to create play worker");
    return false;
}

static bool content_play_enqueue(const char *path, const char *trans_name, int dur, bool use_transition)
{
    if (!content_play_ensure_worker()) {
        return false;
    }

    portENTER_CRITICAL(&s_play_req_lock);
    strlcpy(s_play_req.path, path, sizeof(s_play_req.path));
    if (trans_name && trans_name[0]) {
        strlcpy(s_play_req.transition, trans_name, sizeof(s_play_req.transition));
    } else {
        s_play_req.transition[0] = '\0';
    }
    s_play_req.duration_ms = dur;
    s_play_req.use_transition = use_transition;
    /* Bump generation so an in-flight decode/present for an older path is dropped. */
    s_play_gen++;
    if (s_play_gen == 0) {
        s_play_gen = 1;
    }
    s_play_req.gen = s_play_gen;
    s_play_req_pending = true;
    portEXIT_CRITICAL(&s_play_req_lock);

    xTaskNotifyGive(s_play_worker);
    return true;
}

esp_err_t pd_content_play_async(const char *path, const char *transition, int duration_ms)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!content_path_exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    bool use_transition = transition && transition[0] && duration_ms > 0;
    if (!content_play_enqueue(path, transition, duration_ms, use_transition)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t http_content_play(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_FAIL;
    }

    if (!content_path_exists(path->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "content not found");
        return ESP_FAIL;
    }

    cJSON *j_trans = cJSON_GetObjectItem(root, "transition");
    cJSON *j_dur   = cJSON_GetObjectItem(root, "duration_ms");

    const char *trans_name = NULL;
    int dur = content_config.trans_duration_ms;

    if (cJSON_IsString(j_trans)) {
        /* explicit transition from request */
        trans_name = j_trans->valuestring;
    } else {
        /* use config mode to select transition */
        trans_name = select_transition_name();
    }

    if (cJSON_IsNumber(j_dur)) {
        dur = j_dur->valueint;
    }

    pd_transition_type_t type = pd_transition_type_from_name(trans_name);
    bool use_transition = (type != PD_TRANS_NONE);
    content_play_enqueue(path->valuestring, trans_name, dur, use_transition);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_stop(httpd_req_t *req)
{
    pd_content_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_status_show(httpd_req_t *req)
{
    int duration_ms = 5000;

    /* Read optional JSON body: {"duration_ms": N} */
    int content_len = req->content_len;
    if (content_len > 0 && content_len < 256) {
        char buf[256];
        int received = httpd_req_recv(req, buf, content_len);
        if (received > 0) {
            buf[received] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
                if (cJSON_IsNumber(dur)) {
                    duration_ms = dur->valueint;
                }
                cJSON_Delete(root);
            }
        }
    }

    pd_content_show_source_status_for(duration_ms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_status(httpd_req_t *req)
{
    pd_content_status_t s = pd_content_get_status();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", s.playing);
    cJSON_AddStringToObject(root, "cache", s.cache);
    if (s.playing) {
        cJSON_AddStringToObject(root, "path", s.current_path);
        cJSON_AddBoolToObject(root, "sequence", s.is_sequence);
        cJSON_AddNumberToObject(root, "frame", s.current_frame);
        cJSON_AddNumberToObject(root, "total_frames", s.total_frames);
        if (s.is_sequence) {
            cJSON_AddNumberToObject(root, "fps", s.fps);
            if (s.achieved_fps > 0.05f) {
                cJSON_AddNumberToObject(root, "achieved_fps", s.achieved_fps);
            }
        }
        if (s.is_sequence && content_cache_fallback && content_cache_fail_reason[0]) {
            cJSON_AddStringToObject(root, "cache_reason", content_cache_fail_reason);
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_content_log(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "lines");
    int start = (content_log_head - content_log_count + PD_CONTENT_LOG_LINES) % PD_CONTENT_LOG_LINES;
    for (int i = 0; i < content_log_count; i++) {
        int idx = (start + i) % PD_CONTENT_LOG_LINES;
        cJSON_AddItemToArray(arr, cJSON_CreateString(content_log_lines[idx]));
    }
    if (content_cache_fail_reason[0]) {
        cJSON_AddStringToObject(root, "cache_reason", content_cache_fail_reason);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_config_get(httpd_req_t *req)
{
    const char *mode_str = "baseline";
    if (content_config.trans_mode == PD_TRANS_MODE_RANDOM) mode_str = "random";
    else if (content_config.trans_mode == PD_TRANS_MODE_PER_ITEM) mode_str = "per-item";

    cJSON *root = cJSON_CreateObject();
    cJSON *trans = cJSON_AddObjectToObject(root, "transition");
    cJSON_AddStringToObject(trans, "mode", mode_str);
    cJSON_AddStringToObject(trans, "baseline", content_config.trans_baseline);
    cJSON_AddNumberToObject(trans, "duration_ms", content_config.trans_duration_ms);

    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(disp, "hold_ms", content_config.hold_ms);
    cJSON_AddBoolToObject(disp, "loop_sequences", content_config.loop_sequences);
    /* Report persisted globals, not the active play-time composite override. */
    cJSON_AddStringToObject(disp, "background", saved_background);
    cJSON_AddStringToObject(disp, "overlay", saved_overlay);
    cJSON_AddBoolToObject(disp, "auto_quantize_palette", content_config.auto_quantize_palette);
    cJSON_AddBoolToObject(disp, "show_fps_counter", content_config.show_fps_counter);

    cJSON *attr = cJSON_AddObjectToObject(root, "attract");
    cJSON_AddBoolToObject(attr, "enabled", content_config.attract_enabled);
    cJSON_AddStringToObject(attr, "path", content_config.attract_path);
    cJSON_AddBoolToObject(attr, "shuffle", content_config.attract_shuffle);
    cJSON_AddNumberToObject(attr, "idle_timeout_ms", content_config.attract_idle_timeout_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_config_set(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* merge partial updates */
    cJSON *trans = cJSON_GetObjectItem(root, "transition");
    if (trans) {
        cJSON *mode = cJSON_GetObjectItem(trans, "mode");
        if (cJSON_IsString(mode)) {
            if (strcmp(mode->valuestring, "random") == 0)
                content_config.trans_mode = PD_TRANS_MODE_RANDOM;
            else if (strcmp(mode->valuestring, "baseline") == 0)
                content_config.trans_mode = PD_TRANS_MODE_BASELINE;
            else if (strcmp(mode->valuestring, "per-item") == 0)
                content_config.trans_mode = PD_TRANS_MODE_PER_ITEM;
        }
        cJSON *baseline = cJSON_GetObjectItem(trans, "baseline");
        if (cJSON_IsString(baseline))
            strlcpy(content_config.trans_baseline, baseline->valuestring,
                    sizeof(content_config.trans_baseline));
        cJSON *dur = cJSON_GetObjectItem(trans, "duration_ms");
        if (cJSON_IsNumber(dur))
            content_config.trans_duration_ms = dur->valueint;
    }

    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (disp) {
        cJSON *hold = cJSON_GetObjectItem(disp, "hold_ms");
        if (cJSON_IsNumber(hold))
            content_config.hold_ms = hold->valueint;
        cJSON *loop_seq = cJSON_GetObjectItem(disp, "loop_sequences");
        if (cJSON_IsBool(loop_seq))
            content_config.loop_sequences = cJSON_IsTrue(loop_seq);
        cJSON *bg = cJSON_GetObjectItem(disp, "background");
        if (cJSON_IsString(bg)) {
            strlcpy(content_config.background, bg->valuestring,
                    sizeof(content_config.background));
            strlcpy(saved_background, bg->valuestring, sizeof(saved_background));
        }
        cJSON *ov = cJSON_GetObjectItem(disp, "overlay");
        if (cJSON_IsString(ov)) {
            strlcpy(content_config.overlay, ov->valuestring,
                    sizeof(content_config.overlay));
            strlcpy(saved_overlay, ov->valuestring, sizeof(saved_overlay));
        }
        cJSON *aq = cJSON_GetObjectItem(disp, "auto_quantize_palette");
        if (cJSON_IsBool(aq))
            content_config.auto_quantize_palette = cJSON_IsTrue(aq);
        cJSON *sfc = cJSON_GetObjectItem(disp, "show_fps_counter");
        if (cJSON_IsBool(sfc))
            content_config.show_fps_counter = cJSON_IsTrue(sfc);
    }

    cJSON *attr = cJSON_GetObjectItem(root, "attract");
    if (attr) {
        cJSON *enabled = cJSON_GetObjectItem(attr, "enabled");
        if (cJSON_IsBool(enabled))
            content_config.attract_enabled = cJSON_IsTrue(enabled);
        cJSON *apath = cJSON_GetObjectItem(attr, "path");
        if (cJSON_IsString(apath))
            strlcpy(content_config.attract_path, apath->valuestring,
                    sizeof(content_config.attract_path));
        cJSON *shuffle = cJSON_GetObjectItem(attr, "shuffle");
        if (cJSON_IsBool(shuffle))
            content_config.attract_shuffle = cJSON_IsTrue(shuffle);
        cJSON *idle = cJSON_GetObjectItem(attr, "idle_timeout_ms");
        if (cJSON_IsNumber(idle))
            content_config.attract_idle_timeout_ms = idle->valueint;
    }

    /* device config fields (optional partial update) */
    pd_config_t *cfg = pd_config_get_active();
    if (cfg) {
        cJSON *dev_name = cJSON_GetObjectItem(root, "device_name");
        cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
        cJSON *wifi_password = cJSON_GetObjectItem(root, "wifi_password");
        if (cJSON_IsString(dev_name)) {
            strlcpy(cfg->device_name, dev_name->valuestring, sizeof(cfg->device_name));
        }
        if (cJSON_IsString(wifi_ssid)) {
            strlcpy(cfg->wifi_ssid, wifi_ssid->valuestring, sizeof(cfg->wifi_ssid));
        }
        if (cJSON_IsString(wifi_password)) {
            strlcpy(cfg->wifi_password, wifi_password->valuestring, sizeof(cfg->wifi_password));
        }
        if (dev_name || wifi_ssid || wifi_password) {
            pd_config_save(cfg);
        }
    }

    /* optionally save content config to disk */
    cJSON *save = cJSON_GetObjectItem(root, "save");
    bool do_save = cJSON_IsTrue(save);
    cJSON_Delete(root);

    if (do_save) {
        pd_content_save_config();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Minimal %XX decoder so ?path=images%2Ffoo.png works. ESP's
 * httpd_query_key_value does not turn %2F back into '/'. */
static void url_decode_inplace(char *s)
{
    char *src = s;
    char *dst = s;
    while (*src) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end && end != hex) {
                *dst++ = (char)v;
                src += 3;
                continue;
            }
        } else if (src[0] == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static esp_err_t http_content_upload(httpd_req_t *req)
{
    /* path comes from query string: ?path=images/foo.png */
    char query[256] = "";
    char rel_path[PD_CONTENT_MAX_PATH] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", rel_path, sizeof(rel_path));
        url_decode_inplace(rel_path);
    }

    if (rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total <= 0 || total > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    /* Stream to storage in chunks to avoid large RAM allocation */
    #define CHUNK_SIZE 8192
    uint8_t *chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    /* Ensure parent directories exist */
    char full_path[PD_CONTENT_MAX_PATH + 32];
    snprintf(full_path, sizeof(full_path), "%s/%s", content_base, rel_path);
    
    char dir[PD_CONTENT_MAX_PATH];
    strlcpy(dir, full_path, sizeof(dir));
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        /* create nested dirs one level at a time */
        for (char *p = dir + strlen(content_base) + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                ensure_dir(dir);
                *p = '/';
            }
        }
        ensure_dir(dir);
    }
    
    /* Open file for writing */
    FILE *f = fopen(full_path, "wb");
    if (!f) {
        free(chunk);
        ESP_LOGE(TAG, "upload: cannot create file: %s", full_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
        return ESP_FAIL;
    }

    int received = 0;
    esp_err_t err = ESP_OK;
    
    while (received < total) {
        int to_recv = (total - received) < CHUNK_SIZE ? (total - received) : CHUNK_SIZE;
        int ret = httpd_req_recv(req, (char *)chunk, to_recv);
        if (ret <= 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "upload: receive failed at %d/%d", received, total);
            break;
        }
        
        size_t written = fwrite(chunk, 1, ret, f);
        if (written != (size_t)ret) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "upload: write failed at %d/%d", received, total);
            break;
        }
        
        received += ret;
    }

    fflush(f);
    fclose(f);
    free(chunk);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "uploaded: %s (%d bytes)", rel_path, received);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/ota — raw firmware binary body → inactive OTA slot → reboot */
static esp_err_t http_ota_update(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total <= 0 || (size_t)total > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing %d bytes to %s @ 0x%lx",
             total, part->label, (unsigned long)part->address);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }

    #define OTA_CHUNK_SIZE 4096
    uint8_t *chunk = malloc(OTA_CHUNK_SIZE);
    if (!chunk) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int to_recv = (total - received) < OTA_CHUNK_SIZE ? (total - received) : OTA_CHUNK_SIZE;
        int ret = httpd_req_recv(req, (char *)chunk, to_recv);
        if (ret <= 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "OTA: receive failed at %d/%d", received, total);
            break;
        }
        err = esp_ota_write(handle, chunk, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed at %d/%d: %s",
                     received, total, esp_err_to_name(err));
            break;
        }
        received += ret;
    }
    free(chunk);

    if (err != ESP_OK) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: end/validate failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota validate failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success (%d bytes), rebooting", received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_content_delete(httpd_req_t *req)
{
    /* Prefer JSON body { "path": "images/foo.png" } (POST /api/content/delete).
     * Fall back to ?path= for DELETE /api/content (with URL-decode). */
    char rel_path[PD_CONTENT_MAX_PATH] = "";

    if (req->method == HTTP_POST && req->content_len > 0 && req->content_len < 512) {
        char buf[512];
        int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *p = cJSON_GetObjectItem(root, "path");
                if (cJSON_IsString(p) && p->valuestring) {
                    strlcpy(rel_path, p->valuestring, sizeof(rel_path));
                }
                cJSON_Delete(root);
            }
        }
    }

    if (rel_path[0] == '\0') {
        char query[256] = "";
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "path", rel_path, sizeof(rel_path));
            url_decode_inplace(rel_path);
        }
    }

    if (rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_FAIL;
    }

    esp_err_t err = pd_content_delete_file(rel_path);
    if (err != ESP_OK) {
        httpd_resp_send_err(req,
            err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
            "delete failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_rename(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (!cJSON_IsString(from) || !cJSON_IsString(to)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "from/to required");
        return ESP_FAIL;
    }

    esp_err_t err = pd_content_rename(from->valuestring, to->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req,
            err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
            "rename failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_set_meta(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *fps = cJSON_GetObjectItem(root, "fps");
    if (!cJSON_IsString(path) || !cJSON_IsNumber(fps)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path and fps required");
        return ESP_FAIL;
    }

    esp_err_t err = pd_content_set_sequence_fps(path->valuestring, fps->valueint);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req,
            err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_400_BAD_REQUEST,
            "set meta failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_layout_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "panel_width", pd_display_get_panel_width());
    cJSON_AddNumberToObject(root, "panel_height", pd_display_get_panel_height());
    cJSON_AddNumberToObject(root, "panel_rows", pd_display_get_panel_rows());
    cJSON_AddNumberToObject(root, "panel_cols", pd_display_get_panel_cols());
    cJSON_AddNumberToObject(root, "chain_pattern", pd_display_get_chain_pattern());
    cJSON_AddNumberToObject(root, "panel_rotation_deg", pd_display_get_rotation_deg());
    cJSON_AddNumberToObject(root, "color_order", pd_display_get_color_order());
    cJSON_AddNumberToObject(root, "matrix_width", pd_display_get_width());
    cJSON_AddNumberToObject(root, "matrix_height", pd_display_get_height());

    pd_config_t *cfg = pd_config_get_active();
    if (cfg) {
        cJSON_AddStringToObject(root, "device_name", cfg->device_name);
        cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
        cJSON_AddStringToObject(root, "hostname", cfg->hostname);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_layout_set(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    pd_config_t *cfg = pd_config_get_active();
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config not active");
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetObjectItem(root, "panel_width");
    if (cJSON_IsNumber(item)) cfg->panel_width = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_height");
    if (cJSON_IsNumber(item)) cfg->panel_height = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_rows");
    if (cJSON_IsNumber(item)) cfg->panel_rows = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_cols");
    if (cJSON_IsNumber(item)) cfg->panel_cols = item->valueint;
    item = cJSON_GetObjectItem(root, "chain_pattern");
    if (cJSON_IsNumber(item)) cfg->chain_pattern = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_rotation_deg");
    if (cJSON_IsNumber(item)) cfg->panel_rotation_deg = item->valueint;
    item = cJSON_GetObjectItem(root, "color_order");
    if (cJSON_IsNumber(item)) cfg->color_order = item->valueint;

    /* recompute virtual canvas
     * With 90/270° rotation the driver swaps physical width and height:
     *   virtual_w = panel_h * rows,  virtual_h = panel_w * cols
     * Without rotation:
     *   virtual_w = panel_w * cols,  virtual_h = panel_h * rows
     */
    if (cfg->panel_rotation_deg == 90 || cfg->panel_rotation_deg == 270) {
        cfg->matrix_width  = cfg->panel_height * cfg->panel_rows;
        cfg->matrix_height = cfg->panel_width  * cfg->panel_cols;
    } else {
        cfg->matrix_width  = cfg->panel_width  * cfg->panel_cols;
        cfg->matrix_height = cfg->panel_height * cfg->panel_rows;
    }

    pd_config_save(cfg);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);

    /* reboot after a short delay so the response can be sent */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* Live preview: apply layout changes without saving or rebooting.
 * Mutates the active in-memory config and reinitialises the driver.
 * Changes are lost on reboot (the last saved config is restored). */
static esp_err_t http_config_preview(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    pd_config_t *cfg = pd_config_get_active();
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config not active");
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetObjectItem(root, "panel_width");
    if (cJSON_IsNumber(item)) cfg->panel_width = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_height");
    if (cJSON_IsNumber(item)) cfg->panel_height = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_rows");
    if (cJSON_IsNumber(item)) cfg->panel_rows = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_cols");
    if (cJSON_IsNumber(item)) cfg->panel_cols = item->valueint;
    item = cJSON_GetObjectItem(root, "chain_pattern");
    if (cJSON_IsNumber(item)) cfg->chain_pattern = item->valueint;
    item = cJSON_GetObjectItem(root, "panel_rotation_deg");
    if (cJSON_IsNumber(item)) cfg->panel_rotation_deg = item->valueint;
    item = cJSON_GetObjectItem(root, "color_order");
    if (cJSON_IsNumber(item)) cfg->color_order = item->valueint;

    /* recompute virtual canvas */
    if (cfg->panel_rotation_deg == 90 || cfg->panel_rotation_deg == 270) {
        cfg->matrix_width  = cfg->panel_height * cfg->panel_rows;
        cfg->matrix_height = cfg->panel_width  * cfg->panel_cols;
    } else {
        cfg->matrix_width  = cfg->panel_width  * cfg->panel_cols;
        cfg->matrix_height = cfg->panel_height * cfg->panel_rows;
    }

    pd_display_config_t display_config = {
        .panel_width    = cfg->panel_width,
        .panel_height   = cfg->panel_height,
        .panel_rows     = cfg->panel_rows,
        .panel_cols     = cfg->panel_cols,
        .chain_pattern  = cfg->chain_pattern,
        .rotation_deg   = cfg->panel_rotation_deg,
        .scan_wiring    = cfg->scan_wiring,
        .color_order    = cfg->color_order,
    };
    esp_err_t err = pd_display_reinit(&display_config);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Layout preview requires reboot on this hardware\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"reinit failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_test_start(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *pattern = cJSON_GetObjectItem(root, "pattern");
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");
    if (!cJSON_IsString(pattern)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pattern required");
        return ESP_FAIL;
    }

    pd_test_pattern_t p = pd_display_test_pattern_from_name(pattern->valuestring);
    if (p == PD_TEST_PATTERN_NONE) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown pattern");
        return ESP_FAIL;
    }

    if (cJSON_IsNumber(brightness) && brightness->valueint > 0)
        pd_display_set_brightness((uint8_t)brightness->valueint);
    pd_display_test_start(p, -1);  /* run indefinitely until stopped */

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_test_stop(httpd_req_t *req)
{
    pd_display_test_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_test_panel_select(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *idx = cJSON_GetObjectItem(root, "panel_index");
    if (!cJSON_IsNumber(idx)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "panel_index required");
        return ESP_FAIL;
    }

    pd_display_test_set_layout_selected(idx->valueint);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t pd_content_register_http(httpd_handle_t server)
{
    httpd_uri_t list_uri = {
        .uri = "/api/content",
        .method = HTTP_GET,
        .handler = http_content_list
    };
    httpd_uri_t play_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = http_content_play
    };
    httpd_uri_t stop_uri = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = http_content_stop
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = http_content_status
    };
    httpd_uri_t log_uri = {
        .uri = "/api/log",
        .method = HTTP_GET,
        .handler = http_content_log
    };
    httpd_uri_t status_show_uri = {
        .uri = "/api/status/show",
        .method = HTTP_POST,
        .handler = http_status_show
    };
    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = http_content_upload
    };
    httpd_uri_t ota_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = http_ota_update
    };
    httpd_uri_t delete_uri = {
        .uri = "/api/content",
        .method = HTTP_DELETE,
        .handler = http_content_delete
    };
    httpd_uri_t delete_post_uri = {
        .uri = "/api/content/delete",
        .method = HTTP_POST,
        .handler = http_content_delete
    };
    httpd_uri_t rename_uri = {
        .uri = "/api/content/rename",
        .method = HTTP_POST,
        .handler = http_content_rename
    };
    httpd_uri_t set_meta_uri = {
        .uri = "/api/content/meta",
        .method = HTTP_POST,
        .handler = http_content_set_meta
    };
    httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = http_config_get
    };
    httpd_uri_t config_set_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = http_config_set
    };
    httpd_uri_t layout_get_uri = {
        .uri = "/api/config/layout",
        .method = HTTP_GET,
        .handler = http_layout_get
    };
    httpd_uri_t layout_set_uri = {
        .uri = "/api/config/layout",
        .method = HTTP_POST,
        .handler = http_layout_set
    };
    httpd_uri_t config_preview_uri = {
        .uri = "/api/config/preview",
        .method = HTTP_POST,
        .handler = http_config_preview
    };
    httpd_uri_t test_start_uri = {
        .uri = "/api/test/start",
        .method = HTTP_POST,
        .handler = http_test_start
    };
    httpd_uri_t test_stop_uri = {
        .uri = "/api/test/stop",
        .method = HTTP_POST,
        .handler = http_test_stop
    };
    httpd_uri_t test_panel_select_uri = {
        .uri = "/api/test/panel_select",
        .method = HTTP_POST,
        .handler = http_test_panel_select
    };

    httpd_register_uri_handler(server, &list_uri);
    httpd_register_uri_handler(server, &play_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &log_uri);
    httpd_register_uri_handler(server, &status_show_uri);
    httpd_register_uri_handler(server, &upload_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &delete_uri);
    httpd_register_uri_handler(server, &delete_post_uri);
    httpd_register_uri_handler(server, &rename_uri);
    httpd_register_uri_handler(server, &set_meta_uri);
    httpd_register_uri_handler(server, &config_get_uri);
    httpd_register_uri_handler(server, &config_set_uri);
    httpd_register_uri_handler(server, &layout_get_uri);
    httpd_register_uri_handler(server, &layout_set_uri);
    httpd_register_uri_handler(server, &config_preview_uri);
    httpd_register_uri_handler(server, &test_start_uri);
    httpd_register_uri_handler(server, &test_stop_uri);
    httpd_register_uri_handler(server, &test_panel_select_uri);

    ESP_LOGI(TAG, "HTTP endpoints registered");
    return ESP_OK;
}

void pd_content_show_source_status_for(int duration_ms)
{
    if (duration_ms <= 0) duration_ms = 5000;

    /* Only snapshot resume state if we're not already overlaying */
    if (status_overlay_until_us == 0) {
        status_resume_was_playing = content_playing;
        if (content_playing && content_current[0]) {
            strlcpy(status_resume_path, content_current, sizeof(status_resume_path));
        } else {
            status_resume_path[0] = '\0';
            status_resume_was_playing = false;
        }
    }

    /* pause any ongoing content rendering */
    content_playing = false;

    /* render immediately and set timer */
    pd_content_render_source_status();
    status_last_render_us = esp_timer_get_time();
    status_overlay_until_us = status_last_render_us + (int64_t)duration_ms * 1000;
    status_overlay_just_expired = false;
}

bool pd_content_status_overlay_just_expired(void)
{
    if (status_overlay_just_expired) {
        status_overlay_just_expired = false;
        return true;
    }
    return false;
}

bool pd_content_status_overlay_active(void)
{
    return status_overlay_until_us > 0;
}

void pd_content_render_source_status(void)
{
    int state = pd_discovery_get_state();
    pd_marquee_source_t *source = pd_discovery_get_active_source();
    
    if (!source || state == 0) {
        // No source connected - show searching message
        pd_display_render_no_source();
    } else {
        // Show source information
        pd_display_render_source_status(
            state,
            source->hostname,
            source->ip,
            source->es_version,
            source->browsing_events,
            source->launch_events,
            source->event_methods
        );
    }
}
