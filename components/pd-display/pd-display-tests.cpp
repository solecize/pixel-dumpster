#include "pd-display.h"

#include <cstring>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pd-display-test";

/* ---------- test state ---------- */
static pd_test_pattern_t test_current = PD_TEST_PATTERN_NONE;
static int64_t test_start_us = 0;
static int test_duration_ms = 0;          /* <= 0 = indefinite */
static int test_frame = 0;
static int64_t test_last_tick_us = 0;

/* bouncing-ball state */
static float test_ball_x = 0.0f;
static float test_ball_y = 0.0f;
static float test_ball_vx = 2.0f;
static float test_ball_vy = 1.5f;
static int test_ball_r = 4;

/* arrow-chain state */
static int test_arrow_panel = 0;
static int64_t test_arrow_last_us = 0;

/* rgb-sweep state */
static int test_rgb_phase = 0;
static int64_t test_rgb_last_us = 0;

/* numbered panels: only redraw when dirty */
static bool test_numbered_dirty = true;

/* panel-layout test state */
static int test_layout_selected = 0;

/* ---------- helpers ---------- */

extern "C" pd_test_pattern_t pd_display_test_pattern_from_name(const char *name)
{
    if (!name) return PD_TEST_PATTERN_NONE;
    if (strcasecmp(name, "numbered_panels") == 0)      return PD_TEST_PATTERN_NUMBERED_PANELS;
    if (strcasecmp(name, "checkerboard") == 0)           return PD_TEST_PATTERN_CHECKERBOARD_SCROLL;
    if (strcasecmp(name, "checkerboard_scroll") == 0)    return PD_TEST_PATTERN_CHECKERBOARD_SCROLL;
    if (strcasecmp(name, "arrow_chain") == 0)            return PD_TEST_PATTERN_ARROW_CHAIN;
    if (strcasecmp(name, "bouncing_ball") == 0)          return PD_TEST_PATTERN_BOUNCING_BALL;
    if (strcasecmp(name, "rgb_sweep") == 0)            return PD_TEST_PATTERN_RGB_SWEEP;
    if (strcasecmp(name, "color_test") == 0)          return PD_TEST_PATTERN_COLOR_TEST;
    if (strcasecmp(name, "panel_layout") == 0)        return PD_TEST_PATTERN_PANEL_LAYOUT;
    return PD_TEST_PATTERN_NONE;
}

extern "C" esp_err_t pd_display_test_start(pd_test_pattern_t pattern, int duration_ms)
{
    if (pattern == PD_TEST_PATTERN_NONE) {
        pd_display_test_stop();
        return ESP_OK;
    }
    test_current = pattern;
    test_start_us = esp_timer_get_time();
    test_duration_ms = duration_ms;
    test_frame = 0;
    test_last_tick_us = test_start_us;
    test_numbered_dirty = true;

    /* init per-pattern state */
    test_ball_x = 10.0f;
    test_ball_y = 10.0f;
    test_ball_vx = 2.0f;
    test_ball_vy = 1.5f;
    test_arrow_panel = 0;
    test_arrow_last_us = test_start_us;
    test_rgb_phase = 0;
    test_rgb_last_us = test_start_us;

    /* immediate first draw */
    pd_display_test_tick();
    ESP_LOGI(TAG, "test started: pattern=%d duration=%d ms", (int)pattern, duration_ms);
    return ESP_OK;
}

extern "C" void pd_display_test_stop(void)
{
    if (test_current != PD_TEST_PATTERN_NONE) {
        pd_display_clear();
        pd_display_flip_buffer();
        ESP_LOGI(TAG, "test stopped");
    }
    test_current = PD_TEST_PATTERN_NONE;
}

extern "C" bool pd_display_test_active(void)
{
    if (test_current == PD_TEST_PATTERN_NONE) return false;
    if (test_duration_ms > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - test_start_us) / 1000;
        if (elapsed_ms >= test_duration_ms) {
            pd_display_test_stop();
            return false;
        }
    }
    return true;
}

extern "C" pd_test_pattern_t pd_display_test_current(void)
{
    return test_current;
}

