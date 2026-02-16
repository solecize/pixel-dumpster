#include "pd-content.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lodepng.h"
#include "pd-display.h"
#include "pd-transition.h"

static const char *TAG = "pd-content";

static char content_base[PD_CONTENT_MAX_PATH] = "";
static bool content_playing = false;
static bool content_is_seq = false;
static char content_current[PD_CONTENT_MAX_PATH] = "";
static int  content_frame = 0;
static int  content_total_frames = 0;
static int  content_fps = 12;
static bool content_loop = true;
static int64_t content_last_frame_us = 0;
static char content_frame_pattern[64] = "%04d.png";  /* frame filename pattern */
static int  content_frame_start = 1;                  /* first frame index (0 or 1) */

/* ---- transition state ---- */
static pd_transition_t *content_transition = NULL;
static pd_framebuf_t   *content_fb = NULL;  /* current display framebuffer */

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
};

/* ---- compositing state ---- */
static uint8_t *cached_background_rgb = NULL;  /* cached background image (RGB) */
static uint8_t *cached_overlay_rgba = NULL;    /* cached overlay image (RGBA) - only for static */
static char cached_bg_path[PD_CONTENT_MAX_PATH] = "";
static char cached_overlay_path[PD_CONTENT_MAX_PATH] = "";
static int cached_width = 0;
static int cached_height = 0;

/* animated overlay state */
static bool overlay_is_seq = false;
static int overlay_fps = 12;
static int overlay_frame = 0;
static int overlay_frame_start = 0;
static int overlay_total_frames = 0;
static char overlay_frame_pattern[64] = "%04d.png";
static char overlay_base_path[PD_CONTENT_MAX_PATH] = "";
static int64_t overlay_last_frame_us = 0;

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

static int count_sequence_frames(const char *dir_path)
{
    int count = 0;
    char path[PD_CONTENT_MAX_PATH];
    for (int i = 1; i <= 9999; i++) {
        snprintf(path, sizeof(path), "%s/%04d.png", dir_path, i);
        if (!path_exists(path)) break;
        count = i;
    }
    return count;
}

static bool load_sequence_meta(const char *dir_path, int *fps, bool *loop_out, int *frame_count,
                               char *pattern_out, size_t pattern_size, int *start_out)
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

    if (fps && cJSON_IsNumber(j_fps)) *fps = j_fps->valueint;
    if (loop_out) *loop_out = cJSON_IsBool(j_loop) ? cJSON_IsTrue(j_loop) : true;
    if (frame_count) {
        if (cJSON_IsNumber(j_frames) && j_frames->valueint > 0) {
            *frame_count = j_frames->valueint;
        } else {
            *frame_count = count_sequence_frames(dir_path);
        }
    }
    if (pattern_out && cJSON_IsString(j_pattern)) {
        strlcpy(pattern_out, j_pattern->valuestring, pattern_size);
    }
    if (start_out) {
        *start_out = cJSON_IsNumber(j_start) ? j_start->valueint : 1;
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
    ESP_LOGI(TAG, "config loaded: mode=%d baseline=%s dur=%d",
             content_config.trans_mode, content_config.trans_baseline,
             content_config.trans_duration_ms);
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
    cJSON_AddStringToObject(disp, "background", content_config.background);
    cJSON_AddStringToObject(disp, "overlay", content_config.overlay);

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

static void update_compositing_cache(int width, int height)
{
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
                                   &overlay_frame_start);
                if (overlay_total_frames == 0) {
                    overlay_total_frames = count_sequence_frames(full);
                }
                overlay_frame = overlay_frame_start;
                overlay_last_frame_us = 0;
                ESP_LOGI(TAG, "animated overlay: %s (%d frames @ %d fps)",
                         ov, overlay_total_frames, overlay_fps);
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
}

