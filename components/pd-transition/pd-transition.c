#include "pd-transition.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "pd-transition";

/* ================================================================
 * Framebuffer
 * ================================================================ */

pd_framebuf_t *pd_framebuf_create(int width, int height)
{
    pd_framebuf_t *fb = calloc(1, sizeof(pd_framebuf_t));
    if (!fb) return NULL;
    fb->width = width;
    fb->height = height;
    fb->data = calloc(1, width * height * 3);
    if (!fb->data) { free(fb); return NULL; }
    return fb;
}

void pd_framebuf_destroy(pd_framebuf_t *fb)
{
    if (!fb) return;
    free(fb->data);
    free(fb);
}

void pd_framebuf_clear(pd_framebuf_t *fb)
{
    if (fb && fb->data) {
        memset(fb->data, 0, fb->width * fb->height * 3);
    }
}

void pd_framebuf_copy(pd_framebuf_t *dst, const pd_framebuf_t *src)
{
    if (!dst || !src || !dst->data || !src->data) return;
    int bytes = dst->width * dst->height * 3;
    int src_bytes = src->width * src->height * 3;
    memcpy(dst->data, src->data, bytes < src_bytes ? bytes : src_bytes);
}

void pd_framebuf_set_pixel(pd_framebuf_t *fb, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (!fb || x < 0 || y < 0 || x >= fb->width || y >= fb->height) return;
    int idx = (y * fb->width + x) * 3;
    fb->data[idx]     = r;
    fb->data[idx + 1] = g;
    fb->data[idx + 2] = b;
}

void pd_framebuf_get_pixel(const pd_framebuf_t *fb, int x, int y,
                           uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!fb || x < 0 || y < 0 || x >= fb->width || y >= fb->height) {
        *r = *g = *b = 0;
        return;
    }
    int idx = (y * fb->width + x) * 3;
    *r = fb->data[idx];
    *g = fb->data[idx + 1];
    *b = fb->data[idx + 2];
}

void pd_framebuf_blit_rgb(pd_framebuf_t *fb, const uint8_t *rgb,
                          int img_w, int img_h)
{
    if (!fb || !rgb) return;
    int dw = fb->width;
    int dh = fb->height;
    int ox = (img_w < dw) ? (dw - img_w) / 2 : 0;
    int oy = (img_h < dh) ? (dh - img_h) / 2 : 0;
    int blit_w = (img_w < dw) ? img_w : dw;
    int blit_h = (img_h < dh) ? img_h : dh;

    /* clear first so edges are black if image is smaller */
    pd_framebuf_clear(fb);

    for (int y = 0; y < blit_h; y++) {
        int src_row = y * img_w * 3;
        int dst_row = ((oy + y) * dw + ox) * 3;
        memcpy(fb->data + dst_row, rgb + src_row, blit_w * 3);
    }
}

/* ================================================================
 * Transition name mapping
 * ================================================================ */

static const char *transition_names[PD_TRANS_COUNT] = {
    [PD_TRANS_NONE]        = "none",
    [PD_TRANS_WIPE_LEFT]   = "wipe-left",
    [PD_TRANS_WIPE_RIGHT]  = "wipe-right",
    [PD_TRANS_WIPE_UP]     = "wipe-up",
    [PD_TRANS_WIPE_DOWN]   = "wipe-down",
    [PD_TRANS_SLIDE_LEFT]  = "slide-left",
    [PD_TRANS_SLIDE_RIGHT] = "slide-right",
    [PD_TRANS_SLIDE_UP]    = "slide-up",
    [PD_TRANS_SLIDE_DOWN]  = "slide-down",
    [PD_TRANS_ROLL_UP]     = "roll-up",
    [PD_TRANS_ROLL_DOWN]   = "roll-down",
    [PD_TRANS_SPLIT_H]     = "split-h",
    [PD_TRANS_SPLIT_V]     = "split-v",
    [PD_TRANS_FADE]        = "fade",
    [PD_TRANS_BLOCK_BUILD] = "block-build",
    [PD_TRANS_PIXEL_BUILD] = "pixel-build",
    [PD_TRANS_WIPE_DIAG_TL] = "wipe-diag-tl",
    [PD_TRANS_WIPE_DIAG_TR] = "wipe-diag-tr",
    [PD_TRANS_WIPE_DIAG_BL] = "wipe-diag-bl",
    [PD_TRANS_WIPE_DIAG_BR] = "wipe-diag-br",
    [PD_TRANS_SPLIT_DIAG]   = "split-diag",
    [PD_TRANS_ZOOM_IN]      = "zoom-in",
    [PD_TRANS_ZOOM_OUT]     = "zoom-out",
    [PD_TRANS_FLIP_H]       = "flip-h",
    [PD_TRANS_FLIP_V]       = "flip-v",
};