/* HSV → RGB (h in degrees 0–360, s and v 0.0–1.0) */
static pd_display_color_t hsv_to_rgb(float h, float s, float v)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return (pd_display_color_t){
        (uint8_t)((r + m) * 255),
        (uint8_t)((g + m) * 255),
        (uint8_t)((b + m) * 255)
    };
}

/* 3×5 digit font (bit 14 = top-left … bit 0 = bottom-right) */
static const uint16_t DIG_FONT[10] = {
    0x7B6F, /* 0  ###  */
    0x2C97, /* 1  .#.  */
    0x73E7, /* 2  ###  */
    0x73CF, /* 3  ###  */
    0x5BC9, /* 4  #.#  */
    0x79CF, /* 5  ###  */
    0x79EF, /* 6  ###  */
    0x7249, /* 7  ###  */
    0x7BEF, /* 8  ###  */
    0x7BC9, /* 9  ###  */
};

static void test_draw_digit_scaled(int digit, int cx, int cy, int scale,
                                    pd_display_color_t color)
{
    if (digit < 0 || digit > 9) return;
    uint16_t bits = DIG_FONT[digit];
    int fw = 3 * scale;
    int fh = 5 * scale;
    int x0 = cx - fw / 2;
    int y0 = cy - fh / 2;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            if (bits & (1 << (14 - idx))) {
                pd_display_fill((uint16_t)(x0 + c * scale), (uint16_t)(y0 + r * scale),
                                (uint16_t)scale, (uint16_t)scale, color);
            }
        }
    }
}

static void test_draw_digit(int digit, int cx, int cy, int /*cell_w*/, int /*cell_h*/,
                            pd_display_color_t color)
{
    test_draw_digit_scaled(digit, cx, cy, 3, color);
}

/* Draw faint panel borders and small corner numbers on every panel */
static void test_draw_panel_overlay(void)
{
    int rows = pd_display_get_panel_rows();
    int cols = pd_display_get_panel_cols();
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int px, py, pw, ph;
            if (!pd_display_get_panel_rect(r, c, &px, &py, &pw, &ph)) continue;
            /* faint border */
            pd_display_draw_rect((uint16_t)px, (uint16_t)py, (uint16_t)pw, (uint16_t)ph, PD_COLOR_DIM);
            /* small number in top-left corner (scale 2, offset by 2 px) */
            int num = r * cols + c + 1;
            if (num <= 9) {
                test_draw_digit_scaled(num, px + 5, py + 6, 2, PD_COLOR_DIM);
            } else {
                int tens = num / 10;
                int ones = num % 10;
                test_draw_digit_scaled(tens, px + 4, py + 6, 2, PD_COLOR_DIM);
                test_draw_digit_scaled(ones, px + 9, py + 6, 2, PD_COLOR_DIM);
            }
        }
    }
}

/* Does the chain flow left-to-right for this panel? */
static bool test_arrow_points_right(int row, int /*col*/)
{
    int chain = pd_display_get_chain_pattern();
    bool even_row = (row % 2) == 0;
    switch (chain) {
        case 0:  /* HORIZONTAL */
            return true;
        case 1:  /* TOP_LEFT_DOWN */
        case 3:  /* BOTTOM_LEFT_UP */
            return even_row;
        case 2:  /* TOP_RIGHT_DOWN */
        case 4:  /* BOTTOM_RIGHT_UP */
            return !even_row;
        case 5:  /* TOP_LEFT_DOWN_ZIGZAG */
        case 7:  /* BOTTOM_LEFT_UP_ZIGZAG */
            return true;
        case 6:  /* TOP_RIGHT_DOWN_ZIGZAG */
        case 8:  /* BOTTOM_RIGHT_UP_ZIGZAG */
            return false;
        default:
            return true;
    }
}

/* ---------- pattern implementations ---------- */

