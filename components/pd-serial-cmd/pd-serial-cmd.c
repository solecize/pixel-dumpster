/*
 * pd-serial-cmd — serial command listener for pixel-dumpster
 *
 * Receives JSON commands over USB serial (forwarded by pd-wizard after
 * setup is complete) and dispatches them to pd-content.
 *
 * Protocol: newline-terminated JSON, one command per line.
 *   -> {"cmd":"play","path":"marquees/arcade/pacman","transition":"fade","duration_ms":800}
 *   <- {"type":"ack","cmd":"play","ok":true}
 *
 *   -> {"cmd":"stop"}
 *   <- {"type":"ack","cmd":"stop","ok":true}
 *
 *   -> {"cmd":"status"}
 *   <- {"type":"status","playing":true,"path":"marquees/arcade/pacman",...}
 *
 *   -> {"cmd":"list"}
 *   <- {"type":"list","items":[...]}
 */

#include "pd-serial-cmd.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "pd-content.h"
#include "pd-wizard.h"

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "pd-serial-cmd";

/* ---------- serial response helpers ---------- */

static void serial_write(const char *data, size_t len)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#else
    (void)data; (void)len;
#endif
}

static void serial_send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        serial_write(str, strlen(str));
        serial_write("\n", 1);
        free(str);
    }
    cJSON_Delete(root);
}

/* ---------- command handlers ---------- */

static void handle_play(cJSON *root)
{
    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring[0]) {
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "type", "ack");
        cJSON_AddStringToObject(ack, "cmd", "play");
        cJSON_AddFalseToObject(ack, "ok");
        cJSON_AddStringToObject(ack, "error", "missing path");
        serial_send_json(ack);
        return;
    }

    const char *path = path_item->valuestring;
    cJSON *trans_item = cJSON_GetObjectItem(root, "transition");
    cJSON *dur_item = cJSON_GetObjectItem(root, "duration_ms");
    const char *transition = cJSON_IsString(trans_item) ? trans_item->valuestring : NULL;
    int duration_ms = cJSON_IsNumber(dur_item) ? dur_item->valueint : 0;

    esp_err_t err;
    if (transition && transition[0] && duration_ms > 0) {
        err = pd_content_play_with_transition(path, transition, duration_ms);
    } else {
        err = pd_content_play(path);
    }

    ESP_LOGI(TAG, "play path=%s transition=%s dur=%d -> %s",
             path, transition ? transition : "none", duration_ms,
             err == ESP_OK ? "ok" : esp_err_to_name(err));

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "play");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(ack, "error", esp_err_to_name(err));
    }
    serial_send_json(ack);
}

static void handle_stop(void)
{
    esp_err_t err = pd_content_stop();
    ESP_LOGI(TAG, "stop -> %s", err == ESP_OK ? "ok" : esp_err_to_name(err));

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "stop");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    serial_send_json(ack);
}

static void handle_status(void)
{
    pd_content_status_t st = pd_content_get_status();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "status");
    cJSON_AddBoolToObject(resp, "playing", st.playing);
    cJSON_AddStringToObject(resp, "path", st.current_path);
    cJSON_AddBoolToObject(resp, "is_sequence", st.is_sequence);
    cJSON_AddNumberToObject(resp, "current_frame", st.current_frame);
    cJSON_AddNumberToObject(resp, "total_frames", st.total_frames);
    cJSON_AddNumberToObject(resp, "fps", st.fps);
    serial_send_json(resp);
}

static void handle_list(void)
{
    pd_content_entry_t entries[PD_CONTENT_MAX_LIST];
    int count = pd_content_list_images(entries, PD_CONTENT_MAX_LIST);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "list");
    cJSON *arr = cJSON_AddArrayToObject(resp, "items");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path", entries[i].path);
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddBoolToObject(item, "is_sequence", entries[i].is_sequence);
        if (entries[i].is_sequence) {
            cJSON_AddNumberToObject(item, "frame_count", entries[i].frame_count);
            cJSON_AddNumberToObject(item, "fps", entries[i].fps);
        }
        cJSON_AddItemToArray(arr, item);
    }
    serial_send_json(resp);
}

/* ---------- command dispatch (called by wizard callback) ---------- */

static void serial_cmd_handler(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd->valuestring;

    if (strcmp(cmd_str, "play") == 0) {
        handle_play(root);
    } else if (strcmp(cmd_str, "stop") == 0) {
        handle_stop();
    } else if (strcmp(cmd_str, "status") == 0) {
        handle_status();
    } else if (strcmp(cmd_str, "list") == 0) {
        handle_list();
    } else {
        ESP_LOGW(TAG, "unknown command: %s", cmd_str);
    }

    cJSON_Delete(root);
}

/* ---------- public API ---------- */

esp_err_t pd_serial_cmd_init(void)
{
    pd_wizard_set_cmd_callback(serial_cmd_handler);
    ESP_LOGI(TAG, "serial command listener registered");
    return ESP_OK;
}