const char *pd_transition_type_name(pd_transition_type_t type)
{
    if (type >= 0 && type < PD_TRANS_COUNT) return transition_names[type];
    return "none";
}

pd_transition_type_t pd_transition_type_from_name(const char *name)
{
    if (!name) return PD_TRANS_NONE;
    for (int i = 0; i < PD_TRANS_COUNT; i++) {
        if (strcmp(name, transition_names[i]) == 0) return (pd_transition_type_t)i;
    }
    return PD_TRANS_NONE;
}

/* ================================================================
 * Shuffle generation (for pixel-build and block-build)
 * ================================================================ */

static void fisher_yates_shuffle(uint16_t *arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        uint16_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

static void generate_pixel_shuffle(pd_transition_t *t)
{
    int n = t->out->width * t->out->height;
    if (t->shuffle && t->shuffle_count == n) return;
    free(t->shuffle);
    t->shuffle = malloc(n * sizeof(uint16_t));
    if (!t->shuffle) { t->shuffle_count = 0; return; }
    t->shuffle_count = n;
    for (int i = 0; i < n; i++) t->shuffle[i] = (uint16_t)i;
    fisher_yates_shuffle(t->shuffle, n);
}

static void generate_block_shuffle(pd_transition_t *t)
{
    int bw = (t->out->width + 3) / 4;
    int bh = (t->out->height + 3) / 4;
    int n = bw * bh;
    free(t->shuffle);
    t->shuffle = malloc(n * sizeof(uint16_t));
    if (!t->shuffle) { t->shuffle_count = 0; return; }
    t->shuffle_count = n;
    for (int i = 0; i < n; i++) t->shuffle[i] = (uint16_t)i;
    fisher_yates_shuffle(t->shuffle, n);
}

/* ================================================================
 * Transition engine
 * ================================================================ */

pd_transition_t *pd_transition_create(int width, int height)
{
    pd_transition_t *t = calloc(1, sizeof(pd_transition_t));
    if (!t) return NULL;
    t->from = pd_framebuf_create(width, height);
    t->to   = pd_framebuf_create(width, height);
    t->out  = pd_framebuf_create(width, height);
    if (!t->from || !t->to || !t->out) {
        pd_transition_destroy(t);
        return NULL;
    }
    ESP_LOGI(TAG, "transition engine created (%dx%d, %d bytes per fb)",
             width, height, width * height * 3);
    return t;
}

void pd_transition_destroy(pd_transition_t *t)
{
    if (!t) return;
    pd_framebuf_destroy(t->from);
    pd_framebuf_destroy(t->to);
    pd_framebuf_destroy(t->out);
    free(t->shuffle);
    free(t);
}

void pd_transition_start(pd_transition_t *t, pd_transition_type_t type,
                         int duration_ms)
{
    if (!t) return;
    t->type = type;
    t->duration_ms = duration_ms > 0 ? duration_ms : 500;
    t->start_us = esp_timer_get_time();
    t->active = true;

    if (type == PD_TRANS_PIXEL_BUILD) {
        generate_pixel_shuffle(t);
    } else if (type == PD_TRANS_BLOCK_BUILD) {
        generate_block_shuffle(t);
    }

    ESP_LOGI(TAG, "transition start: %s (%d ms)",
             pd_transition_type_name(type), t->duration_ms);
}

float pd_transition_progress(const pd_transition_t *t)
{
    if (!t || !t->active) return 1.0f;
    int64_t elapsed = esp_timer_get_time() - t->start_us;
    float p = (float)elapsed / ((float)t->duration_ms * 1000.0f);
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p;
}

bool pd_transition_is_active(const pd_transition_t *t)
{
    return t && t->active;
}

/* ---- individual transition renderers ---- */

static void render_none(pd_transition_t *t, float p)
{
    (void)p;
    pd_framebuf_copy(t->out, t->to);
}

static void render_wipe_left(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int cut = (int)(p * w);
    for (int y = 0; y < h; y++) {
        int row = y * w * 3;
        if (cut > 0)
            memcpy(t->out->data + row, t->to->data + row, cut * 3);
        if (cut < w)
            memcpy(t->out->data + row + cut * 3, t->from->data + row + cut * 3, (w - cut) * 3);
    }
}

static void render_wipe_right(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int cut = (int)(p * w);
    for (int y = 0; y < h; y++) {
        int row = y * w * 3;
        int start = w - cut;
        if (start > 0)
            memcpy(t->out->data + row, t->from->data + row, start * 3);
        if (cut > 0)
            memcpy(t->out->data + row + start * 3, t->to->data + row + start * 3, cut * 3);
    }
}

static void render_wipe_up(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int cut = (int)(p * h);
    int row_bytes = w * 3;
    for (int y = 0; y < cut; y++) {
        memcpy(t->out->data + y * row_bytes, t->to->data + y * row_bytes, row_bytes);
    }
    for (int y = cut; y < h; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + y * row_bytes, row_bytes);
    }
}