static void test_pattern_numbered_panels(void)
{
    if (!test_numbered_dirty) return;
    test_numbered_dirty = false;

    pd_display_clear();
    int rows = pd_display_get_panel_rows();
    int cols = pd_display_get_panel_cols();
    pd_display_color_t white = PD_COLOR_WHITE;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int px, py, pw, ph;
            if (!pd_display_get_panel_rect(r, c, &px, &py, &pw, &ph)) continue;
            int num = r * cols + c + 1;
            /* draw a faint border around each panel cell */
            pd_display_draw_rect((uint16_t)px, (uint16_t)py, (uint16_t)pw, (uint16_t)ph, PD_COLOR_DIM);
            /* draw the number centered */
            if (num <= 9) {
                test_draw_digit(num, px + pw / 2, py + ph / 2, pw, ph, white);
            } else {
                /* two-digit: draw tens then ones */
                int tens = num / 10;
                int ones = num % 10;
                int gap = 8;
                test_draw_digit(tens, px + pw / 2 - gap, py + ph / 2, pw, ph, white);
                test_draw_digit(ones, px + pw / 2 + gap, py + ph / 2, pw, ph, white);
            }
        }
    }
    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_checkerboard_scroll(void)
{
    int64_t now = esp_timer_get_time();
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    int shift = (int)((now / 200000) % 16); /* scroll one square every 200ms */

    pd_display_clear();
    int sq = 8;
    for (int y = 0; y < dh; y += sq) {
        for (int x = 0; x < dw; x += sq) {
            int bx = (x + shift) / sq;
            int by = y / sq;
            bool dark = ((bx + by) & 1);
            pd_display_fill((uint16_t)x, (uint16_t)y, (uint16_t)sq, (uint16_t)sq,
                            dark ? PD_COLOR_DIM : PD_COLOR_WHITE);
        }
    }
    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_arrow_chain(void)
{
    int64_t now = esp_timer_get_time();
    int rows = pd_display_get_panel_rows();
    int cols = pd_display_get_panel_cols();
    int total = rows * cols;  /* NOLINT: used in modulo below */

    /* advance one panel per second */
    if ((now - test_arrow_last_us) >= 1000000) {
        test_arrow_last_us = now;
        test_arrow_panel = (test_arrow_panel + 1) % total;
    }

    pd_display_clear();
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int px, py, pw, ph;
            if (!pd_display_get_panel_rect(r, c, &px, &py, &pw, &ph)) continue;
            int idx = r * cols + c;
            pd_display_color_t col = (idx == test_arrow_panel) ? PD_COLOR_GREEN : PD_COLOR_DIM;
            pd_display_draw_rect((uint16_t)px, (uint16_t)py, (uint16_t)pw, (uint16_t)ph, col);
            /* draw arrow inside every panel (direction shows chain flow) */
            int ax = px + pw / 2;
            int ay = py + ph / 2;
            bool right = test_arrow_points_right(r, c);
            /* body */
            pd_display_fill((uint16_t)(ax - 3), (uint16_t)ay, 7, 1, PD_COLOR_WHITE);
            /* head */
            if (right) {
                pd_display_set_pixel((uint16_t)(ax + 3), (uint16_t)(ay - 1), PD_COLOR_WHITE);
                pd_display_set_pixel((uint16_t)(ax + 4), (uint16_t)ay,     PD_COLOR_WHITE);
                pd_display_set_pixel((uint16_t)(ax + 3), (uint16_t)(ay + 1), PD_COLOR_WHITE);
            } else {
                pd_display_set_pixel((uint16_t)(ax - 3), (uint16_t)(ay - 1), PD_COLOR_WHITE);
                pd_display_set_pixel((uint16_t)(ax - 4), (uint16_t)ay,     PD_COLOR_WHITE);
                pd_display_set_pixel((uint16_t)(ax - 3), (uint16_t)(ay + 1), PD_COLOR_WHITE);
            }
            /* highlight active panel with a brighter arrow head */
            if (idx == test_arrow_panel) {
                pd_display_color_t hl = PD_COLOR_GREEN;
                if (right) {
                    pd_display_set_pixel((uint16_t)(ax + 3), (uint16_t)(ay - 1), hl);
                    pd_display_set_pixel((uint16_t)(ax + 4), (uint16_t)ay,     hl);
                    pd_display_set_pixel((uint16_t)(ax + 3), (uint16_t)(ay + 1), hl);
                } else {
                    pd_display_set_pixel((uint16_t)(ax - 3), (uint16_t)(ay - 1), hl);
                    pd_display_set_pixel((uint16_t)(ax - 4), (uint16_t)ay,     hl);
                    pd_display_set_pixel((uint16_t)(ax - 3), (uint16_t)(ay + 1), hl);
                }
            }
        }
    }
    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_bouncing_ball(void)
{
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();

    /* physics */
    test_ball_x += test_ball_vx;
    test_ball_y += test_ball_vy;
    if (test_ball_x - test_ball_r < 0) { test_ball_x = (float)test_ball_r; test_ball_vx = -test_ball_vx; }
    if (test_ball_x + test_ball_r >= dw) { test_ball_x = (float)(dw - 1 - test_ball_r); test_ball_vx = -test_ball_vx; }
    if (test_ball_y - test_ball_r < 0) { test_ball_y = (float)test_ball_r; test_ball_vy = -test_ball_vy; }
    if (test_ball_y + test_ball_r >= dh) { test_ball_y = (float)(dh - 1 - test_ball_r); test_ball_vy = -test_ball_vy; }

    pd_display_clear();
    /* draw ball */
    int bx = (int)roundf(test_ball_x);
    int by = (int)roundf(test_ball_y);
    for (int dy = -test_ball_r; dy <= test_ball_r; dy++) {
        for (int dx = -test_ball_r; dx <= test_ball_r; dx++) {
            if (dx * dx + dy * dy <= test_ball_r * test_ball_r) {
                int bpx = bx + dx;
                int bpy = by + dy;
                if (bpx >= 0 && bpx < dw && bpy >= 0 && bpy < dh) {
                    pd_display_set_pixel((uint16_t)bpx, (uint16_t)bpy, PD_COLOR_WHITE);
                }
            }
        }
    }
    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_rgb_sweep(void)
{
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    int64_t now = esp_timer_get_time();

    /* full rainbow cycle every 4 seconds */
    float shift = fmodf(now / 4000000.0f, 1.0f) * 360.0f;

    pd_display_clear();
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            float hue = fmodf((x * 360.0f / dw) + shift, 360.0f);
            pd_display_color_t col = hsv_to_rgb(hue, 1.0f, 1.0f);
            pd_display_set_pixel((uint16_t)x, (uint16_t)y, col);
        }
    }
    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_color_test(void)
{
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    int h3 = dh / 3;

    pd_display_clear();

    pd_display_color_t red    = {255, 0, 0};
    pd_display_color_t green  = {0, 255, 0};
    pd_display_color_t blue   = {0, 0, 255};
    pd_display_color_t white  = {255, 255, 255};
    pd_display_color_t black  = {0, 0, 0};

    /* top third: pure red */
    pd_display_fill(0, 0, (uint16_t)dw, (uint16_t)h3, red);
    /* middle third: pure green */
    pd_display_fill(0, (uint16_t)h3, (uint16_t)dw, (uint16_t)h3, green);
    /* bottom third: pure blue */
    pd_display_fill(0, (uint16_t)(h3 * 2), (uint16_t)dw, (uint16_t)(dh - h3 * 2), blue);

    /* white separators */
    pd_display_fill(0, (uint16_t)h3, (uint16_t)dw, 1, white);
    pd_display_fill(0, (uint16_t)(h3 * 2), (uint16_t)dw, 1, white);

    /* labels in black text centered in each band */
    int cy1 = h3 / 2;
    int cy2 = h3 + h3 / 2;
    int cy3 = h3 * 2 + h3 / 2;
    int cx = dw / 2 - 3;  /* 6px wide text roughly centered */
    pd_display_draw_text((uint16_t)cx, (uint16_t)(cy1 - 4), "R", black);
    pd_display_draw_text((uint16_t)cx, (uint16_t)(cy2 - 4), "G", black);
    pd_display_draw_text((uint16_t)cx, (uint16_t)(cy3 - 4), "B", black);

    test_draw_panel_overlay();
    pd_display_flip_buffer();
}

