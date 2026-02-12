#include "pd-config.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "pd-config";

static void pd_config_set_defaults(pd_config_t *config)
{
    if (!config) {
        return;
    }

    config->matrix_width = 224;
    config->matrix_height = 64;
    config->orientation_deg = 0;
    config->wifi_ssid[0] = '\0';
    config->wifi_password[0] = '\0';
    strncpy(config->device_name, "pixel-dumpster", sizeof(config->device_name) - 1);
    config->device_name[sizeof(config->device_name) - 1] = '\0';
    config->setup_complete = false;
    config->config_version = 1;
}

esp_err_t pd_config_init(pd_config_t *config)
{
    pd_config_set_defaults(config);
    ESP_LOGI(TAG, "config initialized");
    return ESP_OK;
}

esp_err_t pd_config_load(pd_config_t *config)
{
    pd_config_set_defaults(config);
    ESP_LOGW(TAG, "config load not implemented yet");
    return ESP_OK;
}

esp_err_t pd_config_save(const pd_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "config save not implemented yet");
    return ESP_OK;
}
