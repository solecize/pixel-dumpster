#ifndef PD_DISPLAY_H
#define PD_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Driver init config — describes a chain of identical panel modules.
 *
 * For a single panel:
 *   panel_width  = panel_height = native module size
 *   panel_rows   = panel_cols   = 1
 *   chain_pattern = 0 (HORIZONTAL)
 *
 * For a multi-panel chain (1×N marquee, M×N grid, etc) the driver fans the
 * virtual canvas across all panels using layout_rows/cols and chain_pattern. */
typedef struct {
    int panel_width;        /* per-module native width  (e.g. 64) */
    int panel_height;       /* per-module native height (e.g. 32) */
    int panel_rows;         /* number of panels stacked vertically (>=1) */
    int panel_cols;         /* number of panels chained horizontally (>=1) */
    int chain_pattern;      /* enum mapping to Hub75PanelLayout */
    int rotation_deg;       /* global canvas rotation 0/90/180/270 */
    int scan_wiring;        /* enum mapping to Hub75ScanWiring */
    int color_order;        /* 0=RGB, 1=BGR, 2=GRB, 3=BRG */
} pd_display_config_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pd_display_color_t;

#define PD_COLOR_BLACK   ((pd_display_color_t){0, 0, 0})
#define PD_COLOR_WHITE   ((pd_display_color_t){220, 220, 220})
#define PD_COLOR_YELLOW  ((pd_display_color_t){220, 200, 0})
#define PD_COLOR_GREEN   ((pd_display_color_t){0, 200, 60})
#define PD_COLOR_RED     ((pd_display_color_t){200, 40, 20})
#define PD_COLOR_CYAN    ((pd_display_color_t){0, 180, 200})
#define PD_COLOR_DIM     ((pd_display_color_t){60, 60, 60})

esp_err_t pd_display_init(const pd_display_config_t *config);
esp_err_t pd_display_reinit(const pd_display_config_t *config);
void pd_display_set_brightness(uint8_t brightness);
uint8_t pd_display_get_brightness(void);
void pd_display_render_boot_message(void);
void pd_display_render_idle(const char *device_name, int res_w, int res_h,
                            int orient, int scan, const char *ip);

void pd_display_clear(void);
void pd_display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, pd_display_color_t color);
void pd_display_draw_char(uint16_t x, uint16_t y, char ch, pd_display_color_t color);
void pd_display_draw_text(uint16_t x, uint16_t y, const char *text, pd_display_color_t color);
void pd_display_draw_char_tiny(uint16_t x, uint16_t y, char ch, pd_display_color_t color);
void pd_display_draw_text_tiny(uint16_t x, uint16_t y, const char *text, pd_display_color_t color);

int pd_display_get_width(void);
int pd_display_get_height(void);
int pd_display_text_cols(void);
int pd_display_text_rows(void);

void pd_display_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, pd_display_color_t color);
void pd_display_set_pixel(uint16_t x, uint16_t y, pd_display_color_t color);

void pd_display_render_rgb(const uint8_t *rgb, int img_w, int img_h);
void pd_display_render_framebuf(const uint8_t *rgb);

void pd_display_wizard_menu(const char *title, const char **options, int count, int selected);
void pd_display_wizard_text(const char *title, const char *value, bool mask);
void pd_display_wizard_status(const char *message);

void pd_display_render_source_status(int state, const char *hostname, const char *ip,
                                     const char *es_version, bool browsing, bool launch,
                                     const char *methods);
void pd_display_render_no_source(void);
void pd_display_render_default_marquee(void);

/* ---------- Multi-panel layout introspection ---------- */
/* These describe the *current* configured layout so test patterns and
 * diagnostics can iterate over individual panels in the chain. */
int pd_display_get_panel_width(void);
int pd_display_get_panel_height(void);
int pd_display_get_panel_rows(void);
int pd_display_get_panel_cols(void);
int pd_display_get_chain_pattern(void);
int pd_display_get_rotation_deg(void);
int pd_display_get_color_order(void);

/* Compute the virtual-canvas (post-rotation) coordinates of panel at
 * (row, col) within the chain. Returns false if indices are out of range
 * or the driver isn't initialized. The returned rectangle covers the
 * region of the framebuffer that maps to that physical module. */
bool pd_display_get_panel_rect(int row, int col,
                               int *out_x, int *out_y,
                               int *out_w, int *out_h);

void pd_display_flip_buffer(void);

/* ---------- Test patterns ---------- */
typedef enum {
    PD_TEST_PATTERN_NONE = 0,
    PD_TEST_PATTERN_NUMBERED_PANELS,
    PD_TEST_PATTERN_CHECKERBOARD_SCROLL,
    PD_TEST_PATTERN_ARROW_CHAIN,
    PD_TEST_PATTERN_BOUNCING_BALL,
    PD_TEST_PATTERN_RGB_SWEEP,
    PD_TEST_PATTERN_COLOR_TEST,
    PD_TEST_PATTERN_PANEL_LAYOUT,
} pd_test_pattern_t;

/* Start a test pattern. duration_ms <= 0 means run indefinitely until stopped. */
esp_err_t pd_display_test_start(pd_test_pattern_t pattern, int duration_ms);
void pd_display_test_stop(void);
bool pd_display_test_active(void);
pd_test_pattern_t pd_display_test_current(void);
/* Called by main loop / content tick to advance animated patterns. */
void pd_display_test_tick(void);
/* Map a pattern name string to enum (case-insensitive). Returns NONE if unknown. */
pd_test_pattern_t pd_display_test_pattern_from_name(const char *name);
/* Set the selected panel index for the panel_layout test pattern. */
void pd_display_test_set_layout_selected(int index);

#ifdef __cplusplus
}
#endif

#endif