static void test_pattern_panel_layout(void)
{
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    int rows = pd_display_get_panel_rows();
    int cols = pd_display_get_panel_cols();
    if (rows <= 0) rows = 1;
    if (cols <= 0) cols = 1;
    int pw = dw / cols;
    int ph = dh / rows;

    pd_display_clear();

    /* Clamp selection */
    int total = rows * cols;
    if (test_layout_selected < 0) test_layout_selected = 0;
    if (test_layout_selected >= total) test_layout_selected = total - 1;

    pd_display_color_t red   = {255, 0, 0};
    pd_display_color_t green = {0, 255, 0};
    pd_display_color_t blue  = {0, 0, 255};
    pd_display_color_t white = {255, 255, 255};
    pd_display_color_t black = {0, 0, 0};
    pd_display_color_t dim   = {64, 64, 64};

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            int px = c * pw;
            int py = r * ph;
            bool sel = (idx == test_layout_selected);

            /* Panel border */
            pd_display_color_t border = sel ? white : dim;
            for (int x = 0; x < pw; x++) {
                pd_display_set_pixel((uint16_t)(px + x), (uint16_t)py, border);
                pd_display_set_pixel((uint16_t)(px + x), (uint16_t)(py + ph - 1), border);
            }
            for (int y = 0; y < ph; y++) {
                pd_display_set_pixel((uint16_t)px, (uint16_t)(py + y), border);
                pd_display_set_pixel((uint16_t)(px + pw - 1), (uint16_t)(py + y), border);
            }

            /* Panel number */
            char num_buf[12];
            snprintf(num_buf, sizeof(num_buf), "%d", idx + 1);
            int nx = px + pw / 2 - 2;
            int ny = py + 4;
            pd_display_draw_text((uint16_t)nx, (uint16_t)ny, num_buf, sel ? white : dim);

            /* RGB squares — small 4x4 blocks near bottom center */
            int sq = 4;
            int gap = 2;
            int rgb_w = sq * 3 + gap * 2;
            int rx = px + (pw - rgb_w) / 2;
            int ry = py + ph - sq - 4;

            /* R */
            pd_display_fill((uint16_t)rx, (uint16_t)ry, (uint16_t)sq, (uint16_t)sq, red);
            pd_display_draw_text((uint16_t)(rx + 1), (uint16_t)(ry - 6), "R", sel ? white : dim);
            /* G */
            pd_display_fill((uint16_t)(rx + sq + gap), (uint16_t)ry, (uint16_t)sq, (uint16_t)sq, green);
            pd_display_draw_text((uint16_t)(rx + sq + gap + 1), (uint16_t)(ry - 6), "G", sel ? white : dim);
            /* B */
            pd_display_fill((uint16_t)(rx + (sq + gap) * 2), (uint16_t)ry, (uint16_t)sq, (uint16_t)sq, blue);
            pd_display_draw_text((uint16_t)(rx + (sq + gap) * 2 + 1), (uint16_t)(ry - 6), "B", sel ? white : dim);
        }
    }

    pd_display_flip_buffer();
}

