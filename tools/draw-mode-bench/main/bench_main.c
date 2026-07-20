/*
 * draw-mode-bench — A/B full-matrix vs bounds-limited panel updates.
 *
 * Uses the same pd-display / pd-config / pd-storage stack as production.
 * Does not modify the Control app. Flash production firmware afterward to restore.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "lodepng.h"
#include "pd-config.h"
#include "pd-display.h"
#include "pd-storage.h"

static const char *TAG = "draw-mode-bench";

#define BENCH_SECONDS       10
#define CONTENT_W           64
#define CONTENT_H           64
#define MAX_FRAMES          128
#define PAC_GHOST_DIR       "/pd/content/images/pac-ghost"

typedef struct {
    uint8_t *frames[MAX_FRAMES]; /* each CONTENT_W*CONTENT_H*3 RGB */
    int count;
    int w;
    int h;
} frame_set_t;

static pd_config_t s_cfg;

static uint8_t *alloc_rgb(int w, int h)
{
    size_t n = (size_t)w * (size_t)h * 3;
    uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

static void rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb, int w, int h)
{
    size_t pixels = (size_t)w * (size_t)h;
    for (size_t i = 0; i < pixels; i++) {
        uint8_t a = rgba[i * 4 + 3];
        if (a == 0) {
            rgb[i * 3 + 0] = 0;
            rgb[i * 3 + 1] = 0;
            rgb[i * 3 + 2] = 0;
        } else if (a == 255) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        } else {
            rgb[i * 3 + 0] = (uint8_t)((rgba[i * 4 + 0] * a) / 255);
            rgb[i * 3 + 1] = (uint8_t)((rgba[i * 4 + 1] * a) / 255);
            rgb[i * 3 + 2] = (uint8_t)((rgba[i * 4 + 2] * a) / 255);
        }
    }
}

static bool load_png_rgb(const char *path, uint8_t **out_rgb, int *out_w, int *out_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t *png = malloc((size_t)sz);
    if (!png) {
        fclose(f);
        return false;
    }
    if (fread(png, 1, (size_t)sz, f) != (size_t)sz) {
        free(png);
        fclose(f);
        return false;
    }
    fclose(f);

    unsigned w = 0, h = 0;
    unsigned char *rgba = NULL;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png, (size_t)sz);
    free(png);
    if (err || !rgba) {
        ESP_LOGW(TAG, "decode fail %s: %u", path, err);
        return false;
    }

    uint8_t *rgb = alloc_rgb((int)w, (int)h);
    if (!rgb) {
        free(rgba);
        return false;
    }
    rgba_to_rgb(rgba, rgb, (int)w, (int)h);
    free(rgba);
    *out_rgb = rgb;
    *out_w = (int)w;
    *out_h = (int)h;
    return true;
}

static void synthesize_sparse_frames(frame_set_t *set)
{
    set->w = CONTENT_W;
    set->h = CONTENT_H;
    set->count = 48;
    for (int i = 0; i < set->count; i++) {
        uint8_t *rgb = alloc_rgb(set->w, set->h);
        if (!rgb) {
            set->count = i;
            break;
        }
        memset(rgb, 0, (size_t)set->w * set->h * 3);
        int gx = 8 + (i * 2) % (set->w - 24);
        int gy = 16 + ((i / 2) % 2) * 8;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                int px = gx + x;
                int py = gy + y;
                if (px < 0 || py < 0 || px >= set->w || py >= set->h) continue;
                size_t idx = ((size_t)py * set->w + (size_t)px) * 3;
                rgb[idx + 0] = 40;
                rgb[idx + 1] = 180;
                rgb[idx + 2] = 255;
            }
        }
        /* single-pixel scanline accent */
        int ly = (i * 3) % set->h;
        for (int x = 0; x < set->w; x++) {
            size_t idx = ((size_t)ly * set->w + (size_t)x) * 3;
            rgb[idx + 0] = 20;
            rgb[idx + 1] = 20;
            rgb[idx + 2] = 40;
        }
        set->frames[i] = rgb;
    }
    ESP_LOGW(TAG, "using synthetic sparse frames (%d x %dx%d)", set->count, set->w, set->h);
}

static bool load_pac_ghost(frame_set_t *set)
{
    memset(set, 0, sizeof(*set));
    set->w = CONTENT_W;
    set->h = CONTENT_H;

    char path[192];
    for (int i = 1; i <= MAX_FRAMES; i++) {
        snprintf(path, sizeof(path), "%s/%04d.png", PAC_GHOST_DIR, i);
        uint8_t *rgb = NULL;
        int w = 0, h = 0;
        if (!load_png_rgb(path, &rgb, &w, &h)) {
            break;
        }
        if (w != CONTENT_W || h != CONTENT_H) {
            ESP_LOGW(TAG, "unexpected size %dx%d for %s — keeping anyway", w, h, path);
            set->w = w;
            set->h = h;
        }
        set->frames[set->count++] = rgb;
    }

    if (set->count <= 0) {
        synthesize_sparse_frames(set);
        return set->count > 0;
    }
    ESP_LOGI(TAG, "loaded %d pac-ghost frames %dx%d from LittleFS", set->count, set->w, set->h);
    return true;
}

