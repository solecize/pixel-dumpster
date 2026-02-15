#include "pd-wizard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "pd-network.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pd-display.h"

#if CONFIG_TINYUSB_HOST
#include "tusb.h"
#include "tinyusb.h"
#endif

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "pd-wizard";

#define PD_WIZARD_MAX_SSIDS 32
#define PD_WIZARD_LINE_BUF 1024
#define PD_WIZARD_VALUE_LEN 128

/* ---------- step definitions ---------- */

typedef enum {
    PD_STEP_MATRIX_SIZE = 0,
    PD_STEP_MATRIX_CUSTOM,
    PD_STEP_ORIENTATION,
    PD_STEP_SCAN_WIRING,
    PD_STEP_WIFI_SSID,
    PD_STEP_WIFI_SSID_MANUAL,
    PD_STEP_WIFI_PASSWORD,
    PD_STEP_DEVICE_NAME,
    PD_STEP_HOSTNAME,
    PD_STEP_TIMEZONE,
    PD_STEP_STATIC_IP,
    PD_STEP_STATIC_GATEWAY,
    PD_STEP_STATIC_NETMASK,
    PD_STEP_COUNT
} pd_wizard_step_t;

typedef enum {
    PD_MODE_MENU = 0,
    PD_MODE_TEXT
} pd_step_mode_t;

typedef struct {
    const char *id;
    const char *title;
    pd_step_mode_t mode;
    bool mask;
} pd_step_def_t;

static const pd_step_def_t pd_step_defs[PD_STEP_COUNT] = {
    [PD_STEP_MATRIX_SIZE]      = {"matrix_size",      "Matrix size",                    PD_MODE_MENU, false},
    [PD_STEP_MATRIX_CUSTOM]    = {"matrix_custom",     "Custom size (e.g. 128x64)",     PD_MODE_TEXT, false},
    [PD_STEP_ORIENTATION]      = {"orientation",       "Orientation",                    PD_MODE_MENU, false},
    [PD_STEP_SCAN_WIRING]      = {"scan_wiring",       "Scan wiring",                   PD_MODE_MENU, false},
    [PD_STEP_WIFI_SSID]        = {"wifi_ssid",         "WiFi SSID",                     PD_MODE_MENU, false},
    [PD_STEP_WIFI_SSID_MANUAL] = {"wifi_ssid_manual",  "Enter WiFi SSID",               PD_MODE_TEXT, false},
    [PD_STEP_WIFI_PASSWORD]    = {"wifi_password",     "WiFi password (blank=open)",     PD_MODE_TEXT, true},
    [PD_STEP_DEVICE_NAME]      = {"device_name",       "Device name",                   PD_MODE_TEXT, false},
    [PD_STEP_HOSTNAME]         = {"hostname",          "Hostname",                      PD_MODE_TEXT, false},
    [PD_STEP_TIMEZONE]         = {"timezone",          "Timezone (e.g. CST6CDT)",       PD_MODE_TEXT, false},
    [PD_STEP_STATIC_IP]        = {"static_ip",         "Static IP (blank=DHCP)",        PD_MODE_TEXT, false},
    [PD_STEP_STATIC_GATEWAY]   = {"static_gateway",    "Static gateway",                PD_MODE_TEXT, false},
    [PD_STEP_STATIC_NETMASK]   = {"static_netmask",    "Static netmask",                PD_MODE_TEXT, false},
};

static const char *pd_matrix_options[] = {"16x16", "32x32", "64x64", "128x128", "other", "reztest"};
static const int pd_matrix_option_count = 6;
static const int pd_matrix_sizes[][2] = {{16,16},{32,32},{64,64},{128,128},{0,0},{0,0}};

static const char *pd_orientation_options[] = {"0", "90", "180", "270"};
static const int pd_orientation_option_count = 4;

static const char *pd_scan_options[] = {"Standard (1/16 or 1/32)", "1/4 scan 16px", "1/8 scan 32px", "1/8 scan 40px", "1/8 scan 64px"};
static const int pd_scan_option_count = 5;

static const char *pd_manual_entry_label = "<manual entry>";

/* ---------- reztest combo table ---------- */
typedef struct {
    int width;
    int height;
    int scan_wiring;
    const char *label;
} reztest_combo_t;

static const reztest_combo_t reztest_combos[] = {
    { 16,  16, 0, "16x16 std"    },
    { 32,  32, 0, "32x32 std"    },
    { 64,  32, 0, "64x32 std"    },
    { 64,  64, 0, "64x64 std"    },
    { 64,  64, 1, "64x64 1/4"    },
    { 64,  64, 2, "64x64 1/8-32" },
    { 64,  64, 3, "64x64 1/8-40" },
    { 64,  64, 4, "64x64 1/8-64" },
    {128,  64, 0, "128x64 std"   },
    {128, 128, 0, "128x128 std"  },
};
#define REZTEST_COMBO_COUNT (int)(sizeof(reztest_combos) / sizeof(reztest_combos[0]))

static bool wiz_reztest_active = false;  /* true while in reztest mode */

/* ---------- wizard state ---------- */

static pd_config_t *wiz_config = NULL;
static pd_wizard_state_t wiz_public_state = PD_WIZARD_STATE_IDLE;
static pd_wizard_step_t wiz_step = PD_STEP_MATRIX_SIZE;
static int wiz_menu_selected = 0;
static char wiz_text_value[PD_WIZARD_VALUE_LEN] = {0};

static char wiz_wifi_ssids[PD_WIZARD_MAX_SSIDS][PD_WIZARD_VALUE_LEN];
static int wiz_wifi_ssid_count = 0;
static const char *wiz_wifi_menu_ptrs[PD_WIZARD_MAX_SSIDS + 1];
static int wiz_wifi_menu_count = 0;
static bool wiz_wifi_scanned = false;

