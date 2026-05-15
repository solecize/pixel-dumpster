#ifndef PD_SERIAL_CMD_H
#define PD_SERIAL_CMD_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the serial command listener.
 *
 * Registers a callback with pd-wizard so that any JSON command received
 * over USB serial / UART after setup is complete gets routed here.
 *
 * Supported commands:
 *   {"cmd":"play","path":"<content_path>"[,"transition":"<name>","duration_ms":<ms>]}
 *   {"cmd":"stop"}
 *   {"cmd":"status"}
 *   {"cmd":"list"}
 */
esp_err_t pd_serial_cmd_init(void);

#ifdef __cplusplus
}
#endif

#endif