static void render_wipe_down(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int cut = (int)(p * h);
    int row_bytes = w * 3;
    int start = h - cut;
    for (int y = 0; y < start; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + y * row_bytes, row_bytes);
    }
    for (int y = start; y < h; y++) {
        memcpy(t->out->data + y * row_bytes, t->to->data + y * row_bytes, row_bytes);
    }
}

static void render_slide_left(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * w);
    for (int y = 0; y < h; y++) {
        int row = y * w * 3;
        /* old content slides left */
        if (w - shift > 0)
            memcpy(t->out->data + row, t->from->data + row + shift * 3, (w - shift) * 3);
        /* new content enters from right */
        if (shift > 0)
            memcpy(t->out->data + row + (w - shift) * 3, t->to->data + row + (w - shift) * 3, shift * 3);
    }
}

static void render_slide_right(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * w);
    for (int y = 0; y < h; y++) {
        int row = y * w * 3;
        /* new content enters from left */
        if (shift > 0)
            memcpy(t->out->data + row, t->to->data + row, shift * 3);
        /* old content slides right */
        if (w - shift > 0)
            memcpy(t->out->data + row + shift * 3, t->from->data + row, (w - shift) * 3);
    }
}

static void render_slide_up(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * h);
    int row_bytes = w * 3;
    for (int y = 0; y < h; y++) {
        int src_y = y + shift;
        if (src_y < h) {
            memcpy(t->out->data + y * row_bytes, t->from->data + src_y * row_bytes, row_bytes);
        } else {
            int to_y = src_y - h;
            if (to_y < h)
                memcpy(t->out->data + y * row_bytes, t->to->data + to_y * row_bytes, row_bytes);
        }
    }
}

static void render_slide_down(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * h);
    int row_bytes = w * 3;
    for (int y = 0; y < h; y++) {
        int src_y = y - shift;
        if (src_y >= 0) {
            memcpy(t->out->data + y * row_bytes, t->from->data + src_y * row_bytes, row_bytes);
        } else {
            int to_y = h + src_y;
            if (to_y >= 0 && to_y < h)
                memcpy(t->out->data + y * row_bytes, t->to->data + to_y * row_bytes, row_bytes);
        }
    }
}

static void render_roll_up(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * h);
    int row_bytes = w * 3;
    /* old content scrolls up, new content appears from bottom */
    for (int y = 0; y < h - shift; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + (y + shift) * row_bytes, row_bytes);
    }
    for (int y = h - shift; y < h; y++) {
        memcpy(t->out->data + y * row_bytes, t->to->data + y * row_bytes, row_bytes);
    }
}