/* serial line buffer */
static char wiz_line_buf[PD_WIZARD_LINE_BUF];
static size_t wiz_line_len = 0;

/* serial I/O state */
static bool wiz_uart_started = false;
static bool wiz_uart_warned = false;
static bool wiz_usb_serial_started = false;
static bool wiz_usb_serial_warned = false;
#if CONFIG_TINYUSB_HOST
static bool wiz_usb_started = false;
#endif
static bool wiz_usb_warned = false;

/* ---------- serial output ---------- */

static void wiz_serial_write(const char *data, size_t len)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (wiz_usb_serial_started) {
        usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(50));
        return;
    }
#endif
    if (wiz_uart_started) {
        uart_write_bytes(UART_NUM_0, data, len);
    }
}

static void wiz_send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        wiz_serial_write(str, strlen(str));
        wiz_serial_write("\n", 1);
        free(str);
    }
    cJSON_Delete(root);
}

/* ---------- step helpers ---------- */

static bool wiz_step_active(pd_wizard_step_t step)
{
    switch (step) {
        case PD_STEP_MATRIX_SIZE:
        case PD_STEP_ORIENTATION:
        case PD_STEP_SCAN_WIRING:
            /* skip display steps if reztest already locked in a config */
            return !(wiz_config && wiz_config->reztest_done);
        case PD_STEP_MATRIX_CUSTOM:
            return wiz_config && wiz_config->matrix_width == 0;
        case PD_STEP_WIFI_SSID_MANUAL:
            return false;
        case PD_STEP_STATIC_GATEWAY:
        case PD_STEP_STATIC_NETMASK:
            return wiz_config && wiz_config->static_ip[0] != '\0';
        default:
            return true;
    }
}

static int wiz_active_step_index(void)
{
    int idx = 0;
    for (int i = 0; i < (int)wiz_step; i++) {
        if (wiz_step_active((pd_wizard_step_t)i)) {
            idx++;
        }
    }
    return idx;
}

static int wiz_active_step_count(void)
{
    int count = 0;
    for (int i = 0; i < PD_STEP_COUNT; i++) {
        if (wiz_step_active((pd_wizard_step_t)i)) {
            count++;
        }
    }
    return count;
}

static void wiz_get_menu_options(const char ***out_options, int *out_count)
{
    switch (wiz_step) {
        case PD_STEP_MATRIX_SIZE:
            *out_options = pd_matrix_options;
            *out_count = pd_matrix_option_count;
            break;
        case PD_STEP_ORIENTATION:
            *out_options = pd_orientation_options;
            *out_count = pd_orientation_option_count;
            break;
        case PD_STEP_SCAN_WIRING:
            *out_options = pd_scan_options;
            *out_count = pd_scan_option_count;
            break;
        case PD_STEP_WIFI_SSID:
            *out_options = wiz_wifi_menu_ptrs;
            *out_count = wiz_wifi_menu_count;
            break;
        default:
            *out_options = NULL;
            *out_count = 0;
            break;
    }
}

static const char *wiz_get_text_value(void)
{
    switch (wiz_step) {
        case PD_STEP_MATRIX_CUSTOM: {
            static char buf[32];
            if (wiz_config->matrix_width > 0 && wiz_config->matrix_height > 0) {
                snprintf(buf, sizeof(buf), "%dx%d", wiz_config->matrix_width, wiz_config->matrix_height);
                return buf;
            }
            return wiz_text_value;
        }
        case PD_STEP_WIFI_SSID_MANUAL: return wiz_config->wifi_ssid;
        case PD_STEP_WIFI_PASSWORD:    return wiz_config->wifi_password;
        case PD_STEP_DEVICE_NAME:      return wiz_config->device_name;
        case PD_STEP_HOSTNAME:         return wiz_config->hostname;
        case PD_STEP_TIMEZONE:         return wiz_config->timezone;
        case PD_STEP_STATIC_IP:        return wiz_config->static_ip;
        case PD_STEP_STATIC_GATEWAY:   return wiz_config->static_gateway;
        case PD_STEP_STATIC_NETMASK:   return wiz_config->static_netmask;
        default: return wiz_text_value;
    }
}

/* ---------- send state to CLI ---------- */

static void wiz_send_state(void)
{
    const pd_step_def_t *def = &pd_step_defs[wiz_step];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddStringToObject(root, "step", def->id);
    cJSON_AddNumberToObject(root, "step_index", wiz_active_step_index());
    cJSON_AddNumberToObject(root, "step_count", wiz_active_step_count());
    cJSON_AddStringToObject(root, "title", def->title);

    if (def->mode == PD_MODE_MENU) {
        cJSON_AddStringToObject(root, "mode", "menu");
        const char **options = NULL;
        int count = 0;
        wiz_get_menu_options(&options, &count);
        cJSON *arr = cJSON_AddArrayToObject(root, "options");
        for (int i = 0; i < count; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(options[i]));
        }
        cJSON_AddNumberToObject(root, "selected", wiz_menu_selected);
    } else {
        cJSON_AddStringToObject(root, "mode", "text");
        const char *val = wiz_get_text_value();
        if (def->mask && val && val[0]) {
            char masked[PD_WIZARD_VALUE_LEN];
            size_t len = strlen(val);
            if (len >= sizeof(masked)) len = sizeof(masked) - 1;
            memset(masked, '*', len);
            masked[len] = '\0';
            cJSON_AddStringToObject(root, "value", masked);
        } else {
            cJSON_AddStringToObject(root, "value", val ? val : "");
        }
        cJSON_AddBoolToObject(root, "mask", def->mask);
    }

    cJSON *nav = cJSON_AddObjectToObject(root, "nav");
    cJSON_AddBoolToObject(nav, "back", wiz_step > PD_STEP_MATRIX_SIZE);
    cJSON_AddBoolToObject(nav, "next", true);

    wiz_send_json(root);
}

