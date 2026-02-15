#ifndef PD_NETWORK_H
#define PD_NETWORK_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_network_config_t {
    int http_port;
    int udp_port;
    char wifi_ssid[33];
    char wifi_password[65];
    char hostname[64];
    char static_ip[16];
    char static_gateway[16];
    char static_netmask[16];
} pd_network_config_t;

esp_err_t pd_network_init(const pd_network_config_t *config);
void pd_network_start(void);
void pd_network_poll(void);
bool pd_network_is_connected(void);
const char *pd_network_get_ip(void);
void pd_network_suspend(void);   /* suppress auto-reconnect */
void pd_network_resume(void);    /* re-enable auto-reconnect */
httpd_handle_t pd_network_get_http_server(void);

#ifdef __cplusplus
}
#endif

#endif
