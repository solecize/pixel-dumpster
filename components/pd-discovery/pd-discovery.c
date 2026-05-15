#include "pd-discovery.h"
#include "pd-network.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "pd-discovery";

static pd_marquee_source_t sources[PD_DISCOVERY_MAX_SOURCES];
static int source_count = 0;
static int discovery_socket = -1;
static TaskHandle_t discovery_task_handle = NULL;
static esp_timer_handle_t broadcast_timer = NULL;

static int connection_state = 0;  // 0=searching, 1=connecting, 2=connected

static void send_discover_broadcast(void);
static void discovery_task(void *pvParameters);
static void broadcast_timer_callback(void *arg);

esp_err_t pd_discovery_init(void)
{
    memset(sources, 0, sizeof(sources));
    source_count = 0;
    connection_state = 0;
    
    // Create UDP socket
    discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }
    
    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "Failed to set broadcast option");
        close(discovery_socket);
        return ESP_FAIL;
    }
    
    // Bind to discovery port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PD_DISCOVERY_PORT);
    
    if (bind(discovery_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(discovery_socket);
        return ESP_FAIL;
    }
    
    // Set receive timeout
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(discovery_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Start discovery task
    xTaskCreate(discovery_task, "discovery", 4096, NULL, 5, &discovery_task_handle);
    
    // Create broadcast timer (every 10 seconds)
    const esp_timer_create_args_t timer_args = {
        .callback = &broadcast_timer_callback,
        .name = "discovery_broadcast"
    };
    esp_timer_create(&timer_args, &broadcast_timer);
    esp_timer_start_periodic(broadcast_timer, 10000000);  // 10 seconds in microseconds
    
    // Send initial broadcast
    send_discover_broadcast();
    
    ESP_LOGI(TAG, "Discovery service initialized");
    return ESP_OK;
}

void pd_discovery_send_query(void)
{
    send_discover_broadcast();
}

static void send_discover_broadcast(void)
{
    const char *my_ip = pd_network_get_ip();
    if (!my_ip) {
        ESP_LOGW(TAG, "No IP address, skipping broadcast");
        return;
    }
    
    // Build discover message
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "DISCOVER");
    cJSON_AddStringToObject(root, "device_id", "pixel-dumpster");
    cJSON_AddStringToObject(root, "device_ip", my_ip);
    cJSON_AddNumberToObject(root, "api_port", 8088);
    
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("marquee_display"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("64x64"));
    cJSON_AddItemToObject(root, "capabilities", caps);
    
    cJSON_AddStringToObject(root, "version", "1.0.0");
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to create JSON");
        return;
    }
    
    // Send broadcast
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    dest_addr.sin_port = htons(PD_DISCOVERY_PORT);
    
    int sent = sendto(discovery_socket, json_str, strlen(json_str), 0,
                     (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    if (sent < 0) {
        ESP_LOGW(TAG, "Broadcast send failed");
    } else {
        ESP_LOGD(TAG, "Sent discovery broadcast");
        connection_state = 1;  // Connecting
    }
    
    free(json_str);
}

static void process_announce_message(cJSON *json, const char *sender_ip)
{
    const cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type || strcmp(type->valuestring, "ANNOUNCE") != 0) {
        return;
    }
    
    const cJSON *source_id = cJSON_GetObjectItem(json, "source_id");
    const cJSON *hostname = cJSON_GetObjectItem(json, "hostname");
    const cJSON *daemon_version = cJSON_GetObjectItem(json, "daemon_version");
    const cJSON *es_version = cJSON_GetObjectItem(json, "es_version");
    const cJSON *features = cJSON_GetObjectItem(json, "features");
    
    if (!source_id || !hostname) {
        ESP_LOGW(TAG, "Invalid ANNOUNCE message");
        return;
    }
    
    // Find or create source entry
    pd_marquee_source_t *src = NULL;
    for (int i = 0; i < source_count; i++) {
        if (strcmp(sources[i].source_id, source_id->valuestring) == 0) {
            src = &sources[i];
            break;
        }
    }
    
    if (!src && source_count < PD_DISCOVERY_MAX_SOURCES) {
        src = &sources[source_count++];
    }
    
    if (!src) {
        ESP_LOGW(TAG, "No space for new source");
        return;
    }
    
    // Update source info
    strlcpy(src->source_id, source_id->valuestring, sizeof(src->source_id));
    strlcpy(src->hostname, hostname->valuestring, sizeof(src->hostname));
    strlcpy(src->ip, sender_ip, sizeof(src->ip));
    
    if (daemon_version) {
        strlcpy(src->daemon_version, daemon_version->valuestring, sizeof(src->daemon_version));
    }
    if (es_version) {
        strlcpy(src->es_version, es_version->valuestring, sizeof(src->es_version));
    }
    
    if (features) {
        const cJSON *browsing = cJSON_GetObjectItem(features, "browsing_events");
        const cJSON *launch = cJSON_GetObjectItem(features, "launch_events");
        const cJSON *methods = cJSON_GetObjectItem(features, "event_methods");
        
        src->browsing_events = browsing && cJSON_IsTrue(browsing);
        src->launch_events = launch && cJSON_IsTrue(launch);
        
        if (methods && cJSON_IsArray(methods)) {
            src->event_methods[0] = '\0';
            int count = cJSON_GetArraySize(methods);
            for (int i = 0; i < count && i < 3; i++) {
                cJSON *item = cJSON_GetArrayItem(methods, i);
                if (item && item->valuestring) {
                    if (i > 0) strlcat(src->event_methods, ", ", sizeof(src->event_methods));
                    strlcat(src->event_methods, item->valuestring, sizeof(src->event_methods));
                }
            }
        }
    }
    
    src->priority = source_count - 1;  // Lower number = higher priority
    src->last_seen = (uint32_t)(esp_timer_get_time() / 1000000);
    src->active = true;
    connection_state = 2;  // Connected
    
    ESP_LOGI(TAG, "Registered source: %s (%s) - ES %s - %s events",
             src->hostname, src->ip, src->es_version,
             src->browsing_events ? "browsing+launch" : "launch only");
}