static void render_roll_down(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int shift = (int)(p * h);
    int row_bytes = w * 3;
    /* new content appears from top */
    for (int y = 0; y < shift; y++) {
        memcpy(t->out->data + y * row_bytes, t->to->data + y * row_bytes, row_bytes);
    }
    for (int y = shift; y < h; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + (y - shift) * row_bytes, row_bytes);
    }
}

static void render_split_h(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int half = h / 2;
    int shift = (int)(p * half);
    int row_bytes = w * 3;

    pd_framebuf_copy(t->out, t->to);

    /* top half slides up */
    for (int y = 0; y < half - shift; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + (y + shift) * row_bytes, row_bytes);
    }
    /* bottom half slides down */
    for (int y = half + shift; y < h; y++) {
        memcpy(t->out->data + y * row_bytes, t->from->data + (y - shift) * row_bytes, row_bytes);
    }
}

static void render_split_v(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int half = w / 2;
    int shift = (int)(p * half);

    pd_framebuf_copy(t->out, t->to);

    for (int y = 0; y < h; y++) {
        int row = y * w * 3;
        /* left half slides left */
        int left_visible = half - shift;
        if (left_visible > 0)
            memcpy(t->out->data + row, t->from->data + row + shift * 3, left_visible * 3);
        /* right half slides right */
        int right_start = half + shift;
        if (right_start < w)
            memcpy(t->out->data + row + right_start * 3,
                   t->from->data + row + half * 3, (w - right_start) * 3);
    }
}

static void render_fade(pd_transition_t *t, float p)
{
    int n = t->out->width * t->out->height * 3;
    const uint8_t *from = t->from->data;
    const uint8_t *to   = t->to->data;
    uint8_t *out = t->out->data;
    /* fixed-point blend: 0..256 */
    int alpha = (int)(p * 256.0f);
    if (alpha > 256) alpha = 256;
    int inv = 256 - alpha;
    for (int i = 0; i < n; i++) {
        out[i] = (uint8_t)((from[i] * inv + to[i] * alpha) >> 8);
    }
}

static void render_block_build(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    int bw = (w + 3) / 4;   /* blocks are 4x4 pixels */
    int bh = (h + 3) / 4;
    int total_blocks = bw * bh;
    int reveal = (int)(p * total_blocks);

    pd_framebuf_copy(t->out, t->from);

    if (!t->shuffle || t->shuffle_count != total_blocks) return;

    for (int i = 0; i < reveal && i < total_blocks; i++) {
        int block_idx = t->shuffle[i];
        int bx = (block_idx % bw) * 4;
        int by = (block_idx / bw) * 4;
        for (int dy = 0; dy < 4 && (by + dy) < h; dy++) {
            int row_dst = ((by + dy) * w + bx) * 3;
            int copy_w = 4;
            if (bx + copy_w > w) copy_w = w - bx;
            memcpy(t->out->data + row_dst, t->to->data + row_dst, copy_w * 3);
        }
    }
}

static void render_pixel_build(pd_transition_t *t, float p)
{
    int total = t->out->width * t->out->height;
    int reveal = (int)(p * total);

    pd_framebuf_copy(t->out, t->from);

    if (!t->shuffle || t->shuffle_count != total) return;

    for (int i = 0; i < reveal && i < total; i++) {
        int px = t->shuffle[i];
        int idx = px * 3;
        t->out->data[idx]     = t->to->data[idx];
        t->out->data[idx + 1] = t->to->data[idx + 1];
        t->out->data[idx + 2] = t->to->data[idx + 2];
    }
}

/* ---- diagonal wipes ---- */