static void center_blit_to_fb(uint8_t *fb, int dw, int dh,
                              const uint8_t *rgb, int sw, int sh)
{
    memset(fb, 0, (size_t)dw * dh * 3);
    int ox = (sw < dw) ? (dw - sw) / 2 : 0;
    int oy = (sh < dh) ? (dh - sh) / 2 : 0;
    int blit_w = (sw < dw) ? sw : dw;
    int blit_h = (sh < dh) ? sh : dh;
    for (int y = 0; y < blit_h; y++) {
        memcpy(fb + ((size_t)(oy + y) * dw + (size_t)ox) * 3,
               rgb + (size_t)y * sw * 3,
               (size_t)blit_w * 3);
    }
}

static float run_bench_full(const frame_set_t *set, uint8_t *fb, int dw, int dh, int seconds)
{
    ESP_LOGI(TAG, "=== BENCH full-matrix %ds (%dx%d canvas, content %dx%d) ===",
             seconds, dw, dh, set->w, set->h);
    pd_display_clear();
    int64_t t0 = esp_timer_get_time();
    int64_t end = t0 + (int64_t)seconds * 1000000;
    int frames = 0;
    int idx = 0;
    while (esp_timer_get_time() < end) {
        center_blit_to_fb(fb, dw, dh, set->frames[idx], set->w, set->h);
        pd_display_render_framebuf(fb);
        idx = (idx + 1) % set->count;
        frames++;
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    float fps = frames * 1000000.0f / (float)elapsed;
    ESP_LOGI(TAG, "bench: mode=full fps=%.2f frames=%d elapsed_ms=%lld",
             fps, frames, (long long)(elapsed / 1000));
    return fps;
}

static float run_bench_bounds(const frame_set_t *set, int dw, int dh, int seconds)
{
    ESP_LOGI(TAG, "=== BENCH bounds-limited %ds (push %dx%d only) ===",
             seconds, set->w, set->h);
    pd_display_clear(); /* margins once */
    int64_t t0 = esp_timer_get_time();
    int64_t end = t0 + (int64_t)seconds * 1000000;
    int frames = 0;
    int idx = 0;
    while (esp_timer_get_time() < end) {
        pd_display_render_rgb(set->frames[idx], set->w, set->h);
        idx = (idx + 1) % set->count;
        frames++;
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    float fps = frames * 1000000.0f / (float)elapsed;
    ESP_LOGI(TAG, "bench: mode=bounds fps=%.2f frames=%d elapsed_ms=%lld",
             fps, frames, (long long)(elapsed / 1000));
    (void)dw;
    (void)dh;
    return fps;
}

/* Erase axis-aligned old∖new (same algorithm as production pd-content). */
static void erase_rect_diff(int ox, int oy, int ow, int oh,
                            int nx, int ny, int nw, int nh)
{
    if (ow <= 0 || oh <= 0) return;
    int ox1 = ox + ow, oy1 = oy + oh;
    int nx1 = nx + nw, ny1 = ny + nh;
    int ix0 = ox > nx ? ox : nx;
    int iy0 = oy > ny ? oy : ny;
    int ix1 = ox1 < nx1 ? ox1 : nx1;
    int iy1 = oy1 < ny1 ? oy1 : ny1;
    if (ix0 >= ix1 || iy0 >= iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)oh, PD_COLOR_BLACK);
        return;
    }
    if (oy < iy0) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)(iy0 - oy), PD_COLOR_BLACK);
    }
    if (oy1 > iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy1, (uint16_t)ow, (uint16_t)(oy1 - iy1), PD_COLOR_BLACK);
    }
    if (ox < ix0) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy0, (uint16_t)(ix0 - ox), (uint16_t)(iy1 - iy0), PD_COLOR_BLACK);
    }
    if (ox1 > ix1) {
        pd_display_fill((uint16_t)ix1, (uint16_t)iy0, (uint16_t)(ox1 - ix1), (uint16_t)(iy1 - iy0), PD_COLOR_BLACK);
    }
}

/* Slide the sprite horizontally while animating frames — proves dirty-rect
 * erase leaves no trails (visual) and still beats full-matrix push (FPS). */