extern "C" void pd_display_test_set_layout_selected(int index)
{
    int rows = pd_display_get_panel_rows();
    int cols = pd_display_get_panel_cols();
    int total = (rows > 0 ? rows : 1) * (cols > 0 ? cols : 1);
    if (index < 0) index = total - 1;
    if (index >= total) index = 0;
    test_layout_selected = index;
}

extern "C" void pd_display_test_tick(void)
{
    if (!pd_display_test_active()) return;

    /* cap test patterns to ~30 fps to avoid flicker/tearing */
    int64_t now = esp_timer_get_time();
    if (now - test_last_tick_us < 33333) return;
    test_last_tick_us = now;

    switch (test_current) {
        case PD_TEST_PATTERN_NUMBERED_PANELS:
            test_pattern_numbered_panels();
            break;
        case PD_TEST_PATTERN_CHECKERBOARD_SCROLL:
            test_pattern_checkerboard_scroll();
            break;
        case PD_TEST_PATTERN_ARROW_CHAIN:
            test_pattern_arrow_chain();
            break;
        case PD_TEST_PATTERN_BOUNCING_BALL:
            test_pattern_bouncing_ball();
            break;
        case PD_TEST_PATTERN_RGB_SWEEP:
            test_pattern_rgb_sweep();
            break;
        case PD_TEST_PATTERN_COLOR_TEST:
            test_pattern_color_test();
            break;
        case PD_TEST_PATTERN_PANEL_LAYOUT:
            test_pattern_panel_layout();
            break;
        default:
            break;
    }
    test_frame++;
}
