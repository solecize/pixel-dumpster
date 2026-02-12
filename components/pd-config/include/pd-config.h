#ifndef PD_CONFIG_H
#define PD_CONFIG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int matrix_width;
    int matrix_height;
    int orientation_deg;
    char wifi_ssid[33];
    char wifi_password[65];
    char device_name[33];
    bool setup_complete;
    int config_version;
} pd_config_t;

esp_err_t pd_config_init(pd_config_t *config);
esp_err_t pd_config_load(pd_config_t *config);
esp_err_t pd_config_save(const pd_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