static float run_bench_slide(const frame_set_t *set, int dw, int dh, int seconds)
{
    ESP_LOGI(TAG, "=== BENCH slide dirty-rect %ds (erase old∖new + render_at) ===",
             seconds);
    pd_display_clear();
    int max_x = dw - set->w;
    if (max_x < 0) max_x = 0;
    int y = (set->h < dh) ? (dh - set->h) / 2 : 0;
    int x = 0;
    int dir = 4; /* px per frame */
    bool last_valid = false;
    int last_x = 0, last_y = 0, last_w = 0, last_h = 0;

    int64_t t0 = esp_timer_get_time();
    int64_t end = t0 + (int64_t)seconds * 1000000;
    int frames = 0;
    int idx = 0;
    while (esp_timer_get_time() < end) {
        if (last_valid && (last_x != x || last_y != y || last_w != set->w || last_h != set->h)) {
            erase_rect_diff(last_x, last_y, last_w, last_h, x, y, set->w, set->h);
        }
        pd_display_render_rgb_at(x, y, set->frames[idx], set->w, set->h);
        last_x = x;
        last_y = y;
        last_w = set->w;
        last_h = set->h;
        last_valid = true;

        x += dir;
        if (x >= max_x) {
            x = max_x;
            dir = -dir;
        } else if (x <= 0) {
            x = 0;
            dir = -dir;
        }
        idx = (idx + 1) % set->count;
        frames++;
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    float fps = frames * 1000000.0f / (float)elapsed;
    ESP_LOGI(TAG, "bench: mode=slide fps=%.2f frames=%d elapsed_ms=%lld",
             fps, frames, (long long)(elapsed / 1000));
    ESP_LOGI(TAG, "slide: visually confirm no trails across the canvas");
    return fps;
}

static void init_display_from_config(void)
{
    pd_display_config_t dc = {
        .panel_width = s_cfg.panel_width > 0 ? s_cfg.panel_width
                       : (s_cfg.matrix_width > 0 ? s_cfg.matrix_width : 64),
        .panel_height = s_cfg.panel_height > 0 ? s_cfg.panel_height
                        : (s_cfg.matrix_height > 0 ? s_cfg.matrix_height : 64),
        .panel_rows = s_cfg.panel_rows > 0 ? s_cfg.panel_rows : 1,
        .panel_cols = s_cfg.panel_cols > 0 ? s_cfg.panel_cols : 1,
        .chain_pattern = s_cfg.chain_pattern,
        .rotation_deg = s_cfg.panel_rotation_deg,
        .scan_wiring = s_cfg.scan_wiring,
        .color_order = s_cfg.color_order,
    };
    ESP_LOGI(TAG, "display config panel=%dx%d chain=%dx%d pat=%d rot=%d",
             dc.panel_width, dc.panel_height, dc.panel_rows, dc.panel_cols,
             dc.chain_pattern, dc.rotation_deg);
    if (pd_display_init(&dc) != ESP_OK) {
        ESP_LOGW(TAG, "display init failed — trying 64x32 safe mode");
        pd_display_config_t safe = {
            .panel_width = 64,
            .panel_height = 32,
            .panel_rows = 1,
            .panel_cols = 1,
            .chain_pattern = 0,
            .rotation_deg = 0,
            .scan_wiring = 0,
            .color_order = 0,
        };
        ESP_ERROR_CHECK(pd_display_init(&safe));
    }
    pd_display_set_brightness(80);
}

void app_main(void)
{
    ESP_LOGI(TAG, "draw-mode-bench starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    pd_storage_config_t storage_config = { .base_path = "/pd" };
    ESP_ERROR_CHECK(pd_storage_init(&storage_config));

    memset(&s_cfg, 0, sizeof(s_cfg));
    pd_config_init(&s_cfg);
    pd_config_load(&s_cfg);
    pd_config_set_active(&s_cfg);

    init_display_from_config();
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    ESP_LOGI(TAG, "canvas %dx%d", dw, dh);

    frame_set_t set;
    if (!load_pac_ghost(&set) || set.count <= 0) {
        ESP_LOGE(TAG, "no frames available — abort");
        return;
    }

    uint8_t *fb = alloc_rgb(dw, dh);
    if (!fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    float fps_full = run_bench_full(&set, fb, dw, dh, BENCH_SECONDS);
    vTaskDelay(pdMS_TO_TICKS(500));
    float fps_bounds = run_bench_bounds(&set, dw, dh, BENCH_SECONDS);
    vTaskDelay(pdMS_TO_TICKS(500));
    float fps_slide = run_bench_slide(&set, dw, dh, BENCH_SECONDS);

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "RESULT full=%.2f fps  bounds=%.2f fps  slide=%.2f fps",
             fps_full, fps_bounds, fps_slide);
    ESP_LOGI(TAG, "  bounds/full=%.2fx  slide/full=%.2fx",
             (fps_full > 0.01f) ? (fps_bounds / fps_full) : 0.0f,
             (fps_full > 0.01f) ? (fps_slide / fps_full) : 0.0f);
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Reflash production firmware when done:");
    ESP_LOGI(TAG, "  cd <repo> && idf.py -p /dev/cu.usbmodem101 flash");

    /* Idle: keep showing last slide frame */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
