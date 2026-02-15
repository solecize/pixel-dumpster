#include "pd-config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "pd-storage.h"

static const char *TAG = "pd-config";
static const char *PD_CONFIG_FILENAME = "config.json";

static void pd_config_build_path(char *path, size_t length)
{
    const char *base_path = pd_storage_get_base_path();
    snprintf(path, length, "%s/%s", base_path, PD_CONFIG_FILENAME);
}

static void pd_config_set_defaults(pd_config_t *config)
{
    if (!config) {
        return;
    }

    config->matrix_width = 64;
    config->matrix_height = 64;
    config->orientation_deg = 0;
    config->scan_wiring = 0;
    config->wifi_ssid[0] = '\0';
    config->wifi_password[0] = '\0';
    strncpy(config->device_name, "pixel-dumpster", sizeof(config->device_name) - 1);
    config->device_name[sizeof(config->device_name) - 1] = '\0';
    strncpy(config->hostname, "pixel-dumpster", sizeof(config->hostname) - 1);
    config->hostname[sizeof(config->hostname) - 1] = '\0';
    strncpy(config->timezone, "UTC0", sizeof(config->timezone) - 1);
    config->timezone[sizeof(config->timezone) - 1] = '\0';
    config->static_ip[0] = '\0';
    config->static_gateway[0] = '\0';
    config->static_netmask[0] = '\0';
    config->setup_complete = false;
    config->reztest_mode = false;
    config->reztest_done = false;
    config->reztest_index = 0;
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
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    pd_config_set_defaults(config);

    char path[128];
    pd_config_build_path(path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) {
        ESP_LOGW(TAG, "config not found, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    if (size <= 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = calloc(1, size + 1);
    if (!buffer) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    fread(buffer, 1, size, file);
    fclose(file);

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);

    if (!root) {
        ESP_LOGW(TAG, "config parse failed, using defaults");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *matrix_width = cJSON_GetObjectItem(root, "matrix_width");
    cJSON *matrix_height = cJSON_GetObjectItem(root, "matrix_height");
    cJSON *orientation = cJSON_GetObjectItem(root, "orientation_deg");
    cJSON *scan_wiring = cJSON_GetObjectItem(root, "scan_wiring");
    cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *wifi_password = cJSON_GetObjectItem(root, "wifi_password");
    cJSON *device_name = cJSON_GetObjectItem(root, "device_name");
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    cJSON *static_ip = cJSON_GetObjectItem(root, "static_ip");
    cJSON *static_gateway = cJSON_GetObjectItem(root, "static_gateway");
    cJSON *static_netmask = cJSON_GetObjectItem(root, "static_netmask");
    cJSON *setup_complete = cJSON_GetObjectItem(root, "setup_complete");
    cJSON *reztest_mode = cJSON_GetObjectItem(root, "reztest_mode");
    cJSON *reztest_done = cJSON_GetObjectItem(root, "reztest_done");
    cJSON *reztest_index = cJSON_GetObjectItem(root, "reztest_index");
    cJSON *config_version = cJSON_GetObjectItem(root, "config_version");

    if (cJSON_IsNumber(matrix_width)) {
        config->matrix_width = matrix_width->valueint;
    }
    if (cJSON_IsNumber(matrix_height)) {
        config->matrix_height = matrix_height->valueint;
    }
    if (cJSON_IsNumber(orientation)) {
        config->orientation_deg = orientation->valueint;
    }
    if (cJSON_IsNumber(scan_wiring)) {
        config->scan_wiring = scan_wiring->valueint;
    }
    if (cJSON_IsString(wifi_ssid)) {
        strlcpy(config->wifi_ssid, wifi_ssid->valuestring, sizeof(config->wifi_ssid));
    }
    if (cJSON_IsString(wifi_password)) {
        strlcpy(config->wifi_password, wifi_password->valuestring, sizeof(config->wifi_password));
    }
    if (cJSON_IsString(device_name)) {
        strlcpy(config->device_name, device_name->valuestring, sizeof(config->device_name));
    }
    if (cJSON_IsString(hostname)) {
        strlcpy(config->hostname, hostname->valuestring, sizeof(config->hostname));
    }
    if (cJSON_IsString(timezone)) {
        strlcpy(config->timezone, timezone->valuestring, sizeof(config->timezone));
    }
    if (cJSON_IsString(static_ip)) {
        strlcpy(config->static_ip, static_ip->valuestring, sizeof(config->static_ip));
    }
    if (cJSON_IsString(static_gateway)) {
        strlcpy(config->static_gateway, static_gateway->valuestring, sizeof(config->static_gateway));
    }
    if (cJSON_IsString(static_netmask)) {
        strlcpy(config->static_netmask, static_netmask->valuestring, sizeof(config->static_netmask));
    }
    if (cJSON_IsBool(setup_complete)) {
        config->setup_complete = cJSON_IsTrue(setup_complete);
    }
    if (cJSON_IsBool(reztest_mode)) {
        config->reztest_mode = cJSON_IsTrue(reztest_mode);
    }
    if (cJSON_IsBool(reztest_done)) {
        config->reztest_done = cJSON_IsTrue(reztest_done);
    }
    if (cJSON_IsNumber(reztest_index)) {
        config->reztest_index = reztest_index->valueint;
    }
    if (cJSON_IsNumber(config_version)) {
        config->config_version = config_version->valueint;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t pd_config_save(const pd_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "matrix_width", config->matrix_width);
    cJSON_AddNumberToObject(root, "matrix_height", config->matrix_height);
    cJSON_AddNumberToObject(root, "orientation_deg", config->orientation_deg);
    cJSON_AddNumberToObject(root, "scan_wiring", config->scan_wiring);
    cJSON_AddStringToObject(root, "wifi_ssid", config->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", config->wifi_password);
    cJSON_AddStringToObject(root, "device_name", config->device_name);
    cJSON_AddStringToObject(root, "hostname", config->hostname);
    cJSON_AddStringToObject(root, "timezone", config->timezone);
    cJSON_AddStringToObject(root, "static_ip", config->static_ip);
    cJSON_AddStringToObject(root, "static_gateway", config->static_gateway);
    cJSON_AddStringToObject(root, "static_netmask", config->static_netmask);
    cJSON_AddBoolToObject(root, "setup_complete", config->setup_complete);
    cJSON_AddBoolToObject(root, "reztest_mode", config->reztest_mode);
    cJSON_AddBoolToObject(root, "reztest_done", config->reztest_done);
    cJSON_AddNumberToObject(root, "reztest_index", config->reztest_index);
    cJSON_AddNumberToObject(root, "config_version", config->config_version);

    char *payload = cJSON_Print(root);
    cJSON_Delete(root);

    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char path[128];
    pd_config_build_path(path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (!file) {
        ESP_LOGE(TAG, "config save failed %s: %s", path, strerror(errno));
        free(payload);
        return ESP_FAIL;
    }

    fwrite(payload, 1, strlen(payload), file);
    fclose(file);
    free(payload);

    ESP_LOGI(TAG, "config saved to %s", path);
    return ESP_OK;
}