/* ---------- HUB75 mirror ---------- */

static void wiz_render_display(void)
{
    const pd_step_def_t *def = &pd_step_defs[wiz_step];

    if (def->mode == PD_MODE_MENU) {
        const char **options = NULL;
        int count = 0;
        wiz_get_menu_options(&options, &count);
        pd_display_wizard_menu(def->title, options, count, wiz_menu_selected);
    } else {
        const char *val = wiz_get_text_value();
        pd_display_wizard_text(def->title, val, def->mask);
    }
}

/* ---------- Wi-Fi helpers ---------- */

static bool wiz_wifi_inited = false;

static void wiz_ensure_wifi_init(void)
{
    if (wiz_wifi_inited) return;

    /* check if WiFi is already started (e.g. by pd_network_start) */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        /* WiFi stack already running — nothing to do */
        wiz_wifi_inited = true;
        ESP_LOGI(TAG, "wifi already running (mode=%d), reusing", (int)mode);
        return;
    }

    /* first-time init: netif + event loop + wifi */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    wiz_wifi_inited = true;
    ESP_LOGI(TAG, "wifi initialized for wizard scan");
}

/* ---------- Wi-Fi connection test ---------- */

static volatile bool wiz_wifi_got_ip = false;
static volatile bool wiz_wifi_connect_fail = false;
static char wiz_wifi_ip_str[16] = {0};

static void wiz_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wiz_wifi_connect_fail = true;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(wiz_wifi_ip_str, sizeof(wiz_wifi_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        wiz_wifi_got_ip = true;
    }
}

static bool wiz_test_wifi_connection(const char *ssid, const char *password)
{
    wiz_ensure_wifi_init();

    wiz_wifi_got_ip = false;
    wiz_wifi_connect_fail = false;
    wiz_wifi_ip_str[0] = '\0';

    /* register temporary event handlers */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wiz_wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wiz_wifi_event_handler, NULL);

    /* disconnect if currently connected */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* reset flags AFTER disconnect settles — the disconnect event
       would have set wiz_wifi_connect_fail, so clear it now */
    wiz_wifi_got_ip = false;
    wiz_wifi_connect_fail = false;

    /* configure and connect */
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    if (password[0] != '\0') {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    /* wait up to 15 seconds for IP */
    for (int i = 0; i < 150; i++) {
        if (wiz_wifi_got_ip || wiz_wifi_connect_fail) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* unregister handlers */
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wiz_wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wiz_wifi_event_handler);

    return wiz_wifi_got_ip;
}

/* ---------- Wi-Fi scan ---------- */

static void wiz_do_wifi_scan(void)
{
    wiz_ensure_wifi_init();
    wiz_wifi_ssid_count = 0;
    wiz_wifi_menu_count = 0;

    cJSON *scanning = cJSON_CreateObject();
    cJSON_AddStringToObject(scanning, "type", "wifi_scan");
    cJSON_AddBoolToObject(scanning, "scanning", true);
    cJSON *empty = cJSON_AddArrayToObject(scanning, "ssids");
    (void)empty;
    wiz_send_json(scanning);

    pd_display_wizard_status("Scanning...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } }
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
    }

    /* let any pending log output flush before we send JSON */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > PD_WIZARD_MAX_SSIDS) {
        ap_count = PD_WIZARD_MAX_SSIDS;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);
        for (int i = 0; i < ap_count; i++) {
            const char *ssid = (const char *)ap_records[i].ssid;
            if (ssid[0] == '\0') continue;
            bool dup = false;
            for (int j = 0; j < wiz_wifi_ssid_count; j++) {
                if (strcmp(wiz_wifi_ssids[j], ssid) == 0) { dup = true; break; }
            }
            if (!dup && wiz_wifi_ssid_count < PD_WIZARD_MAX_SSIDS) {
                strlcpy(wiz_wifi_ssids[wiz_wifi_ssid_count], ssid, PD_WIZARD_VALUE_LEN);
                wiz_wifi_ssid_count++;
            }
        }
        free(ap_records);
    }

    wiz_wifi_menu_count = 0;
    for (int i = 0; i < wiz_wifi_ssid_count; i++) {
        wiz_wifi_menu_ptrs[wiz_wifi_menu_count++] = wiz_wifi_ssids[i];
    }
    wiz_wifi_menu_ptrs[wiz_wifi_menu_count++] = pd_manual_entry_label;
    wiz_wifi_scanned = true;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "type", "wifi_scan");
    cJSON_AddBoolToObject(result, "scanning", false);
    cJSON *ssids_arr = cJSON_AddArrayToObject(result, "ssids");
    for (int i = 0; i < wiz_wifi_ssid_count; i++) {
        cJSON_AddItemToArray(ssids_arr, cJSON_CreateString(wiz_wifi_ssids[i]));
    }
    wiz_send_json(result);
}

/* ---------- forward declarations ---------- */

static void wiz_go_next(void);
static void wiz_go_back(void);
static void wiz_enter_step(void);

