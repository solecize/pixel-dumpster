#include "pd-display.h"

#include "esp_log.h"

static const char *TAG = "pd-display";

esp_err_t pd_display_init(const pd_display_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "display init: %dx%d @ %d", config->width, config->height, config->orientation_deg);
    return ESP_OK;
}

void pd_display_render_boot_message(void)
{
    ESP_LOGI(TAG, "render boot message: keyboard");
}

void pd_display_render_idle(void)
{
    ESP_LOGI(TAG, "render idle display");
}