static void render_wipe_diag_tl(pd_transition_t *t, float p)
{
    /* diagonal wipe from top-left corner: pixels where (x/w + y/h) < 2*p are "to" */
    int w = t->out->width;
    int h = t->out->height;
    float threshold = 2.0f * p;
    for (int y = 0; y < h; y++) {
        float fy = (float)y / (float)h;
        for (int x = 0; x < w; x++) {
            float fx = (float)x / (float)w;
            int idx = (y * w + x) * 3;
            if (fx + fy < threshold) {
                t->out->data[idx]     = t->to->data[idx];
                t->out->data[idx + 1] = t->to->data[idx + 1];
                t->out->data[idx + 2] = t->to->data[idx + 2];
            } else {
                t->out->data[idx]     = t->from->data[idx];
                t->out->data[idx + 1] = t->from->data[idx + 1];
                t->out->data[idx + 2] = t->from->data[idx + 2];
            }
        }
    }
}

static void render_wipe_diag_tr(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    float threshold = 2.0f * p;
    for (int y = 0; y < h; y++) {
        float fy = (float)y / (float)h;
        for (int x = 0; x < w; x++) {
            float fx = (float)(w - 1 - x) / (float)w;
            int idx = (y * w + x) * 3;
            if (fx + fy < threshold) {
                t->out->data[idx]     = t->to->data[idx];
                t->out->data[idx + 1] = t->to->data[idx + 1];
                t->out->data[idx + 2] = t->to->data[idx + 2];
            } else {
                t->out->data[idx]     = t->from->data[idx];
                t->out->data[idx + 1] = t->from->data[idx + 1];
                t->out->data[idx + 2] = t->from->data[idx + 2];
            }
        }
    }
}

static void render_wipe_diag_bl(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    float threshold = 2.0f * p;
    for (int y = 0; y < h; y++) {
        float fy = (float)(h - 1 - y) / (float)h;
        for (int x = 0; x < w; x++) {
            float fx = (float)x / (float)w;
            int idx = (y * w + x) * 3;
            if (fx + fy < threshold) {
                t->out->data[idx]     = t->to->data[idx];
                t->out->data[idx + 1] = t->to->data[idx + 1];
                t->out->data[idx + 2] = t->to->data[idx + 2];
            } else {
                t->out->data[idx]     = t->from->data[idx];
                t->out->data[idx + 1] = t->from->data[idx + 1];
                t->out->data[idx + 2] = t->from->data[idx + 2];
            }
        }
    }
}

static void render_wipe_diag_br(pd_transition_t *t, float p)
{
    int w = t->out->width;
    int h = t->out->height;
    float threshold = 2.0f * p;
    for (int y = 0; y < h; y++) {
        float fy = (float)(h - 1 - y) / (float)h;
        for (int x = 0; x < w; x++) {
            float fx = (float)(w - 1 - x) / (float)w;
            int idx = (y * w + x) * 3;
            if (fx + fy < threshold) {
                t->out->data[idx]     = t->to->data[idx];
                t->out->data[idx + 1] = t->to->data[idx + 1];
                t->out->data[idx + 2] = t->to->data[idx + 2];
            } else {
                t->out->data[idx]     = t->from->data[idx];
                t->out->data[idx + 1] = t->from->data[idx + 1];
                t->out->data[idx + 2] = t->from->data[idx + 2];
            }
        }
    }
}

/* ---- diagonal split ---- */

static void render_split_diag(pd_transition_t *t, float p)
{
    /* top-left triangle slides toward TL, bottom-right toward BR, revealing "to" */
    int w = t->out->width;
    int h = t->out->height;
    float shift = p;  /* 0..1 */

    pd_framebuf_copy(t->out, t->to);

    for (int y = 0; y < h; y++) {
        float fy = (float)y / (float)h;
        for (int x = 0; x < w; x++) {
            float fx = (float)x / (float)w;
            int idx = (y * w + x) * 3;
            if (fx + fy < 1.0f) {
                /* top-left triangle: sample from shifted position */
                int sx = x - (int)(shift * w * 0.5f);
                int sy = y - (int)(shift * h * 0.5f);
                if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                    int si = (sy * w + sx) * 3;
                    t->out->data[idx]     = t->from->data[si];
                    t->out->data[idx + 1] = t->from->data[si + 1];
                    t->out->data[idx + 2] = t->from->data[si + 2];
                }
            } else {
                /* bottom-right triangle: sample from shifted position */
                int sx = x + (int)(shift * w * 0.5f);
                int sy = y + (int)(shift * h * 0.5f);
                if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                    int si = (sy * w + sx) * 3;
                    t->out->data[idx]     = t->from->data[si];
                    t->out->data[idx + 1] = t->from->data[si + 1];
                    t->out->data[idx + 2] = t->from->data[si + 2];
                }
            }
        }
    }
}

