#ifndef PD_WIZARD_H
#define PD_WIZARD_H

#include <stdbool.h>

#include "esp_err.h"
#include "pd-config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PD_WIZARD_STATE_IDLE = 0,
    PD_WIZARD_STATE_KEYBOARD_CHECK,
    PD_WIZARD_STATE_MATRIX_SIZE,
    PD_WIZARD_STATE_MATRIX_ORIENTATION,
    PD_WIZARD_STATE_WIFI_SSID,
    PD_WIZARD_STATE_WIFI_PASSWORD,
    PD_WIZARD_STATE_DEVICE_NAME,
    PD_WIZARD_STATE_HOSTNAME,
    PD_WIZARD_STATE_TIMEZONE,
    PD_WIZARD_STATE_STATIC_IP,
    PD_WIZARD_STATE_STATIC_GATEWAY,
    PD_WIZARD_STATE_STATIC_NETMASK,
    PD_WIZARD_STATE_COMPLETE
} pd_wizard_state_t;

esp_err_t pd_wizard_start(pd_config_t *config);
void pd_wizard_tick(void);
bool pd_wizard_is_complete(void);

/* reztest: get display config for current combo index (for app-main display init) */
bool pd_wizard_reztest_get_display(const pd_config_t *config, int *width, int *height, int *scan_wiring);

#ifdef __cplusplus
}
#endif

#endif
