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
 *
 *   -> {"cmd":"set_playback","auto_quantize_palette":true,"save":true}
 *   <- {"type":"ack","cmd":"set_playback","ok":true,"auto_quantize_palette":true}
 *
 * Streaming content upload (USB / BLE NDJSON; WiFi still uses HTTP /api/upload):
 *   -> {"cmd":"upload_begin","path":"marquees/arcade/pacman.png","size":12345}
 *   <- {"type":"ack","cmd":"upload_begin","ok":true,"path":"...","size":12345}
 *   -> {"cmd":"upload_chunk","data":"<base64>"}
 *   <- {"type":"ack","cmd":"upload_chunk","ok":true,"received":N}
 *   -> {"cmd":"upload_end"}
 *   <- {"type":"ack","cmd":"upload_end","ok":true,"path":"...","size":N}
 *   -> {"cmd":"upload_abort"}
 *   <- {"type":"ack","cmd":"upload_abort","ok":true}
 */

#include "pd-serial-cmd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "pd-content.h"
#include "pd-wizard.h"

static const char *TAG = "pd-serial-cmd";

/* FS cmds must not run on the BLE/NimBLE host task. Play is handed to
 * pd_content_play_async (shared pd_play worker). list() needs ~main-task
 * depth (LittleFS + cJSON); 6 KiB overflowed and rebooted on BLE connect. */
#define PD_SERIAL_HEAVY_Q_LEN 4
#define PD_SERIAL_HEAVY_STACK 12288

static void serial_heavy_task(void *arg);

static StaticQueue_t s_heavy_q_mem;
static uint8_t s_heavy_q_storage[PD_SERIAL_HEAVY_Q_LEN * sizeof(char *)];
static QueueHandle_t s_heavy_q = NULL;
static StaticTask_t s_heavy_task_mem;
static StackType_t s_heavy_task_stack[PD_SERIAL_HEAVY_STACK / sizeof(StackType_t)];
static TaskHandle_t s_heavy_task = NULL;

static bool cmd_is_heavy(const char *cmd_str)
{
    return strcmp(cmd_str, "list") == 0
        || strcmp(cmd_str, "stop") == 0
        || strcmp(cmd_str, "delete") == 0
        || strcmp(cmd_str, "rename") == 0
        || strcmp(cmd_str, "set_meta") == 0
        || strcmp(cmd_str, "upload_begin") == 0
        || strcmp(cmd_str, "upload_chunk") == 0
        || strcmp(cmd_str, "upload_end") == 0
        || strcmp(cmd_str, "upload_abort") == 0;
}

/* ---------- serial response helpers ---------- */

static void serial_send_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        /* Routes through pd-wizard so USB/UART and BLE TX hooks all see it. */
        pd_wizard_write_raw(str, strlen(str));
        pd_wizard_write_raw("\n", 1);
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

    /* Async onto pd_play worker — same path as HTTP /api/play. Keeps NimBLE
     * (and USB wizard) stacks clear of PNG decode. */
    esp_err_t err = pd_content_play_async(path, transition, duration_ms);

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
    if (st.is_sequence) {
        cJSON_AddNumberToObject(resp, "fps", st.fps);
        if (st.achieved_fps > 0.05f) {
            cJSON_AddNumberToObject(resp, "achieved_fps", st.achieved_fps);
        }
    }
    serial_send_json(resp);
}

static void handle_list(void)
{
    ESP_LOGI(TAG, "handle_list: begin");
    pd_content_entry_t entries[PD_CONTENT_MAX_LIST];
    int count = pd_content_list_images(entries, PD_CONTENT_MAX_LIST);
    ESP_LOGI(TAG, "handle_list: got %d entries", count);

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
    ESP_LOGI(TAG, "handle_list: sending response");
    serial_send_json(resp);
    ESP_LOGI(TAG, "handle_list: response sent");
}

static void send_ack(const char *cmd, bool ok, const char *error)
{
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", cmd);
    cJSON_AddBoolToObject(ack, "ok", ok);
    if (!ok && error && error[0]) {
        cJSON_AddStringToObject(ack, "error", error);
    }
    serial_send_json(ack);
}