static uint8_t *composite_frame(uint8_t *content_rgba, int width, int height)
{
    update_compositing_cache(width, height);

    size_t pixels = width * height;
    uint8_t *rgb = malloc(pixels * 3);
    if (!rgb) return NULL;

    /* layer 1: background (or black if none) */
    if (cached_background_rgb) {
        memcpy(rgb, cached_background_rgb, pixels * 3);
    } else {
        memset(rgb, 0, pixels * 3);
    }

    /* layer 2: content with alpha blending */
    for (size_t i = 0; i < pixels; i++) {
        uint8_t cr = content_rgba[i * 4 + 0];
        uint8_t cg = content_rgba[i * 4 + 1];
        uint8_t cb = content_rgba[i * 4 + 2];
        uint8_t ca = content_rgba[i * 4 + 3];
        uint8_t br = rgb[i * 3 + 0];
        uint8_t bg = rgb[i * 3 + 1];
        uint8_t bb = rgb[i * 3 + 2];
        rgb[i * 3 + 0] = (cr * ca + br * (255 - ca)) / 255;
        rgb[i * 3 + 1] = (cg * ca + bg * (255 - ca)) / 255;
        rgb[i * 3 + 2] = (cb * ca + bb * (255 - ca)) / 255;
    }

    /* layer 3: overlay with alpha blending */
    uint8_t *overlay_rgba = NULL;
    if (overlay_is_seq && overlay_total_frames > 0) {
        /* load current frame of animated overlay */
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
        }
    } else if (cached_overlay_rgba) {
        overlay_rgba = cached_overlay_rgba;
    }

    if (overlay_rgba) {
        for (size_t i = 0; i < pixels; i++) {
            uint8_t or_ = overlay_rgba[i * 4 + 0];
            uint8_t og = overlay_rgba[i * 4 + 1];
            uint8_t ob = overlay_rgba[i * 4 + 2];
            uint8_t oa = overlay_rgba[i * 4 + 3];
            uint8_t br = rgb[i * 3 + 0];
            uint8_t bg = rgb[i * 3 + 1];
            uint8_t bb = rgb[i * 3 + 2];
            rgb[i * 3 + 0] = (or_ * oa + br * (255 - oa)) / 255;
            rgb[i * 3 + 1] = (og * oa + bg * (255 - oa)) / 255;
            rgb[i * 3 + 2] = (ob * oa + bb * (255 - oa)) / 255;
        }
        /* free if we loaded it dynamically (not the cached static one) */
        if (overlay_rgba != cached_overlay_rgba) {
            free(overlay_rgba);
        }
    }

    return rgb;
}

static uint8_t *decode_png_file(const char *path, unsigned *w, unsigned *h)
{
    uint8_t *rgba = decode_png_rgba(path, w, h);
    if (!rgba) return NULL;

    uint8_t *rgb = composite_frame(rgba, (int)*w, (int)*h);
    free(rgba);
    return rgb;
}

/* ---- public API ---- */

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
        if (!content_transition || !content_fb) {
            ESP_LOGW(TAG, "failed to allocate transition engine");
        }
    }

    load_config();

    ESP_LOGI(TAG, "content initialized at %s", content_base);
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
            load_sequence_meta(full, &e->fps, NULL, &e->frame_count, NULL, 0, NULL);
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
        load_sequence_meta(full_path, NULL, NULL, NULL, pattern, sizeof(pattern), &start);

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
        uint8_t *rgb = decode_png_file(full_path, &w, &h);
        if (!rgb) return false;
        pd_framebuf_blit_rgb(fb, rgb, (int)w, (int)h);
        free(rgb);
        return true;
    }
    return false;
}

