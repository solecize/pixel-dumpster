#include "pd-network.h"

#include "esp_log.h"

static const char *TAG = "pd-network";

static pd_network_config_t pd_network_config = {
    .http_port = 8088,
    .udp_port = 9876
};

esp_err_t pd_network_init(const pd_network_config_t *config)
{
    if (config) {
        pd_network_config = *config;
    }

    ESP_LOGI(TAG, "network init http=%d udp=%d", pd_network_config.http_port, pd_network_config.udp_port);
    return ESP_OK;
}

void pd_network_start(void)
{
    ESP_LOGI(TAG, "network start (wifi/http/udp pending)");
}

void pd_network_poll(void)
{
    ESP_LOGD(TAG, "network poll tick");
}
