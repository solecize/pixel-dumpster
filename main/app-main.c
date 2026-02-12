#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd-config.h"
#include "pd-display.h"
#include "pd-network.h"
#include "pd-storage.h"
#include "pd-wizard.h"

static const char *TAG = "pixel-dumpster";

static void initialize_system(void)
{
    pd_config_t config = {0};
    pd_display_config_t display_config = {
        .width = 224,
        .height = 64,
        .orientation_deg = 0
    };
    pd_network_config_t network_config = {
        .http_port = 8088,
        .udp_port = 9876
    };
    pd_storage_config_t storage_config = {
        .base_path = "/pd"
    };

    ESP_LOGI(TAG, "Initializing system (ESP-IDF workflow)");
    pd_config_init(&config);
    pd_storage_init(&storage_config);
    pd_display_init(&display_config);
    pd_display_render_boot_message();
    pd_network_init(&network_config);
    pd_network_start();
    pd_wizard_start();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    initialize_system();

    while (true) {
        pd_network_poll();
        pd_wizard_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
