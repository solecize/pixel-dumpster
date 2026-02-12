#include "pd-storage.h"

#include "esp_log.h"

static const char *TAG = "pd-storage";

esp_err_t pd_storage_init(const pd_storage_config_t *config)
{
    const char *base_path = config && config->base_path ? config->base_path : "/pd";
    ESP_LOGI(TAG, "storage init at %s", base_path);
    return ESP_OK;
}