static void wiz_enter_step(void)
{
    while (wiz_step < PD_STEP_COUNT && !wiz_step_active(wiz_step)) {
        wiz_step++;
    }

    if (wiz_step >= PD_STEP_COUNT) {
        wiz_public_state = PD_WIZARD_STATE_COMPLETE;
        wiz_config->setup_complete = true;
        wiz_config->reztest_done = false;
        pd_config_save(wiz_config);

        cJSON *done = cJSON_CreateObject();
        cJSON_AddStringToObject(done, "type", "complete");
        cJSON *cfg = cJSON_AddObjectToObject(done, "config");
        cJSON_AddNumberToObject(cfg, "matrix_width", wiz_config->matrix_width);
        cJSON_AddNumberToObject(cfg, "matrix_height", wiz_config->matrix_height);
        cJSON_AddNumberToObject(cfg, "orientation_deg", wiz_config->orientation_deg);
        cJSON_AddNumberToObject(cfg, "scan_wiring", wiz_config->scan_wiring);
        cJSON_AddStringToObject(cfg, "wifi_ssid", wiz_config->wifi_ssid);
        cJSON_AddStringToObject(cfg, "device_name", wiz_config->device_name);
        cJSON_AddStringToObject(cfg, "hostname", wiz_config->hostname);
        cJSON_AddStringToObject(cfg, "timezone", wiz_config->timezone);
        cJSON_AddStringToObject(cfg, "static_ip", wiz_config->static_ip);
        cJSON_AddStringToObject(cfg, "static_gateway", wiz_config->static_gateway);
        cJSON_AddStringToObject(cfg, "static_netmask", wiz_config->static_netmask);
        wiz_send_json(done);
        vTaskDelay(pdMS_TO_TICKS(100));

        pd_display_wizard_status("Rebooting...");
        ESP_LOGI(TAG, "wizard complete — saved: %dx%d orient=%d scan=%d reztest_done=%d",
                 wiz_config->matrix_width, wiz_config->matrix_height,
                 wiz_config->orientation_deg, wiz_config->scan_wiring,
                 wiz_config->reztest_done);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    wiz_menu_selected = 0;
    wiz_text_value[0] = '\0';

    if (wiz_step == PD_STEP_WIFI_SSID && !wiz_wifi_scanned) {
        wiz_do_wifi_scan();
    }

    if (wiz_step == PD_STEP_MATRIX_SIZE) {
        if (wiz_config->matrix_width == 64 && wiz_config->matrix_height == 64) wiz_menu_selected = 2;
        else if (wiz_config->matrix_width == 32 && wiz_config->matrix_height == 32) wiz_menu_selected = 1;
        else if (wiz_config->matrix_width == 16 && wiz_config->matrix_height == 16) wiz_menu_selected = 0;
        else if (wiz_config->matrix_width == 128 && wiz_config->matrix_height == 128) wiz_menu_selected = 3;
    }

    if (wiz_step == PD_STEP_ORIENTATION) {
        for (int i = 0; i < pd_orientation_option_count; i++) {
            if (atoi(pd_orientation_options[i]) == wiz_config->orientation_deg) {
                wiz_menu_selected = i;
                break;
            }
        }
    }

    if (wiz_step == PD_STEP_SCAN_WIRING) {
        if (wiz_config->scan_wiring >= 0 && wiz_config->scan_wiring < pd_scan_option_count) {
            wiz_menu_selected = wiz_config->scan_wiring;
        }
    }

    if (wiz_step == PD_STEP_WIFI_SSID) {
        wiz_menu_selected = wiz_wifi_menu_count - 1;
        for (int i = 0; i < wiz_wifi_menu_count - 1; i++) {
            if (strcmp(wiz_wifi_menu_ptrs[i], wiz_config->wifi_ssid) == 0) {
                wiz_menu_selected = i;
                break;
            }
        }
    }

    wiz_send_state();
    wiz_render_display();
}

static void wiz_go_next(void)
{
    wiz_step++;
    wiz_enter_step();
}

static void wiz_go_back(void)
{
    if (wiz_step == PD_STEP_MATRIX_SIZE) return;
    wiz_step--;
    while (wiz_step > PD_STEP_MATRIX_SIZE && !wiz_step_active(wiz_step)) {
        wiz_step--;
    }
    wiz_enter_step();
}

/* ---------- command: apply menu selection ---------- */

static void wiz_apply_menu_select(int index)
{
    const char **options = NULL;
    int count = 0;
    wiz_get_menu_options(&options, &count);
    if (index < 0 || index >= count) return;

    switch (wiz_step) {
        case PD_STEP_MATRIX_SIZE: {
            /* "reztest" is the last option */
            if (index == pd_matrix_option_count - 1) {
                ESP_LOGI(TAG, "reztest selected — saving and rebooting");
                wiz_config->reztest_mode = true;
                wiz_config->reztest_index = 0;
                pd_config_save(wiz_config);

                /* tell CLI that reztest is starting so it enters reconnect loop */
                cJSON *rt = cJSON_CreateObject();
                cJSON_AddStringToObject(rt, "type", "reztest_starting");
                wiz_send_json(rt);

                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
                return;
            }
            /* "other" is second-to-last */
            if (index == pd_matrix_option_count - 2) {
                wiz_config->matrix_width = 0;
                wiz_config->matrix_height = 0;
                wiz_go_next();
                return;
            }
            wiz_config->matrix_width = pd_matrix_sizes[index][0];
            wiz_config->matrix_height = pd_matrix_sizes[index][1];
            wiz_go_next();
            break;
        }
        case PD_STEP_ORIENTATION:
            wiz_config->orientation_deg = atoi(pd_orientation_options[index]);
            wiz_go_next();
            break;
        case PD_STEP_SCAN_WIRING:
            wiz_config->scan_wiring = index;
            wiz_go_next();
            break;
        case PD_STEP_WIFI_SSID:
            if (index == wiz_wifi_menu_count - 1) {
                wiz_step = PD_STEP_WIFI_SSID_MANUAL;
                wiz_enter_step();
                return;
            }
            strlcpy(wiz_config->wifi_ssid, wiz_wifi_menu_ptrs[index], sizeof(wiz_config->wifi_ssid));
            wiz_step = PD_STEP_WIFI_PASSWORD;
            wiz_enter_step();
            break;
        default:
            break;
    }
}

/* ---------- command: apply text input ---------- */

static void wiz_apply_text_input(const char *value)
{
    switch (wiz_step) {
        case PD_STEP_MATRIX_CUSTOM: {
            int w = 0, h = 0;
            if (sscanf(value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                wiz_config->matrix_width = w;
                wiz_config->matrix_height = h;
                wiz_go_next();
            } else {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Invalid size. Use WIDTHxHEIGHT (e.g. 128x64)");
                wiz_send_json(err);
            }
            break;
        }
        case PD_STEP_WIFI_SSID_MANUAL:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "SSID cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->wifi_ssid, value, sizeof(wiz_config->wifi_ssid));
            wiz_step = PD_STEP_WIFI_PASSWORD;
            wiz_enter_step();
            break;
        case PD_STEP_WIFI_PASSWORD:
            strlcpy(wiz_config->wifi_password, value, sizeof(wiz_config->wifi_password));

            /* test WiFi connection before advancing */
            {
                ESP_LOGI(TAG, "wifi test: ssid='%s' pass='%s'", wiz_config->wifi_ssid, wiz_config->wifi_password);

                /* suspend pd_network auto-reconnect so it doesn't interfere */
                pd_network_suspend();

                cJSON *testing = cJSON_CreateObject();
                cJSON_AddStringToObject(testing, "type", "wifi_test");
                cJSON_AddBoolToObject(testing, "testing", true);
                cJSON_AddStringToObject(testing, "ssid", wiz_config->wifi_ssid);
                wiz_send_json(testing);

                pd_display_wizard_status("Connecting...");

                bool ok = wiz_test_wifi_connection(wiz_config->wifi_ssid, wiz_config->wifi_password);

                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "type", "wifi_test");
                cJSON_AddBoolToObject(result, "testing", false);
                cJSON_AddBoolToObject(result, "success", ok);
                if (ok) {
                    cJSON_AddStringToObject(result, "ip", wiz_wifi_ip_str);
                    ESP_LOGI(TAG, "wifi test OK — IP: %s", wiz_wifi_ip_str);
                    pd_display_wizard_status("Connected!");
                } else {
                    ESP_LOGW(TAG, "wifi test FAILED for SSID: %s", wiz_config->wifi_ssid);
                    pd_display_wizard_status("WiFi failed!");
                }
                wiz_send_json(result);

                vTaskDelay(pdMS_TO_TICKS(1500));

                /* resume pd_network auto-reconnect */
                pd_network_resume();

                if (!ok) {
                    /* go back to SSID selection so user can retry */
                    wiz_step = PD_STEP_WIFI_SSID;
                    wiz_enter_step();
                    return;
                }
            }
            wiz_go_next();
            break;
        case PD_STEP_DEVICE_NAME:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Device name cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->device_name, value, sizeof(wiz_config->device_name));
            wiz_go_next();
            break;
        case PD_STEP_HOSTNAME:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Hostname cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->hostname, value, sizeof(wiz_config->hostname));
            wiz_go_next();
            break;
        case PD_STEP_TIMEZONE:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Timezone cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->timezone, value, sizeof(wiz_config->timezone));
            wiz_go_next();
            break;
        case PD_STEP_STATIC_IP:
            strlcpy(wiz_config->static_ip, value, sizeof(wiz_config->static_ip));
            if (value[0] == '\0') {
                wiz_config->static_gateway[0] = '\0';
                wiz_config->static_netmask[0] = '\0';
            }
            wiz_go_next();
            break;
        case PD_STEP_STATIC_GATEWAY:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Gateway cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->static_gateway, value, sizeof(wiz_config->static_gateway));
            wiz_go_next();
            break;
        case PD_STEP_STATIC_NETMASK:
            if (value[0] == '\0') {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "error");
                cJSON_AddStringToObject(err, "message", "Netmask cannot be empty");
                wiz_send_json(err);
                return;
            }
            strlcpy(wiz_config->static_netmask, value, sizeof(wiz_config->static_netmask));
            wiz_go_next();
            break;
        default:
            break;
    }
}