static void handle_upload_begin(cJSON *root)
{
    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    cJSON *size_item = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsString(path_item) || !path_item->valuestring[0] || !cJSON_IsNumber(size_item)) {
        send_ack("upload_begin", false, "missing path or size");
        return;
    }
    if (size_item->valuedouble < 0 || size_item->valuedouble > (double)PD_CONTENT_UPLOAD_MAX_BYTES) {
        send_ack("upload_begin", false, "invalid size");
        return;
    }

    size_t total = (size_t)size_item->valuedouble;
    esp_err_t err = pd_content_upload_begin(path_item->valuestring, total);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "upload_begin");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    cJSON_AddStringToObject(ack, "path", path_item->valuestring);
    cJSON_AddNumberToObject(ack, "size", (double)total);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(ack, "error", esp_err_to_name(err));
    }
    serial_send_json(ack);
}

static void handle_upload_chunk(cJSON *root)
{
    if (!pd_content_upload_active()) {
        send_ack("upload_chunk", false, "no active upload");
        return;
    }

    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data_item) || !data_item->valuestring[0]) {
        send_ack("upload_chunk", false, "missing data");
        return;
    }

    const char *b64 = data_item->valuestring;
    size_t b64_len = strlen(b64);
    /* Chunks are capped small for BLE ATT / line buffer; reject oversized. */
    if (b64_len == 0 || b64_len > 800) {
        send_ack("upload_chunk", false, "chunk too large");
        return;
    }
    size_t cap = (b64_len / 4) * 3 + 4;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        send_ack("upload_chunk", false, "oom");
        return;
    }
    size_t decoded = 0;
    int rc = mbedtls_base64_decode(buf, cap, &decoded, (const unsigned char *)b64, b64_len);
    if (rc != 0 || decoded == 0) {
        free(buf);
        send_ack("upload_chunk", false, "decode failed");
        return;
    }

    esp_err_t err = pd_content_upload_write(buf, decoded);
    free(buf);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "upload_chunk");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(ack, "received", (double)pd_content_upload_received());
    if (err != ESP_OK) {
        cJSON_AddStringToObject(ack, "error", esp_err_to_name(err));
    }
    serial_send_json(ack);
}

static void handle_upload_end(void)
{
    size_t received = pd_content_upload_received();
    esp_err_t err = pd_content_upload_finish();

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "upload_end");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(ack, "size", (double)received);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(ack, "error", esp_err_to_name(err));
    }
    serial_send_json(ack);
}

static void handle_upload_abort(void)
{
    pd_content_upload_abort();
    send_ack("upload_abort", true, NULL);
}

static void handle_delete(cJSON *root)
{
    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path) || !path->valuestring[0]) {
        send_ack("delete", false, "missing path");
        return;
    }
    esp_err_t err = pd_content_delete_file(path->valuestring);
    send_ack("delete", err == ESP_OK, err == ESP_OK ? NULL : esp_err_to_name(err));
}

static void handle_rename(cJSON *root)
{
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (!cJSON_IsString(from) || !cJSON_IsString(to)) {
        send_ack("rename", false, "missing from/to");
        return;
    }
    esp_err_t err = pd_content_rename(from->valuestring, to->valuestring);
    send_ack("rename", err == ESP_OK, err == ESP_OK ? NULL : esp_err_to_name(err));
}

static void handle_set_meta(cJSON *root)
{
    cJSON *path = cJSON_GetObjectItem(root, "path");
    cJSON *fps = cJSON_GetObjectItem(root, "fps");
    if (!cJSON_IsString(path) || !cJSON_IsNumber(fps)) {
        send_ack("set_meta", false, "missing path/fps");
        return;
    }
    esp_err_t err = pd_content_set_sequence_fps(path->valuestring, fps->valueint);
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "type", "ack");
    cJSON_AddStringToObject(ack, "cmd", "set_meta");
    cJSON_AddBoolToObject(ack, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(ack, "fps", fps->valueint);
    } else {
        cJSON_AddStringToObject(ack, "error", esp_err_to_name(err));
    }
    serial_send_json(ack);
}

/* ---------- command dispatch ---------- */

