#include "pd-wizard.h"

#include "esp_log.h"

static const char *TAG = "pd-wizard";
static pd_wizard_state_t pd_wizard_state = PD_WIZARD_STATE_IDLE;

esp_err_t pd_wizard_start(void)
{
    pd_wizard_state = PD_WIZARD_STATE_KEYBOARD_CHECK;
    ESP_LOGI(TAG, "wizard start");
    return ESP_OK;
}

void pd_wizard_tick(void)
{
    ESP_LOGD(TAG, "wizard state=%d", pd_wizard_state);
}