/* ---------- command: handle key ---------- */

static void wiz_handle_key(const char *code)
{
    const pd_step_def_t *def = &pd_step_defs[wiz_step];

    if (strcmp(code, "<") == 0) {
        wiz_go_back();
        return;
    }
    if (strcmp(code, ">") == 0) {
        if (def->mode == PD_MODE_MENU) {
            wiz_apply_menu_select(wiz_menu_selected);
        } else {
            wiz_apply_text_input(wiz_get_text_value());
        }
        return;
    }

    if (def->mode == PD_MODE_MENU) {
        const char **options = NULL;
        int count = 0;
        wiz_get_menu_options(&options, &count);

        if (strcmp(code, "up") == 0) {
            wiz_menu_selected = (wiz_menu_selected - 1 + count) % count;
            wiz_send_state();
            wiz_render_display();
        } else if (strcmp(code, "down") == 0) {
            wiz_menu_selected = (wiz_menu_selected + 1) % count;
            wiz_send_state();
            wiz_render_display();
        } else if (strcmp(code, "enter") == 0) {
            wiz_apply_menu_select(wiz_menu_selected);
        } else if (strlen(code) == 1 && code[0] >= '1' && code[0] <= '9') {
            int idx = code[0] - '1';
            if (idx < count) {
                wiz_menu_selected = idx;
                wiz_apply_menu_select(idx);
            }
        }
    } else {
        if (strcmp(code, "enter") == 0) {
            wiz_apply_text_input(wiz_text_value);
        } else if (strcmp(code, "backspace") == 0) {
            size_t len = strlen(wiz_text_value);
            if (len > 0) {
                wiz_text_value[len - 1] = '\0';
                wiz_send_state();
                wiz_render_display();
            }
        } else if (strlen(code) == 1) {
            size_t len = strlen(wiz_text_value);
            if (len + 1 < sizeof(wiz_text_value)) {
                wiz_text_value[len] = code[0];
                wiz_text_value[len + 1] = '\0';
                wiz_send_state();
                wiz_render_display();
            }
        }
    }
}