static void discovery_task(void *pvParameters)
{
    char rx_buffer[1024];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    
    while (1) {
        int len = recvfrom(discovery_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &socklen);
        
        if (len > 0) {
            rx_buffer[len] = '\0';
            
            char sender_ip[16];
            inet_ntoa_r(source_addr.sin_addr, sender_ip, sizeof(sender_ip));
            
            cJSON *json = cJSON_Parse(rx_buffer);
            if (json) {
                process_announce_message(json, sender_ip);
                cJSON_Delete(json);
            }
        }
        
        // Check for expired sources
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        for (int i = 0; i < source_count; i++) {
            if (sources[i].active && (now - sources[i].last_seen) > PD_DISCOVERY_SOURCE_TIMEOUT) {
                ESP_LOGW(TAG, "Source %s timed out", sources[i].hostname);
                sources[i].active = false;
            }
        }
        
        // Update connection state
        bool has_active = false;
        for (int i = 0; i < source_count; i++) {
            if (sources[i].active) {
                has_active = true;
                break;
            }
        }
        if (!has_active && connection_state == 2) {
            connection_state = 0;  // Back to searching
        }
    }
}

static void broadcast_timer_callback(void *arg)
{
    if (connection_state != 2) {
        send_discover_broadcast();
    }
}

pd_marquee_source_t* pd_discovery_get_active_source(void)
{
    // Return highest priority active source
    pd_marquee_source_t *best = NULL;
    for (int i = 0; i < source_count; i++) {
        if (sources[i].active) {
            if (!best || sources[i].priority < best->priority) {
                best = &sources[i];
            }
        }
    }
    return best;
}

bool pd_discovery_has_source(void)
{
    return pd_discovery_get_active_source() != NULL;
}

int pd_discovery_get_source_count(void)
{
    int count = 0;
    for (int i = 0; i < source_count; i++) {
        if (sources[i].active) count++;
    }
    return count;
}

pd_marquee_source_t* pd_discovery_get_source(int index)
{
    if (index >= 0 && index < source_count) {
        return &sources[index];
    }
    return NULL;
}

int pd_discovery_get_state(void)
{
    return connection_state;
}
