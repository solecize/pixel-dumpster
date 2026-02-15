#ifndef PD_STORAGE_H
#define PD_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;
} pd_storage_config_t;

typedef enum {
    PD_DISPLAY_MODE_IDLE = 0,
    PD_DISPLAY_MODE_SYSTEM,
    PD_DISPLAY_MODE_GAME,
    PD_DISPLAY_MODE_CUSTOM
} pd_display_mode_t;

typedef struct {
    pd_display_mode_t mode;
    char system[32];
    char game[64];
    char asset[128];
    int64_t updated_at;
} pd_display_state_t;

esp_err_t pd_storage_init(const pd_storage_config_t *config);
esp_err_t pd_storage_load_state(pd_display_state_t *state);
esp_err_t pd_storage_save_state(const pd_display_state_t *state);

const char *pd_storage_get_base_path(void);

#ifdef __cplusplus
}
#endif

#endif