/* ---------- JSON command dispatch ---------- */

static void wiz_process_command(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd->valuestring;

    if (strcmp(cmd_str, "hello") == 0) {
        cJSON *force_item = cJSON_GetObjectItem(root, "force");
        bool force = cJSON_IsTrue(force_item);

        if (wiz_reztest_active) {
            /* send reztest status instead of normal wizard state */
            int idx = wiz_config->reztest_index;
            const reztest_combo_t *c = (idx >= 0 && idx < REZTEST_COMBO_COUNT) ? &reztest_combos[idx] : NULL;
            cJSON *rt = cJSON_CreateObject();
            cJSON_AddStringToObject(rt, "type", "reztest_status");
            cJSON_AddNumberToObject(rt, "index", idx);
            cJSON_AddNumberToObject(rt, "total", REZTEST_COMBO_COUNT);
            cJSON_AddStringToObject(rt, "label", c ? c->label : "?");
            cJSON_AddNumberToObject(rt, "width", c ? c->width : 0);
            cJSON_AddNumberToObject(rt, "height", c ? c->height : 0);
            cJSON_AddNumberToObject(rt, "scan_wiring", c ? c->scan_wiring : 0);
            wiz_send_json(rt);
            ESP_LOGI(TAG, "CLI connected (hello) — sent reztest_status %d/%d", idx + 1, REZTEST_COMBO_COUNT);
        } else if (wiz_config->setup_complete && !force) {
            /* already configured — tell CLI we're done */
            ESP_LOGI(TAG, "CLI connected (hello) — setup already complete");
            cJSON *done = cJSON_CreateObject();
            cJSON_AddStringToObject(done, "type", "complete");
            cJSON *cfg = cJSON_AddObjectToObject(done, "config");
            cJSON_AddNumberToObject(cfg, "matrix_width", wiz_config->matrix_width);
            cJSON_AddNumberToObject(cfg, "matrix_height", wiz_config->matrix_height);
            cJSON_AddNumberToObject(cfg, "orientation_deg", wiz_config->orientation_deg);
            cJSON_AddNumberToObject(cfg, "scan_wiring", wiz_config->scan_wiring);
            cJSON_AddStringToObject(cfg, "wifi_ssid", wiz_config->wifi_ssid);
            cJSON_AddStringToObject(cfg, "device_name", wiz_config->device_name);
            cJSON_AddStringToObject(cfg, "hostname", wiz_config->hostname);
            cJSON_AddStringToObject(cfg, "timezone", wiz_config->timezone);
            cJSON_AddStringToObject(cfg, "static_ip", wiz_config->static_ip);
            wiz_send_json(done);
        } else {
            ESP_LOGI(TAG, "CLI connected (hello) — resetting wizard");
            wiz_public_state = PD_WIZARD_STATE_MATRIX_SIZE;
            wiz_step = PD_STEP_MATRIX_SIZE;
            wiz_menu_selected = 0;
            wiz_text_value[0] = '\0';
            wiz_wifi_scanned = false;
            wiz_enter_step();
        }
    } else if (strcmp(cmd_str, "reztest_keep") == 0) {
        if (wiz_reztest_active) {
            int idx = wiz_config->reztest_index;
            const reztest_combo_t *c = (idx >= 0 && idx < REZTEST_COMBO_COUNT) ? &reztest_combos[idx] : NULL;
            if (c) {
                ESP_LOGI(TAG, "reztest: KEEP combo %d — %s", idx, c->label);
                wiz_config->matrix_width = c->width;
                wiz_config->matrix_height = c->height;
                wiz_config->scan_wiring = c->scan_wiring;
                wiz_config->reztest_mode = false;
                wiz_config->reztest_done = true;
                wiz_config->reztest_index = 0;
                wiz_config->setup_complete = false;
                pd_config_save(wiz_config);

                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "reztest_locked");
                cJSON_AddStringToObject(ack, "label", c->label);
                cJSON_AddNumberToObject(ack, "width", c->width);
                cJSON_AddNumberToObject(ack, "height", c->height);
                cJSON_AddNumberToObject(ack, "scan_wiring", c->scan_wiring);
                wiz_send_json(ack);

                pd_display_clear();
                pd_display_draw_text_tiny(2, 2, "SAVED!", PD_COLOR_GREEN);
                pd_display_draw_text_tiny(2, 8, c->label, PD_COLOR_WHITE);
                pd_display_draw_text_tiny(2, 16, "Rebooting...", PD_COLOR_DIM);
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        }
    } else if (strcmp(cmd_str, "reztest_skip") == 0) {
        if (wiz_reztest_active) {
            int idx = wiz_config->reztest_index + 1;
            if (idx >= REZTEST_COMBO_COUNT) {
                /* cycled all — exit reztest */
                ESP_LOGI(TAG, "reztest: cycled all combos, exiting");
                wiz_config->reztest_mode = false;
                wiz_config->reztest_index = 0;
                wiz_config->setup_complete = false;
                pd_config_save(wiz_config);

                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "reztest_done");
                wiz_send_json(ack);

                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                ESP_LOGI(TAG, "reztest: skip to combo %d", idx);
                wiz_config->reztest_index = idx;
                pd_config_save(wiz_config);

                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "reztest_next");
                cJSON_AddNumberToObject(ack, "index", idx);
                wiz_send_json(ack);

                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    } else if (strcmp(cmd_str, "nav") == 0) {
        cJSON *dir = cJSON_GetObjectItem(root, "dir");
        if (cJSON_IsString(dir)) {
            if (strcmp(dir->valuestring, "back") == 0) wiz_go_back();
            else if (strcmp(dir->valuestring, "next") == 0) {
                const pd_step_def_t *def = &pd_step_defs[wiz_step];
                if (def->mode == PD_MODE_MENU) wiz_apply_menu_select(wiz_menu_selected);
                else wiz_apply_text_input(wiz_get_text_value());
            }
        }
    } else if (strcmp(cmd_str, "select") == 0) {
        cJSON *index = cJSON_GetObjectItem(root, "index");
        if (cJSON_IsNumber(index)) {
            wiz_menu_selected = index->valueint;
            wiz_apply_menu_select(wiz_menu_selected);
        }
    } else if (strcmp(cmd_str, "input") == 0) {
        cJSON *value = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(value)) {
            strlcpy(wiz_text_value, value->valuestring, sizeof(wiz_text_value));
            wiz_apply_text_input(wiz_text_value);
        }
    } else if (strcmp(cmd_str, "key") == 0) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsString(code)) {
            wiz_handle_key(code->valuestring);
        }
    } else if (strcmp(cmd_str, "scan_wifi") == 0) {
        wiz_wifi_scanned = false;
        wiz_do_wifi_scan();
        wiz_send_state();
        wiz_render_display();
    } else if (strcmp(cmd_str, "goodbye") == 0) {
        ESP_LOGI(TAG, "CLI disconnected (goodbye)");
        if (wiz_config && wiz_config->setup_complete) {
            /* restore idle screen */
            const char *ip = pd_network_get_ip();
            pd_display_render_idle(
                wiz_config->device_name,
                wiz_config->matrix_width, wiz_config->matrix_height,
                wiz_config->orientation_deg, wiz_config->scan_wiring,
                ip ? ip : "DHCP");
            wiz_public_state = PD_WIZARD_STATE_COMPLETE;
            wiz_step = PD_STEP_COUNT;
        } else {
            pd_display_render_boot_message();
        }
    }

    cJSON_Delete(root);
}

