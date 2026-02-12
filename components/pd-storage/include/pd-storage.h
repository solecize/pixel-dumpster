#ifndef PD_STORAGE_H
#define PD_STORAGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;
} pd_storage_config_t;

esp_err_t pd_storage_init(const pd_storage_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
