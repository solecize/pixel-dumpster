#include "pd-storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "pd-storage";
static const char *PD_NOW_FILENAME = "now.json";
static const char *PD_DEFAULT_FILENAME = "default.png";
static char pd_base_path[32] = "/pd";

static esp_err_t pd_storage_ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "path exists but is not a dir: %s", path);
        return ESP_FAIL;
    }

    if (mkdir(path, 0775) != 0) {
        ESP_LOGE(TAG, "mkdir failed for %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void pd_storage_state_defaults(pd_display_state_t *state)
{
    if (!state) {
        return;
    }

    state->mode = PD_DISPLAY_MODE_IDLE;
    state->system[0] = '\0';
    state->game[0] = '\0';
    state->asset[0] = '\0';
    state->updated_at = 0;
}

static const char *pd_storage_mode_to_string(pd_display_mode_t mode)
{
    switch (mode) {
        case PD_DISPLAY_MODE_SYSTEM:
            return "system";
        case PD_DISPLAY_MODE_GAME:
            return "game";
        case PD_DISPLAY_MODE_CUSTOM:
            return "custom";
        case PD_DISPLAY_MODE_IDLE:
        default:
            return "idle";
    }
}

static pd_display_mode_t pd_storage_mode_from_string(const char *mode)
{
    if (!mode) {
        return PD_DISPLAY_MODE_IDLE;
    }

    if (strcmp(mode, "system") == 0) {
        return PD_DISPLAY_MODE_SYSTEM;
    }
    if (strcmp(mode, "game") == 0) {
        return PD_DISPLAY_MODE_GAME;
    }
    if (strcmp(mode, "custom") == 0) {
        return PD_DISPLAY_MODE_CUSTOM;
    }

    return PD_DISPLAY_MODE_IDLE;
}

const char *pd_storage_get_base_path(void)
{
    return pd_base_path;
}

esp_err_t pd_storage_init(const pd_storage_config_t *config)
{
    if (config && config->base_path && strlen(config->base_path) < sizeof(pd_base_path)) {
        strlcpy(pd_base_path, config->base_path, sizeof(pd_base_path));
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = pd_base_path,
        .partition_label = "pd",
        .format_if_mount_failed = true,
        .dont_mount = false
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount littlefs: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "storage mounted at %s", pd_base_path);

    char path[128];
    snprintf(path, sizeof(path), "%s/system", pd_base_path);
    ESP_ERROR_CHECK(pd_storage_ensure_dir(pd_base_path));
    ESP_ERROR_CHECK(pd_storage_ensure_dir(path));
    snprintf(path, sizeof(path), "%s/game", pd_base_path);
    ESP_ERROR_CHECK(pd_storage_ensure_dir(path));
    snprintf(path, sizeof(path), "%s/assets", pd_base_path);
    ESP_ERROR_CHECK(pd_storage_ensure_dir(path));

    snprintf(path, sizeof(path), "%s/%s", pd_base_path, PD_NOW_FILENAME);
    struct stat st;
    if (stat(path, &st) != 0) {
        pd_display_state_t state;
        pd_storage_state_defaults(&state);
        pd_storage_save_state(&state);
    }

    snprintf(path, sizeof(path), "%s/%s", pd_base_path, PD_DEFAULT_FILENAME);
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "default.png missing at %s", path);
    }

    return ESP_OK;
}

esp_err_t pd_storage_load_state(pd_display_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    pd_storage_state_defaults(state);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", pd_base_path, PD_NOW_FILENAME);
    FILE *file = fopen(path, "r");
    if (!file) {
        ESP_LOGW(TAG, "now.json missing, using defaults");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    if (size <= 0) {
        fclose(file);
        ESP_LOGW(TAG, "now.json empty, using defaults");
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
        ESP_LOGW(TAG, "now.json parse failed, using defaults");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    cJSON *system = cJSON_GetObjectItem(root, "system");
    cJSON *game = cJSON_GetObjectItem(root, "game");
    cJSON *asset = cJSON_GetObjectItem(root, "asset");
    cJSON *updated_at = cJSON_GetObjectItem(root, "updated_at");

    state->mode = pd_storage_mode_from_string(cJSON_IsString(mode) ? mode->valuestring : NULL);
    if (cJSON_IsString(system)) {
        strlcpy(state->system, system->valuestring, sizeof(state->system));
    }
    if (cJSON_IsString(game)) {
        strlcpy(state->game, game->valuestring, sizeof(state->game));
    }
    if (cJSON_IsString(asset)) {
        strlcpy(state->asset, asset->valuestring, sizeof(state->asset));
    }
    if (cJSON_IsNumber(updated_at)) {
        state->updated_at = (int64_t)updated_at->valuedouble;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t pd_storage_save_state(const pd_display_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "mode", pd_storage_mode_to_string(state->mode));
    cJSON_AddStringToObject(root, "system", state->system);
    cJSON_AddStringToObject(root, "game", state->game);
    cJSON_AddStringToObject(root, "asset", state->asset);
    cJSON_AddNumberToObject(root, "updated_at", (double)state->updated_at);

    char *payload = cJSON_Print(root);
    cJSON_Delete(root);

    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", pd_base_path, PD_NOW_FILENAME);
    FILE *file = fopen(path, "w");
    if (!file) {
        ESP_LOGE(TAG, "failed to open %s: %s", path, strerror(errno));
        free(payload);
        return ESP_FAIL;
    }

    fwrite(payload, 1, strlen(payload), file);
    fclose(file);
    free(payload);

    ESP_LOGI(TAG, "state saved to %s", path);
    return ESP_OK;
}