/* ---- zoom ---- */

static void render_zoom_in(pd_transition_t *t, float p)
{
    /* new content zooms in from center (starts tiny, grows to full) */
    int w = t->out->width;
    int h = t->out->height;
    float scale = p;  /* 0..1 */
    if (scale < 0.01f) scale = 0.01f;

    int zw = (int)(w * scale);
    int zh = (int)(h * scale);
    int ox = (w - zw) / 2;
    int oy = (h - zh) / 2;

    pd_framebuf_copy(t->out, t->from);

    for (int y = 0; y < zh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= h) continue;
        /* source row in "to" image: scale y back to full size */
        int sy = y * h / zh;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < zw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= w) continue;
            int sx = x * w / zw;
            if (sx >= w) sx = w - 1;
            int di = (dy * w + dx) * 3;
            int si = (sy * w + sx) * 3;
            t->out->data[di]     = t->to->data[si];
            t->out->data[di + 1] = t->to->data[si + 1];
            t->out->data[di + 2] = t->to->data[si + 2];
        }
    }
}

static void render_zoom_out(pd_transition_t *t, float p)
{
    /* old content shrinks to center, revealing new content behind */
    int w = t->out->width;
    int h = t->out->height;
    float scale = 1.0f - p;  /* 1..0 */
    if (scale < 0.01f) scale = 0.01f;

    int zw = (int)(w * scale);
    int zh = (int)(h * scale);
    int ox = (w - zw) / 2;
    int oy = (h - zh) / 2;

    pd_framebuf_copy(t->out, t->to);

    for (int y = 0; y < zh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= h) continue;
        int sy = y * h / zh;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < zw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= w) continue;
            int sx = x * w / zw;
            if (sx >= w) sx = w - 1;
            int di = (dy * w + dx) * 3;
            int si = (sy * w + sx) * 3;
            t->out->data[di]     = t->from->data[si];
            t->out->data[di + 1] = t->from->data[si + 1];
            t->out->data[di + 2] = t->from->data[si + 2];
        }
    }
}

/* ---- flip ---- */

static void render_flip_h(pd_transition_t *t, float p)
{
    /* horizontal flip: first half squeezes old content horizontally,
       second half expands new content */
    int w = t->out->width;
    int h = t->out->height;

    if (p < 0.5f) {
        /* squeeze old content: scale = 1.0 -> 0.0 */
        float scale = 1.0f - 2.0f * p;
        if (scale < 0.02f) scale = 0.02f;
        int sw = (int)(w * scale);
        int ox = (w - sw) / 2;

        pd_framebuf_clear(t->out);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < sw; x++) {
                int dx = ox + x;
                int sx = x * w / sw;
                if (sx >= w) sx = w - 1;
                int di = (y * w + dx) * 3;
                int si = (y * w + sx) * 3;
                t->out->data[di]     = t->from->data[si];
                t->out->data[di + 1] = t->from->data[si + 1];
                t->out->data[di + 2] = t->from->data[si + 2];
            }
        }
    } else {
        /* expand new content: scale = 0.0 -> 1.0 */
        float scale = 2.0f * (p - 0.5f);
        if (scale < 0.02f) scale = 0.02f;
        int sw = (int)(w * scale);
        int ox = (w - sw) / 2;

        pd_framebuf_clear(t->out);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < sw; x++) {
                int dx = ox + x;
                int sx = x * w / sw;
                if (sx >= w) sx = w - 1;
                int di = (y * w + dx) * 3;
                int si = (y * w + sx) * 3;
                t->out->data[di]     = t->to->data[si];
                t->out->data[di + 1] = t->to->data[si + 1];
                t->out->data[di + 2] = t->to->data[si + 2];
            }
        }
    }
}

