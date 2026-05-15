#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd-config.h"
#include "pd-content.h"
#include "pd-display.h"
#include "pd-network.h"
#include "pd-storage.h"
#include "pd-serial-cmd.h"
#include "pd-wizard.h"
#include "pd-discovery.h"

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
    pd_config_set_active(&pd_app_config);

    ESP_LOGI(TAG, "config: virtual=%dx%d panel=%dx%d chain=%dx%d pat=%d rot=%d scan=%d complete=%d reztest=%d[%d]",
             pd_app_config.matrix_width, pd_app_config.matrix_height,
             pd_app_config.panel_width, pd_app_config.panel_height,
             pd_app_config.panel_rows, pd_app_config.panel_cols,
             pd_app_config.chain_pattern, pd_app_config.panel_rotation_deg,
             pd_app_config.scan_wiring,
             pd_app_config.setup_complete,
             pd_app_config.reztest_mode, pd_app_config.reztest_index);

    pd_display_config_t display_config;
    int rz_w, rz_h, rz_scan;
    if (pd_wizard_reztest_get_display(&pd_app_config, &rz_w, &rz_h, &rz_scan)) {
        /* reztest mode: init display with the current combo */
        display_config = (pd_display_config_t){
            .panel_width = rz_w,
            .panel_height = rz_h,
            .panel_rows = 1,
            .panel_cols = 1,
            .chain_pattern = 0,
            .rotation_deg = 0,
            .scan_wiring = rz_scan,
            .color_order = 0
        };
    } else if (!pd_app_config.setup_complete && pd_app_config.reztest_done
               && pd_app_config.matrix_width > 0 && pd_app_config.matrix_height > 0) {
        /* returning from reztest lock-in: use the chosen resolution during wizard */
        display_config = (pd_display_config_t){
            .panel_width = pd_app_config.panel_width > 0 ? pd_app_config.panel_width : pd_app_config.matrix_width,
            .panel_height = pd_app_config.panel_height > 0 ? pd_app_config.panel_height : pd_app_config.matrix_height,
            .panel_rows = pd_app_config.panel_rows > 0 ? pd_app_config.panel_rows : 1,
            .panel_cols = pd_app_config.panel_cols > 0 ? pd_app_config.panel_cols : 1,
            .chain_pattern = pd_app_config.chain_pattern,
            .rotation_deg = pd_app_config.panel_rotation_deg,
            .scan_wiring = pd_app_config.scan_wiring,
            .color_order = pd_app_config.color_order
        };
    } else if (!pd_app_config.setup_complete) {
        display_config = (pd_display_config_t){
            .panel_width = 32,
            .panel_height = 32,
            .panel_rows = 1,
            .panel_cols = 1,
            .chain_pattern = 0,
            .rotation_deg = 0,
            .scan_wiring = 0,
            .color_order = 0
        };
    } else {
        display_config = (pd_display_config_t){
            .panel_width = pd_app_config.panel_width > 0 ? pd_app_config.panel_width : (pd_app_config.matrix_width > 0 ? pd_app_config.matrix_width : 64),
            .panel_height = pd_app_config.panel_height > 0 ? pd_app_config.panel_height : (pd_app_config.matrix_height > 0 ? pd_app_config.matrix_height : 64),
            .panel_rows = pd_app_config.panel_rows > 0 ? pd_app_config.panel_rows : 1,
            .panel_cols = pd_app_config.panel_cols > 0 ? pd_app_config.panel_cols : 1,
            .chain_pattern = pd_app_config.chain_pattern,
            .rotation_deg = pd_app_config.panel_rotation_deg,
            .scan_wiring = pd_app_config.scan_wiring,
            .color_order = pd_app_config.color_order
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
    if (pd_display_init(&display_config) != ESP_OK) {
        ESP_LOGW(TAG, "display init failed with saved config — falling back to 64x32 safe mode");
        pd_display_config_t safe_config = {
            .panel_width   = 64,
            .panel_height  = 32,
            .panel_rows    = 1,
            .panel_cols    = 1,
            .chain_pattern = 0,
            .rotation_deg  = 0,
            .scan_wiring   = 0,
            .color_order   = 0
        };
        pd_display_init(&safe_config);
    }
    pd_display_render_boot_message();
    pd_network_init(&network_config);
    pd_network_start();
    pd_content_init(pd_storage_get_base_path());
    pd_discovery_init();
    pd_wizard_start(&pd_app_config);
    pd_serial_cmd_init();
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
    bool boot_status_triggered = false;
    int64_t network_up_time_us = 0;

    while (true) {
        pd_network_poll();
        pd_wizard_tick();
        pd_content_tick();
        pd_display_test_tick();

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
                network_up_time_us = esp_timer_get_time();
            }
        }

        if (!content_http_registered) {
            httpd_handle_t server = pd_network_get_http_server();
            if (server) {
                pd_content_register_http(server);
                content_http_registered = true;
            }
        }

        /* One-shot boot-time verbose status screen:
         * After network is up, wait for either the first daemon announcement
         * OR up to ~8 seconds, then show the status screen for 5 seconds.
         * Never shown again automatically — use POST /api/status/show for on-demand. */
        if (pd_app_config.setup_complete && idle_ip_updated && !boot_status_triggered) {
            int64_t elapsed_us = esp_timer_get_time() - network_up_time_us;
            bool have_source = pd_discovery_has_source();
            if (have_source || elapsed_us > 8000000) {
                pd_content_show_source_status_for(5000);
                boot_status_triggered = true;
            }
        }

        /* Render idle screen when the status overlay expires with nothing
         * playing (pd-content can't render idle itself — needs device config). */
        if (pd_content_status_overlay_just_expired()) {
            const char *ip = pd_network_get_ip();
            pd_display_render_idle(
                pd_app_config.device_name,
                pd_app_config.matrix_width,
                pd_app_config.matrix_height,
                pd_app_config.orientation_deg,
                pd_app_config.scan_wiring,
                ip ? ip : pd_app_config.static_ip
            );
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
