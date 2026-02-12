#ifndef PD_DISPLAY_H
#define PD_DISPLAY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    int orientation_deg;
} pd_display_config_t;

esp_err_t pd_display_init(const pd_display_config_t *config);
void pd_display_render_boot_message(void);
void pd_display_render_idle(void);

#ifdef __cplusplus
}
#endif

#endif
