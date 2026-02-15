#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd-config.h"
#include "pd-content.h"
#include "pd-display.h"
#include "pd-network.h"
#include "pd-storage.h"
#include "pd-wizard.h"

static const char *TAG = "pixel-dumpster";

static pd_config_t pd_app_config = {0};

static void initialize_system(void)
{
    pd_network_config_t network_config = {
        .http_port = 8088,
        .udp_port = 9876
    };
    pd_storage_config_t storage_config = {
        .base_path = "/pd"
    };

    ESP_LOGI(TAG, "Initializing system (ESP-IDF workflow)");
    pd_storage_init(&storage_config);
    pd_config_init(&pd_app_config);
    pd_config_load(&pd_app_config);

    ESP_LOGI(TAG, "config: %dx%d orient=%d scan=%d complete=%d reztest=%d[%d]",
             pd_app_config.matrix_width, pd_app_config.matrix_height,
             pd_app_config.orientation_deg, pd_app_config.scan_wiring,
             pd_app_config.setup_complete,
             pd_app_config.reztest_mode, pd_app_config.reztest_index);

    pd_display_config_t display_config;
    int rz_w, rz_h, rz_scan;
    if (pd_wizard_reztest_get_display(&pd_app_config, &rz_w, &rz_h, &rz_scan)) {
        /* reztest mode: init display with the current combo */
        display_config = (pd_display_config_t){
            .width = rz_w,
            .height = rz_h,
            .orientation_deg = 0,
            .scan_wiring = rz_scan
        };
    } else if (!pd_app_config.setup_complete && pd_app_config.reztest_done
               && pd_app_config.matrix_width > 0 && pd_app_config.matrix_height > 0) {
        /* returning from reztest lock-in: use the chosen resolution during wizard */
        display_config = (pd_display_config_t){
            .width = pd_app_config.matrix_width,
            .height = pd_app_config.matrix_height,
            .orientation_deg = pd_app_config.orientation_deg,
            .scan_wiring = pd_app_config.scan_wiring
        };
    } else if (!pd_app_config.setup_complete) {
        display_config = (pd_display_config_t){
            .width = 32,
            .height = 32,
            .orientation_deg = 0,
            .scan_wiring = 0
        };
    } else {
        display_config = (pd_display_config_t){
            .width = pd_app_config.matrix_width > 0 ? pd_app_config.matrix_width : 64,
            .height = pd_app_config.matrix_height > 0 ? pd_app_config.matrix_height : 64,
            .orientation_deg = pd_app_config.orientation_deg,
            .scan_wiring = pd_app_config.scan_wiring
        };
    }
    if (pd_app_config.timezone[0] != '\0') {
        setenv("TZ", pd_app_config.timezone, 1);
        tzset();
    }
    strlcpy(network_config.wifi_ssid, pd_app_config.wifi_ssid, sizeof(network_config.wifi_ssid));
    strlcpy(network_config.wifi_password, pd_app_config.wifi_password, sizeof(network_config.wifi_password));
    strlcpy(network_config.hostname, pd_app_config.hostname, sizeof(network_config.hostname));
    strlcpy(network_config.static_ip, pd_app_config.static_ip, sizeof(network_config.static_ip));
    strlcpy(network_config.static_gateway, pd_app_config.static_gateway, sizeof(network_config.static_gateway));
    strlcpy(network_config.static_netmask, pd_app_config.static_netmask, sizeof(network_config.static_netmask));
    pd_display_init(&display_config);
    pd_display_render_boot_message();
    pd_network_init(&network_config);
    pd_network_start();
    pd_content_init(pd_storage_get_base_path());
    pd_wizard_start(&pd_app_config);
    if (pd_app_config.setup_complete) {
        pd_display_render_idle(
            pd_app_config.device_name,
            pd_app_config.matrix_width,
            pd_app_config.matrix_height,
            pd_app_config.orientation_deg,
            pd_app_config.scan_wiring,
            pd_app_config.static_ip
        );
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    initialize_system();

    bool idle_ip_updated = false;
    bool content_http_registered = false;

    while (true) {
        pd_network_poll();
        pd_wizard_tick();
        pd_content_tick();

        /* once WiFi connects, re-render idle screen with actual IP
         * and register content HTTP endpoints */
        if (pd_app_config.setup_complete && !idle_ip_updated && pd_network_is_connected()) {
            const char *ip = pd_network_get_ip();
            if (ip) {
                pd_display_render_idle(
                    pd_app_config.device_name,
                    pd_app_config.matrix_width,
                    pd_app_config.matrix_height,
                    pd_app_config.orientation_deg,
                    pd_app_config.scan_wiring,
                    ip
                );
                idle_ip_updated = true;
            }
        }

        if (!content_http_registered) {
            httpd_handle_t server = pd_network_get_http_server();
            if (server) {
                pd_content_register_http(server);
                content_http_registered = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
