#ifndef PD_WIZARD_H
#define PD_WIZARD_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PD_WIZARD_STATE_IDLE = 0,
    PD_WIZARD_STATE_KEYBOARD_CHECK,
    PD_WIZARD_STATE_MATRIX_SIZE,
    PD_WIZARD_STATE_MATRIX_ORIENTATION,
    PD_WIZARD_STATE_WIFI_SCAN,
    PD_WIZARD_STATE_WIFI_PASSWORD,
    PD_WIZARD_STATE_DEVICE_NAME,
    PD_WIZARD_STATE_COMPLETE
} pd_wizard_state_t;

esp_err_t pd_wizard_start(void);
void pd_wizard_tick(void);

#ifdef __cplusplus
}
#endif

#endif
