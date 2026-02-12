#ifndef PD_NETWORK_H
#define PD_NETWORK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int http_port;
    int udp_port;
} pd_network_config_t;

esp_err_t pd_network_init(const pd_network_config_t *config);
void pd_network_start(void);
void pd_network_poll(void);

#ifdef __cplusplus
}
#endif

#endif