static void serial_cmd_dispatch(cJSON *root, const char *cmd_str)
{
    if (strcmp(cmd_str, "play") == 0) {
        handle_play(root);
    } else if (strcmp(cmd_str, "stop") == 0) {
        handle_stop();
    } else if (strcmp(cmd_str, "status") == 0) {
        handle_status();
    } else if (strcmp(cmd_str, "list") == 0) {
        handle_list();
    } else if (strcmp(cmd_str, "delete") == 0) {
        handle_delete(root);
    } else if (strcmp(cmd_str, "rename") == 0) {
        handle_rename(root);
    } else if (strcmp(cmd_str, "set_meta") == 0) {
        handle_set_meta(root);
    } else if (strcmp(cmd_str, "set_playback") == 0) {
        const pd_content_config_t *cur = pd_content_get_config();
        pd_content_config_t cfg = *cur;
        cJSON *aq = cJSON_GetObjectItem(root, "auto_quantize_palette");
        if (cJSON_IsBool(aq)) {
            cfg.auto_quantize_palette = cJSON_IsTrue(aq);
        }
        cJSON *sfc = cJSON_GetObjectItem(root, "show_fps_counter");
        if (cJSON_IsBool(sfc)) {
            cfg.show_fps_counter = cJSON_IsTrue(sfc);
        }
        pd_content_set_config(&cfg);
        cJSON *save = cJSON_GetObjectItem(root, "save");
        if (cJSON_IsTrue(save)) {
            pd_content_save_config();
        }
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "type", "ack");
        cJSON_AddStringToObject(ack, "cmd", "set_playback");
        cJSON_AddBoolToObject(ack, "ok", true);
        cJSON_AddBoolToObject(ack, "auto_quantize_palette", cfg.auto_quantize_palette);
        cJSON_AddBoolToObject(ack, "show_fps_counter", cfg.show_fps_counter);
        serial_send_json(ack);
    } else if (strcmp(cmd_str, "upload_begin") == 0) {
        handle_upload_begin(root);
    } else if (strcmp(cmd_str, "upload_chunk") == 0) {
        handle_upload_chunk(root);
    } else if (strcmp(cmd_str, "upload_end") == 0) {
        handle_upload_end();
    } else if (strcmp(cmd_str, "upload_abort") == 0) {
        handle_upload_abort();
    } else {
        ESP_LOGW(TAG, "unknown command: %s", cmd_str);
    }
}

static void serial_cmd_run_json(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        serial_cmd_dispatch(root, cmd->valuestring);
    }
    cJSON_Delete(root);
}

static void serial_heavy_task(void *arg)
{
    (void)arg;
    char *json = NULL;
    for (;;) {
        if (xQueueReceive(s_heavy_q, &json, portMAX_DELAY) == pdTRUE && json) {
            serial_cmd_run_json(json);
            free(json);
            json = NULL;
        }
    }
}

/* Called by wizard on the transport task (USB or NimBLE host). */
static void serial_cmd_handler(const char *json_str)
{
    if (!json_str) return;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd->valuestring;
    if (cmd_is_heavy(cmd_str) && s_heavy_q) {
        char *copy = strdup(json_str);
        cJSON_Delete(root);
        if (!copy) {
            send_ack(cmd_str, false, "oom");
            return;
        }
        if (xQueueSend(s_heavy_q, &copy, 0) != pdTRUE) {
            free(copy);
            send_ack(cmd_str, false, "busy");
        }
        return;
    }

    serial_cmd_dispatch(root, cmd_str);
    cJSON_Delete(root);
}

/* ---------- public API ---------- */

esp_err_t pd_serial_cmd_init(void)
{
    s_heavy_q = xQueueCreateStatic(PD_SERIAL_HEAVY_Q_LEN, sizeof(char *),
                                   s_heavy_q_storage, &s_heavy_q_mem);
    /* ESP-IDF FreeRTOS: stack depth is in bytes. */
    s_heavy_task = xTaskCreateStatic(serial_heavy_task, "pd_serial_heavy",
                                     PD_SERIAL_HEAVY_STACK,
                                     NULL, 5, s_heavy_task_stack, &s_heavy_task_mem);
    if (!s_heavy_q || !s_heavy_task) {
        ESP_LOGW(TAG, "static heavy worker unavailable — FS cmds run inline");
        s_heavy_q = NULL;
    }

    pd_wizard_set_cmd_callback(serial_cmd_handler);
    ESP_LOGI(TAG, "serial command listener registered (deferred_fs=%s)",
             s_heavy_q ? "yes" : "no");
    return ESP_OK;
}