/* helper: set up playback state for a content path */
static esp_err_t content_setup_playback(const char *path, const char *full)
{
    if (path_is_dir(full)) {
        int fps = 12;
        bool loop = true;
        int frames = 0;
        char pattern[64] = "%04d.png";
        int start = 1;
        load_sequence_meta(full, &fps, &loop, &frames, pattern, sizeof(pattern), &start);

        if (frames == 0) {
            ESP_LOGW(TAG, "no frames in %s", full);
            return ESP_ERR_NOT_FOUND;
        }

        strlcpy(content_current, full, sizeof(content_current));
        strlcpy(content_frame_pattern, pattern, sizeof(content_frame_pattern));
        content_frame_start = start;
        content_is_seq = true;
        content_fps = fps > 0 ? fps : 12;
        content_loop = loop;
        content_total_frames = frames;
        content_frame = start;
        content_playing = true;
        content_last_frame_us = 0;

        ESP_LOGI(TAG, "playing sequence: %s (%d frames @ %d fps, pattern=%s, start=%d)",
                 path, frames, fps, pattern, start);
    } else if (path_exists(full) && is_png(full)) {
        strlcpy(content_current, full, sizeof(content_current));
        content_is_seq = false;
        content_playing = true;
        content_frame = 0;
        content_total_frames = 1;

        ESP_LOGI(TAG, "displaying static: %s", path);
    } else {
        ESP_LOGW(TAG, "content not found: %s", full);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t pd_content_play(const char *path)
{
    /* stop current playback immediately so tick() won't block us */
    content_playing = false;

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    /* decode first frame into content_fb and display it */
    if (content_fb) {
        if (!content_decode_first_frame(full, content_fb)) {
            return ESP_ERR_NOT_FOUND;
        }
        pd_display_render_framebuf(content_fb->data);
    } else {
        /* fallback: no framebuffer, render directly */
        unsigned w, h;
        uint8_t *rgb = NULL;
        if (path_is_dir(full)) {
            char pattern[64] = "%04d.png";
            int start = 1;
            load_sequence_meta(full, NULL, NULL, NULL, pattern, sizeof(pattern), &start);

            char fp[PD_CONTENT_MAX_PATH];
            snprintf(fp, sizeof(fp), "%s/", full);
            size_t base_len = strlen(fp);
            snprintf(fp + base_len, sizeof(fp) - base_len, pattern, start);
            rgb = decode_png_file(fp, &w, &h);
        } else {
            rgb = decode_png_file(full, &w, &h);
        }
        if (!rgb) return ESP_ERR_NOT_FOUND;
        pd_display_render_rgb(rgb, (int)w, (int)h);
        free(rgb);
    }

    return content_setup_playback(path, full);
}

esp_err_t pd_content_play_with_transition(const char *path, const char *transition,
                                          int duration_ms)
{
    if (!content_transition || !content_fb) {
        return pd_content_play(path);
    }

    /* stop current playback immediately so tick() won't block us */
    content_playing = false;

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    pd_transition_type_t type = pd_transition_type_from_name(transition);
    if (type == PD_TRANS_NONE) {
        return pd_content_play(path);
    }

    /* capture current display as "from" */
    pd_framebuf_copy(content_transition->from, content_fb);

    /* decode new content into "to" */
    if (!content_decode_first_frame(full, content_transition->to)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* start the transition */
    pd_transition_start(content_transition, type, duration_ms);

    return content_setup_playback(path, full);
}

esp_err_t pd_content_stop(void)
{
    content_playing = false;
    content_current[0] = '\0';
    pd_display_clear();
    ESP_LOGI(TAG, "playback stopped");
    return ESP_OK;
}

pd_content_status_t pd_content_get_status(void)
{
    pd_content_status_t s = {0};
    s.playing = content_playing;
    if (content_playing) {
        strlcpy(s.current_path, content_current, sizeof(s.current_path));
        s.is_sequence = content_is_seq;
        s.current_frame = content_frame;
        s.total_frames = content_total_frames;
        s.fps = content_fps;
    }
    return s;
}

void pd_content_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* drive active transition */
    if (content_transition && pd_transition_is_active(content_transition)) {
        bool still_going = pd_transition_tick(content_transition);
        pd_display_render_framebuf(content_transition->out->data);
        if (!still_going) {
            /* transition finished — update content_fb to final state */
            pd_framebuf_copy(content_fb, content_transition->to);
        }
        return;  /* don't advance sequence frames during transition */
    }

    /* advance animated overlay frame independently */
    if (overlay_is_seq && overlay_total_frames > 0) {
        int64_t overlay_interval = 1000000 / overlay_fps;
        if (overlay_last_frame_us == 0) {
            overlay_last_frame_us = now;
        } else if ((now - overlay_last_frame_us) >= overlay_interval) {
            overlay_last_frame_us = now;
            int next_ov = overlay_frame + 1;
            int last_ov = overlay_frame_start + overlay_total_frames - 1;
            if (next_ov > last_ov) {
                next_ov = overlay_frame_start;  /* loop overlay */
            }
            overlay_frame = next_ov;
        }
    }

    if (!content_playing || !content_is_seq) return;

    int64_t frame_interval = 1000000 / content_fps;

    if (content_last_frame_us == 0) {
        content_last_frame_us = now;
        return;
    }

    if ((now - content_last_frame_us) < frame_interval) return;

    content_last_frame_us = now;
    int next = content_frame + 1;

    int last_frame = content_frame_start + content_total_frames - 1;
    if (next > last_frame) {
        if (content_loop) {
            next = content_frame_start;
        } else {
            return;
        }
    }

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
        if (content_fb) {
            pd_framebuf_blit_rgb(content_fb, rgb, (int)w, (int)h);
            pd_display_render_framebuf(content_fb->data);
        } else {
            pd_display_render_rgb(rgb, (int)w, (int)h);
        }
        free(rgb);
        content_frame = next;
    }
}

/* ---- file storage ---- */

esp_err_t pd_content_store_file(const char *rel_path, const uint8_t *data, size_t len)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);

    /* ensure parent directories exist */
    char dir[PD_CONTENT_MAX_PATH];
    strlcpy(dir, full, sizeof(dir));
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

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    fwrite(data, 1, len, f);
    fclose(f);

    ESP_LOGI(TAG, "stored %s (%d bytes)", rel_path, (int)len);
    return ESP_OK;
}

