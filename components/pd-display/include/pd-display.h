#ifndef PD_DISPLAY_H
#define PD_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    int orientation_deg;
    int scan_wiring;
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
esp_err_t pd_display_reinit(int width, int height, int orientation_deg, int scan_wiring);
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

void pd_display_wizard_menu(const char *title, const char **options, int count, int selected);
void pd_display_wizard_text(const char *title, const char *value, bool mask);
void pd_display_wizard_status(const char *message);

#ifdef __cplusplus
}
#endif

#endif