/* ---------- serial line accumulator ---------- */

static void wiz_feed_byte(char ch)
{
    if (ch == '\r') return;

    if (ch == '\n') {
        wiz_line_buf[wiz_line_len] = '\0';
        if (wiz_line_len > 0 && wiz_line_buf[0] == '{') {
            wiz_process_command(wiz_line_buf);
        }
        wiz_line_len = 0;
        return;
    }

    if (wiz_line_len < sizeof(wiz_line_buf) - 1) {
        wiz_line_buf[wiz_line_len++] = ch;
    }
}

/* ---------- serial polling ---------- */

static void wiz_poll_uart(void)
{
    if (!wiz_uart_started) {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT
        };

        esp_err_t err = uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            if (!wiz_uart_warned) {
                wiz_uart_warned = true;
                ESP_LOGE(TAG, "uart init failed: %s", esp_err_to_name(err));
            }
            return;
        }

        ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        wiz_uart_started = true;
    }

    uint8_t buffer[64];
    int len = 0;
    do {
        len = uart_read_bytes(UART_NUM_0, buffer, sizeof(buffer), 0);
        for (int i = 0; i < len; i++) {
            wiz_feed_byte((char)buffer[i]);
        }
    } while (len > 0);
}

static void wiz_poll_usb_serial_jtag(void)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (!wiz_usb_serial_started) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            if (!wiz_usb_serial_warned) {
                wiz_usb_serial_warned = true;
                ESP_LOGE(TAG, "usb-serial-jtag init failed: %s", esp_err_to_name(err));
            }
            return;
        }
        wiz_usb_serial_started = true;
    }

    uint8_t buffer[64];
    int len = 0;
    do {
        len = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), 0);
        for (int i = 0; i < len; i++) {
            wiz_feed_byte((char)buffer[i]);
        }
    } while (len > 0);
#else
    if (!wiz_usb_serial_warned) {
        wiz_usb_serial_warned = true;
        ESP_LOGW(TAG, "usb-serial-jtag not supported on this target");
    }
#endif
}

static void wiz_poll_usb_keyboard(void)
{
#if CONFIG_TINYUSB_HOST
    if (!wiz_usb_started) {
        tinyusb_config_t tusb_cfg = {
            .device_descriptor = NULL,
            .string_descriptor = NULL,
            .external_phy = false
        };
        esp_err_t err = tinyusb_driver_install(&tusb_cfg);
        if (err == ESP_OK) {
            wiz_usb_started = true;
            xTaskCreate(wiz_usb_hid_task, "pd_usb_hid", 4096, NULL, 5, NULL);
        } else {
            ESP_LOGE(TAG, "tinyusb init failed: %s", esp_err_to_name(err));
        }
    }
#else
    if (!wiz_usb_warned) {
        wiz_usb_warned = true;
        ESP_LOGW(TAG, "usb hid keyboard not enabled");
    }
#endif
}

/* ---------- USB HID keyboard (generates key commands) ---------- */

