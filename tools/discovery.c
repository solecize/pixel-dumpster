#include "discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "cJSON.h"

/* Compatibility for systems without strlcpy */
#ifndef strlcpy
#define strlcpy(dst, src, size) do { \
    strncpy(dst, src, size - 1); \
    dst[size - 1] = '\0'; \
} while(0)
#endif

#ifndef strlcat
#define strlcat(dst, src, size) do { \
    size_t len = strlen(dst); \
    if (len < size - 1) { \
        strncpy(dst + len, src, size - len - 1); \
        dst[size - 1] = '\0'; \
    } \
} while(0)
#endif

#define DISCOVERY_PORT 7171
#define MAX_BUFFER 2048

static bool discovery_running = false;
static pthread_t discovery_thread;
static int discovery_socket = -1;

/* Daemon information */
static char g_hostname[64] = "";
static char g_daemon_version[16] = "";
static char g_es_version[32] = "";
static bool g_browsing_events = false;
static bool g_launch_events = false;
static char g_event_methods[128] = "";

static void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("[discovery] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

static void send_announce(const char *device_ip, int device_port)
{
    /* Get local IP */
    char local_ip[16] = "127.0.0.1";
    
    /* Build ANNOUNCE message */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ANNOUNCE");
    cJSON_AddStringToObject(root, "source_id", g_hostname);
    cJSON_AddStringToObject(root, "source_ip", local_ip);
    cJSON_AddStringToObject(root, "hostname", g_hostname);
    cJSON_AddStringToObject(root, "daemon_version", g_daemon_version);
    cJSON_AddStringToObject(root, "es_version", g_es_version);
    
    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "browsing_events", g_browsing_events);
    cJSON_AddBoolToObject(features, "launch_events", g_launch_events);
    
    /* Parse event methods string into array */
    cJSON *methods = cJSON_CreateArray();
    char methods_copy[128];
    strlcpy(methods_copy, g_event_methods, sizeof(methods_copy));
    char *method = strtok(methods_copy, ",");
    while (method) {
        /* Trim whitespace */
        while (*method == ' ') method++;
        char *end = method + strlen(method) - 1;
        while (end > method && *end == ' ') *end-- = '\0';
        
        if (*method) {
            cJSON_AddItemToArray(methods, cJSON_CreateString(method));
        }
        method = strtok(NULL, ",");
    }
    cJSON_AddItemToObject(features, "event_methods", methods);
    
    cJSON_AddItemToObject(root, "features", features);
    
    /* Serialize */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        log_info("ERROR: Failed to create ANNOUNCE JSON");
        return;
    }
    
    /* Send unicast response to device */
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DISCOVERY_PORT);
    inet_pton(AF_INET, device_ip, &dest_addr.sin_addr);
    
    int sent = sendto(discovery_socket, json_str, strlen(json_str), 0,
                     (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    if (sent < 0) {
        log_info("ERROR: Failed to send ANNOUNCE to %s", device_ip);
    } else {
        log_info("Sent ANNOUNCE to %s:%d", device_ip, device_port);
    }
    
    free(json_str);
}

static void process_discover(const char *json_data, const char *sender_ip)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        return;
    }
    
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || strcmp(type->valuestring, "DISCOVER") != 0) {
        cJSON_Delete(root);
        return;
    }
    
    const cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    const cJSON *device_ip = cJSON_GetObjectItem(root, "device_ip");
    const cJSON *api_port = cJSON_GetObjectItem(root, "api_port");
    
    if (!device_id || !device_ip || !api_port) {
        cJSON_Delete(root);
        return;
    }
    
    log_info("Received DISCOVER from %s (%s:%d)",
             device_id->valuestring, device_ip->valuestring, api_port->valueint);
    
    /* Send ANNOUNCE response */
    send_announce(device_ip->valuestring, api_port->valueint);
    
    cJSON_Delete(root);
}

static void *discovery_listener(void *arg)
{
    char buffer[MAX_BUFFER];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    
    log_info("Discovery listener started");
    
    while (discovery_running) {
        int received = recvfrom(discovery_socket, buffer, sizeof(buffer) - 1, 0,
                               (struct sockaddr *)&sender_addr, &sender_len);
        
        if (received > 0) {
            buffer[received] = '\0';
            
            char sender_ip[16];
            inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
            
            process_discover(buffer, sender_ip);
        }
    }
    
    log_info("Discovery listener stopped");
    return NULL;
}

bool discovery_init(const char *hostname, const char *daemon_version,
                   const char *es_version, bool browsing_events, bool launch_events,
                   const char **event_methods, int method_count)
{
    /* Store daemon information */
    strlcpy(g_hostname, hostname ? hostname : "unknown", sizeof(g_hostname));
    strlcpy(g_daemon_version, daemon_version ? daemon_version : "1.0.0", sizeof(g_daemon_version));
    strlcpy(g_es_version, es_version ? es_version : "unknown", sizeof(g_es_version));
    g_browsing_events = browsing_events;
    g_launch_events = launch_events;
    
    /* Build event methods string */
    g_event_methods[0] = '\0';
    for (int i = 0; i < method_count && event_methods[i]; i++) {
        if (i > 0) strlcat(g_event_methods, ", ", sizeof(g_event_methods));
        strlcat(g_event_methods, event_methods[i], sizeof(g_event_methods));
    }
    
    /* Create UDP socket */
    discovery_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (discovery_socket < 0) {
        log_info("ERROR: Failed to create socket");
        return false;
    }
    
    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        log_info("ERROR: Failed to enable broadcast");
        close(discovery_socket);
        return false;
    }
    
    /* Bind to discovery port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DISCOVERY_PORT);
    
    if (bind(discovery_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_info("ERROR: Failed to bind to port %d", DISCOVERY_PORT);
        close(discovery_socket);
        return false;
    }
    
    /* Set receive timeout */
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(discovery_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    log_info("Initialized: %s (ES %s) - browsing=%d launch=%d methods=[%s]",
             g_hostname, g_es_version, g_browsing_events, g_launch_events, g_event_methods);
    
    return true;
}

void discovery_start(void)
{
    if (discovery_running) {
        return;
    }
    
    discovery_running = true;
    if (pthread_create(&discovery_thread, NULL, discovery_listener, NULL) != 0) {
        log_info("ERROR: Failed to create listener thread");
        discovery_running = false;
        return;
    }
    
    log_info("Discovery service started");
}

void discovery_stop(void)
{
    if (!discovery_running) {
        return;
    }
    
    discovery_running = false;
    pthread_join(discovery_thread, NULL);
    
    if (discovery_socket >= 0) {
        close(discovery_socket);
        discovery_socket = -1;
    }
    
    log_info("Discovery service stopped");
}

void discovery_announce(void)
{
    /* Send broadcast announcement (for periodic heartbeat) */
    if (discovery_socket < 0) {
        return;
    }
    
    /* Note: For now, we only respond to DISCOVER queries
     * Periodic announcements could be added here if needed */
}