esp_err_t pd_content_delete_file(const char *rel_path)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);

    if (remove(full) != 0) {
        ESP_LOGW(TAG, "cannot delete %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted %s", rel_path);
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

    cJSON *j_trans = cJSON_GetObjectItem(root, "transition");
    cJSON *j_dur   = cJSON_GetObjectItem(root, "duration_ms");

    esp_err_t err;
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
    if (type == PD_TRANS_NONE) {
        err = pd_content_play(path->valuestring);
    } else {
        err = pd_content_play_with_transition(path->valuestring, trans_name, dur);
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "content not found");
        return ESP_FAIL;
    }

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

static esp_err_t http_content_status(httpd_req_t *req)
{
    pd_content_status_t s = pd_content_get_status();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", s.playing);
    if (s.playing) {
        cJSON_AddStringToObject(root, "path", s.current_path);
        cJSON_AddBoolToObject(root, "sequence", s.is_sequence);
        cJSON_AddNumberToObject(root, "frame", s.current_frame);
        cJSON_AddNumberToObject(root, "total_frames", s.total_frames);
        cJSON_AddNumberToObject(root, "fps", s.fps);
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
    cJSON_AddStringToObject(disp, "background", content_config.background);
    cJSON_AddStringToObject(disp, "overlay", content_config.overlay);

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
        if (cJSON_IsString(bg))
            strlcpy(content_config.background, bg->valuestring,
                    sizeof(content_config.background));
        cJSON *ov = cJSON_GetObjectItem(disp, "overlay");
        if (cJSON_IsString(ov))
            strlcpy(content_config.overlay, ov->valuestring,
                    sizeof(content_config.overlay));
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

    /* optionally save to disk */
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

static esp_err_t http_content_upload(httpd_req_t *req)
{
    /* path comes from query string: ?path=images/foo.png */
    char query[256] = "";
    char rel_path[PD_CONTENT_MAX_PATH] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", rel_path, sizeof(rel_path));
    }

    if (rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total <= 0 || total > 512 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    uint8_t *data = malloc(total);
    if (!data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, (char *)data + received, total - received);
        if (ret <= 0) {
            free(data);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }

    esp_err_t err = pd_content_store_file(rel_path, data, total);
    free(data);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_FAIL;
    }

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
    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = http_content_upload
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

    httpd_register_uri_handler(server, &list_uri);
    httpd_register_uri_handler(server, &play_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &upload_uri);
    httpd_register_uri_handler(server, &config_get_uri);
    httpd_register_uri_handler(server, &config_set_uri);

    ESP_LOGI(TAG, "HTTP endpoints registered");
    return ESP_OK;
}