#if CONFIG_TINYUSB_HOST
static char wiz_keycode_to_char(uint8_t keycode, bool shift)
{
    if (keycode >= 0x04 && keycode <= 0x1d) {
        return (char)((shift ? 'A' : 'a') + (keycode - 0x04));
    }
    if (keycode >= 0x1e && keycode <= 0x27) {
        static const char numbers[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[keycode - 0x1e] : numbers[keycode - 0x1e];
    }
    switch (keycode) {
        case HID_KEY_SPACE:         return ' ';
        case HID_KEY_MINUS:         return shift ? '_' : '-';
        case HID_KEY_EQUAL:         return shift ? '+' : '=';
        case HID_KEY_BRACKET_LEFT:  return shift ? '{' : '[';
        case HID_KEY_BRACKET_RIGHT: return shift ? '}' : ']';
        case HID_KEY_BACKSLASH:     return shift ? '|' : '\\';
        case HID_KEY_SEMICOLON:     return shift ? ':' : ';';
        case HID_KEY_APOSTROPHE:    return shift ? '"' : '\'';
        case HID_KEY_GRAVE:         return shift ? '~' : '`';
        case HID_KEY_COMMA:         return shift ? '<' : ',';
        case HID_KEY_PERIOD:        return shift ? '>' : '.';
        case HID_KEY_SLASH:         return shift ? '?' : '/';
        default: return '\0';
    }
}

static void wiz_handle_hid_report(const uint8_t *report, uint16_t len)
{
    if (!report || len < 8) return;
    bool shift = report[0] & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    for (int i = 2; i < 8; i++) {
        uint8_t kc = report[i];
        if (kc == 0) continue;
        if (kc == HID_KEY_ARROW_UP)    { wiz_handle_key("up"); continue; }
        if (kc == HID_KEY_ARROW_DOWN)  { wiz_handle_key("down"); continue; }
        if (kc == HID_KEY_ARROW_LEFT)  { wiz_handle_key("<"); continue; }
        if (kc == HID_KEY_ARROW_RIGHT) { wiz_handle_key(">"); continue; }
        if (kc == HID_KEY_ENTER)       { wiz_handle_key("enter"); continue; }
        if (kc == HID_KEY_BACKSPACE)   { wiz_handle_key("backspace"); continue; }
        char ch = wiz_keycode_to_char(kc, shift);
        if (ch) {
            char code[2] = {ch, '\0'};
            wiz_handle_key(code);
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len)
{
    (void)desc_report; (void)desc_len;
    if (tuh_hid_interface_protocol(dev_addr, instance) == HID_ITF_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "usb keyboard mounted");
        tuh_hid_receive_report(dev_addr, instance);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)dev_addr; (void)instance;
    ESP_LOGI(TAG, "usb keyboard unmounted");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len)
{
    wiz_handle_hid_report(report, len);
    tuh_hid_receive_report(dev_addr, instance);
}

static void wiz_usb_hid_task(void *arg)
{
    (void)arg;
    while (true) {
        tuh_task();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

/* ---------- public API ---------- */

esp_err_t pd_wizard_start(pd_config_t *config)
{
    wiz_config = config;
    wiz_line_len = 0;
    wiz_text_value[0] = '\0';
    wiz_wifi_scanned = false;
    wiz_reztest_active = (config && config->reztest_mode);

    if (config && config->setup_complete && !wiz_reztest_active) {
        wiz_public_state = PD_WIZARD_STATE_COMPLETE;
        wiz_step = PD_STEP_COUNT;
        ESP_LOGI(TAG, "wizard: setup already complete");
        return ESP_OK;
    }

    if (wiz_reztest_active) {
        int idx = config->reztest_index;
        if (idx >= 0 && idx < REZTEST_COMBO_COUNT) {
            const reztest_combo_t *c = &reztest_combos[idx];
            ESP_LOGI(TAG, "wizard: reztest mode combo %d/%d %s", idx + 1, REZTEST_COMBO_COUNT, c->label);
            /* draw info on display */
            pd_display_clear();
            uint16_t dw = (uint16_t)c->width;
            uint16_t dh = (uint16_t)c->height;
            pd_display_draw_rect(0, 0, dw, dh, PD_COLOR_CYAN);
            char line[32];
            snprintf(line, sizeof(line), "REZTEST %d/%d", idx + 1, REZTEST_COMBO_COUNT);
            pd_display_draw_text_tiny(2, 2, line, PD_COLOR_YELLOW);
            pd_display_draw_text_tiny(2, 8, c->label, PD_COLOR_WHITE);
            pd_display_draw_text_tiny(2, 16, "[enter] keep", PD_COLOR_GREEN);
            pd_display_draw_text_tiny(2, 22, "[any] skip", PD_COLOR_DIM);
        }
        wiz_public_state = PD_WIZARD_STATE_MATRIX_SIZE;
        ESP_LOGI(TAG, "wizard: reztest waiting for CLI");
        return ESP_OK;
    }

    wiz_public_state = PD_WIZARD_STATE_MATRIX_SIZE;
    wiz_step = PD_STEP_MATRIX_SIZE;
    wiz_menu_selected = 0;
    ESP_LOGI(TAG, "wizard: starting setup (waiting for CLI hello)");
    pd_display_wizard_status("SETUP");
    return ESP_OK;
}

void pd_wizard_tick(void)
{
    if (!wiz_config) {
        return;
    }

    wiz_poll_uart();
    wiz_poll_usb_serial_jtag();
    if (wiz_public_state != PD_WIZARD_STATE_COMPLETE) {
        wiz_poll_usb_keyboard();
    }
}

bool pd_wizard_is_complete(void)
{
    return wiz_public_state == PD_WIZARD_STATE_COMPLETE;
}

bool pd_wizard_reztest_get_display(const pd_config_t *config, int *width, int *height, int *scan_wiring)
{
    if (!config || !config->reztest_mode) return false;
    int idx = config->reztest_index;
    if (idx < 0 || idx >= REZTEST_COMBO_COUNT) return false;
    const reztest_combo_t *c = &reztest_combos[idx];
    *width = c->width;
    *height = c->height;
    *scan_wiring = c->scan_wiring;
    return true;
}