static void render_flip_v(pd_transition_t *t, float p)
{
    /* vertical flip: first half squeezes old content vertically,
       second half expands new content */
    int w = t->out->width;
    int h = t->out->height;
    int row_bytes = w * 3;

    if (p < 0.5f) {
        float scale = 1.0f - 2.0f * p;
        if (scale < 0.02f) scale = 0.02f;
        int sh = (int)(h * scale);
        int oy = (h - sh) / 2;

        pd_framebuf_clear(t->out);
        for (int y = 0; y < sh; y++) {
            int dy = oy + y;
            int sy = y * h / sh;
            if (sy >= h) sy = h - 1;
            memcpy(t->out->data + dy * row_bytes, t->from->data + sy * row_bytes, row_bytes);
        }
    } else {
        float scale = 2.0f * (p - 0.5f);
        if (scale < 0.02f) scale = 0.02f;
        int sh = (int)(h * scale);
        int oy = (h - sh) / 2;

        pd_framebuf_clear(t->out);
        for (int y = 0; y < sh; y++) {
            int dy = oy + y;
            int sy = y * h / sh;
            if (sy >= h) sy = h - 1;
            memcpy(t->out->data + dy * row_bytes, t->to->data + sy * row_bytes, row_bytes);
        }
    }
}

/* ---- dispatch ---- */

typedef void (*transition_render_fn)(pd_transition_t *t, float p);

static const transition_render_fn render_fns[PD_TRANS_COUNT] = {
    [PD_TRANS_NONE]        = render_none,
    [PD_TRANS_WIPE_LEFT]   = render_wipe_left,
    [PD_TRANS_WIPE_RIGHT]  = render_wipe_right,
    [PD_TRANS_WIPE_UP]     = render_wipe_up,
    [PD_TRANS_WIPE_DOWN]   = render_wipe_down,
    [PD_TRANS_SLIDE_LEFT]  = render_slide_left,
    [PD_TRANS_SLIDE_RIGHT] = render_slide_right,
    [PD_TRANS_SLIDE_UP]    = render_slide_up,
    [PD_TRANS_SLIDE_DOWN]  = render_slide_down,
    [PD_TRANS_ROLL_UP]     = render_roll_up,
    [PD_TRANS_ROLL_DOWN]   = render_roll_down,
    [PD_TRANS_SPLIT_H]     = render_split_h,
    [PD_TRANS_SPLIT_V]     = render_split_v,
    [PD_TRANS_FADE]        = render_fade,
    [PD_TRANS_BLOCK_BUILD]  = render_block_build,
    [PD_TRANS_PIXEL_BUILD]  = render_pixel_build,
    [PD_TRANS_WIPE_DIAG_TL] = render_wipe_diag_tl,
    [PD_TRANS_WIPE_DIAG_TR] = render_wipe_diag_tr,
    [PD_TRANS_WIPE_DIAG_BL] = render_wipe_diag_bl,
    [PD_TRANS_WIPE_DIAG_BR] = render_wipe_diag_br,
    [PD_TRANS_SPLIT_DIAG]   = render_split_diag,
    [PD_TRANS_ZOOM_IN]      = render_zoom_in,
    [PD_TRANS_ZOOM_OUT]     = render_zoom_out,
    [PD_TRANS_FLIP_H]       = render_flip_h,
    [PD_TRANS_FLIP_V]       = render_flip_v,
};

bool pd_transition_tick(pd_transition_t *t)
{
    if (!t || !t->active) return false;

    float p = pd_transition_progress(t);

    transition_render_fn fn = render_fns[t->type];
    if (fn) {
        fn(t, p);
    } else {
        render_none(t, p);
    }

    if (p >= 1.0f) {
        t->active = false;
        pd_framebuf_copy(t->out, t->to);
        ESP_LOGI(TAG, "transition complete: %s", pd_transition_type_name(t->type));
    }

    return t->active;
}
