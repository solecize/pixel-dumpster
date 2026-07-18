#ifndef PD_BLE_H
#define PD_BLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start NimBLE Nordic UART–style GATT peripheral.
 * Advertises `device_name` (falls back to "pixel-dumpster").
 * Host→device writes are fed into pd_wizard_feed_bytes; outbound wizard /
 * serial-cmd NDJSON is notified on the TX characteristic.
 */
esp_err_t pd_ble_start(const char *device_name);

bool pd_ble_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
