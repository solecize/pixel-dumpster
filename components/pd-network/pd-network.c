#include "pd-network.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "pd-config.h"
#include "pd-storage.h"

static const char *TAG = "pd-network";

static pd_network_config_t pd_network_config = {
    .http_port = 8088,
    .udp_port = 9876,
    .wifi_ssid = "",
    .wifi_password = ""
};

static httpd_handle_t pd_http_server = NULL;
static bool pd_wifi_connected = false;
static bool pd_network_suspended = false;
static int pd_udp_socket = -1;
static time_t pd_now_mtime = 0;

static void pd_network_handle_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        pd_wifi_connected = false;
        if (pd_network_suspended) {
            ESP_LOGW(TAG, "wifi disconnected (suspended, not retrying)");
        } else {
            ESP_LOGW(TAG, "wifi disconnected, retrying");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        pd_wifi_connected = true;
        ESP_LOGI(TAG, "wifi connected");
    }
}

static esp_err_t pd_http_send_json(httpd_req_t *req, cJSON *payload)
{
    char *json = cJSON_PrintUnformatted(payload);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t pd_http_reload_handler(httpd_req_t *req)
{
    pd_display_state_t state;
    pd_storage_load_state(&state);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pd_http_state_handler(httpd_req_t *req)
{
    pd_display_state_t state;
    if (pd_storage_load_state(&state) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load state");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    const char *mode = "idle";
    switch (state.mode) {
        case PD_DISPLAY_MODE_SYSTEM:
            mode = "system";
            break;
        case PD_DISPLAY_MODE_GAME:
            mode = "game";
            break;
        case PD_DISPLAY_MODE_CUSTOM:
            mode = "custom";
            break;
        case PD_DISPLAY_MODE_IDLE:
        default:
            mode = "idle";
            break;
    }

    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddStringToObject(root, "system", state.system);
    cJSON_AddStringToObject(root, "game", state.game);
    cJSON_AddStringToObject(root, "asset", state.asset);
    cJSON_AddNumberToObject(root, "updated_at", (double)state.updated_at);

    esp_err_t err = pd_http_send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t pd_http_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    cJSON *files = cJSON_AddArrayToObject(root, "files");
    const char *base_path = pd_storage_get_base_path();
    DIR *dir = opendir(base_path);
    if (!dir) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open storage");
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }

        cJSON_AddStringToObject(item, "path", entry->d_name);
        cJSON_AddItemToArray(files, item);
    }
    closedir(dir);

    esp_err_t err = pd_http_send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t pd_http_upload_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char path_param[128] = {0};
    char file_path[256];
    const char *base_path = pd_storage_get_base_path();

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", path_param, sizeof(path_param));
    }

    if (strlen(path_param) == 0) {
        snprintf(file_path, sizeof(file_path), "%s/assets/upload.bin", base_path);
    } else {
        if (path_param[0] == '/') {
            snprintf(file_path, sizeof(file_path), "%s%s", base_path, path_param);
        } else {
            snprintf(file_path, sizeof(file_path), "%s/%s", base_path, path_param);
        }
    }

    FILE *file = fopen(file_path, "w");
    if (!file) {
        ESP_LOGE(TAG, "upload open failed %s: %s", file_path, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    char buffer[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, sizeof(buffer));
        if (received <= 0) {
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }
        fwrite(buffer, 1, received, file);
        remaining -= received;
    }

    fclose(file);
    httpd_resp_sendstr(req, "File uploaded");
    return ESP_OK;
}

static esp_err_t pd_http_wizard_status_handler(httpd_req_t *req)
{
    pd_config_t config;
    pd_config_init(&config);
    pd_config_load(&config);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    cJSON_AddBoolToObject(root, "setup_complete", config.setup_complete);
    cJSON_AddStringToObject(root, "device_name", config.device_name);
    cJSON_AddStringToObject(root, "hostname", config.hostname);
    cJSON_AddStringToObject(root, "timezone", config.timezone);
    cJSON_AddStringToObject(root, "wifi_ssid", config.wifi_ssid);
    cJSON_AddStringToObject(root, "static_ip", config.static_ip);
    cJSON_AddStringToObject(root, "static_gateway", config.static_gateway);
    cJSON_AddStringToObject(root, "static_netmask", config.static_netmask);
    cJSON_AddNumberToObject(root, "matrix_width", config.matrix_width);
    cJSON_AddNumberToObject(root, "matrix_height", config.matrix_height);
    cJSON_AddNumberToObject(root, "orientation_deg", config.orientation_deg);

    esp_err_t err = pd_http_send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t pd_http_wizard_submit_handler(httpd_req_t *req)
{
    char *buffer = calloc(1, req->content_len + 1);
    if (!buffer) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer + offset, remaining);
        if (received <= 0) {
            free(buffer);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid payload");
        }
        remaining -= received;
        offset += received;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    pd_config_t config;
    pd_config_init(&config);
    pd_config_load(&config);

    cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *wifi_password = cJSON_GetObjectItem(root, "wifi_password");
    cJSON *device_name = cJSON_GetObjectItem(root, "device_name");
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    cJSON *static_ip = cJSON_GetObjectItem(root, "static_ip");
    cJSON *static_gateway = cJSON_GetObjectItem(root, "static_gateway");
    cJSON *static_netmask = cJSON_GetObjectItem(root, "static_netmask");
    cJSON *matrix_width = cJSON_GetObjectItem(root, "matrix_width");
    cJSON *matrix_height = cJSON_GetObjectItem(root, "matrix_height");
    cJSON *orientation = cJSON_GetObjectItem(root, "orientation_deg");

    if (cJSON_IsString(wifi_ssid)) {
        strlcpy(config.wifi_ssid, wifi_ssid->valuestring, sizeof(config.wifi_ssid));
    }
    if (cJSON_IsString(wifi_password)) {
        strlcpy(config.wifi_password, wifi_password->valuestring, sizeof(config.wifi_password));
    }
    if (cJSON_IsString(device_name)) {
        strlcpy(config.device_name, device_name->valuestring, sizeof(config.device_name));
    }
    if (cJSON_IsString(hostname)) {
        strlcpy(config.hostname, hostname->valuestring, sizeof(config.hostname));
    }
    if (cJSON_IsString(timezone)) {
        strlcpy(config.timezone, timezone->valuestring, sizeof(config.timezone));
    }
    if (cJSON_IsString(static_ip)) {
        strlcpy(config.static_ip, static_ip->valuestring, sizeof(config.static_ip));
    }
    if (cJSON_IsString(static_gateway)) {
        strlcpy(config.static_gateway, static_gateway->valuestring, sizeof(config.static_gateway));
    }
    if (cJSON_IsString(static_netmask)) {
        strlcpy(config.static_netmask, static_netmask->valuestring, sizeof(config.static_netmask));
    }
    if (cJSON_IsNumber(matrix_width)) {
        config.matrix_width = matrix_width->valueint;
    }
    if (cJSON_IsNumber(matrix_height)) {
        config.matrix_height = matrix_height->valueint;
    }
    if (cJSON_IsNumber(orientation)) {
        config.orientation_deg = orientation->valueint;
    }

    if (config.wifi_ssid[0] != '\0' && config.device_name[0] != '\0' && config.hostname[0] != '\0' && config.timezone[0] != '\0') {
        config.setup_complete = true;
    }

    cJSON_Delete(root);
    if (pd_config_save(&config) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
    }

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pd_network_start_http(void)
{
    if (pd_http_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = pd_network_config.http_port;
    config.max_uri_handlers = 16;
    config.max_resp_headers = 8;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.stack_size = 8192;

    if (httpd_start(&pd_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start http server");
        return ESP_FAIL;
    }

    httpd_uri_t reload_uri = {
        .uri = "/reload",
        .method = HTTP_POST,
        .handler = pd_http_reload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &reload_uri);

    httpd_uri_t state_uri = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = pd_http_state_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &state_uri);

    httpd_uri_t list_uri = {
        .uri = "/list",
        .method = HTTP_GET,
        .handler = pd_http_list_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &list_uri);

    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = pd_http_upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &upload_uri);

    httpd_uri_t wizard_status_uri = {
        .uri = "/wizard",
        .method = HTTP_GET,
        .handler = pd_http_wizard_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &wizard_status_uri);

    httpd_uri_t wizard_submit_uri = {
        .uri = "/wizard",
        .method = HTTP_POST,
        .handler = pd_http_wizard_submit_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(pd_http_server, &wizard_submit_uri);

    ESP_LOGI(TAG, "http server started on port %d", config.server_port);
    return ESP_OK;
}

static void pd_network_start_udp(void)
{
    if (pd_udp_socket >= 0) {
        return;
    }

    pd_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (pd_udp_socket < 0) {
        ESP_LOGE(TAG, "udp socket create failed: %s", strerror(errno));
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(pd_network_config.udp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(pd_udp_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "udp bind failed: %s", strerror(errno));
        close(pd_udp_socket);
        pd_udp_socket = -1;
        return;
    }

    int flags = fcntl(pd_udp_socket, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pd_udp_socket, F_SETFL, flags | O_NONBLOCK);
    }

    ESP_LOGI(TAG, "udp socket listening on %d", pd_network_config.udp_port);
}

static void pd_network_check_udp(void)
{
    if (pd_udp_socket < 0) {
        return;
    }

    char buffer[64];
    struct sockaddr_in source = {0};
    socklen_t addr_len = sizeof(source);
    int received = recvfrom(pd_udp_socket, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&source, &addr_len);
    if (received <= 0) {
        return;
    }

    buffer[received] = '\0';
    ESP_LOGI(TAG, "udp doorbell: %s", buffer);
    pd_display_state_t state;
    pd_storage_load_state(&state);
}

static void pd_network_poll_now_json(void)
{
    const char *base_path = pd_storage_get_base_path();
    char path[128];
    snprintf(path, sizeof(path), "%s/now.json", base_path);
    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }

    if (pd_now_mtime == 0) {
        pd_now_mtime = st.st_mtime;
        return;
    }

    if (st.st_mtime != pd_now_mtime) {
        pd_now_mtime = st.st_mtime;
        ESP_LOGI(TAG, "now.json updated");
        pd_display_state_t state;
        pd_storage_load_state(&state);
    }
}

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
    if (pd_network_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "wifi credentials missing; configure ssid/password before starting network");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &pd_network_handle_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &pd_network_handle_wifi_event, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, pd_network_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pd_network_config.wifi_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    if (pd_network_config.hostname[0] != '\0') {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_set_hostname(netif, pd_network_config.hostname);
        }
    }

    if (pd_network_config.static_ip[0] != '\0') {
        esp_netif_ip_info_t ip_info = {0};
        ip4_addr_t ip = {0};
        ip4_addr_t gw = {0};
        ip4_addr_t netmask = {0};
        if (ip4addr_aton(pd_network_config.static_ip, &ip)) {
            ip_info.ip.addr = ip.addr;
        }
        if (ip4addr_aton(pd_network_config.static_gateway, &gw)) {
            ip_info.gw.addr = gw.addr;
        }
        if (ip4addr_aton(pd_network_config.static_netmask, &netmask)) {
            ip_info.netmask.addr = netmask.addr;
        }
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_dhcpc_stop(netif);
            esp_netif_set_ip_info(netif, &ip_info);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi init started");
    pd_network_start_http();
    pd_network_start_udp();
}

void pd_network_poll(void)
{
    pd_network_check_udp();
    pd_network_poll_now_json();
    if (!pd_http_server && pd_wifi_connected) {
        pd_network_start_http();
    }
}

bool pd_network_is_connected(void)
{
    return pd_wifi_connected;
}

const char *pd_network_get_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return NULL;
    static char ip_str[16];
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        return ip_str;
    }
    return NULL;
}

httpd_handle_t pd_network_get_http_server(void)
{
    return pd_http_server;
}

void pd_network_suspend(void)
{
    pd_network_suspended = true;
    ESP_LOGI(TAG, "network auto-reconnect suspended");
}

void pd_network_resume(void)
{
    pd_network_suspended = false;
    ESP_LOGI(TAG, "network auto-reconnect resumed");
}
