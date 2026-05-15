#ifndef PD_CONFIG_H
#define PD_CONFIG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int matrix_width;          /* virtual canvas width  (post-rotation, post-tile) */
    int matrix_height;         /* virtual canvas height (post-rotation, post-tile) */
    int orientation_deg;       /* legacy global rotation (kept for backward compat) */
    int scan_wiring;

    /* Multi-panel chain layout. For a single panel, panel_rows = panel_cols = 1
     * and panel_width/height match matrix_width/height. */
    int panel_width;           /* single module native width  (e.g. 64) */
    int panel_height;          /* single module native height (e.g. 32) */
    int panel_rows;            /* count arranged vertically   (>=1) */
    int panel_cols;            /* count arranged horizontally (>=1) */
    int chain_pattern;         /* enum mapping to Hub75PanelLayout */
    int panel_rotation_deg;    /* per-panel mount rotation 0/90/180/270 */
    int color_order;           /* 0=RGB, 1=BGR, 2=GRB, 3=BRG */

    char wifi_ssid[33];
    char wifi_password[65];
    char device_name[33];
    char hostname[64];
    char timezone[64];
    char static_ip[16];
    char static_gateway[16];
    char static_netmask[16];
    bool setup_complete;
    bool reztest_mode;
    bool reztest_done;
    int reztest_index;
    int config_version;
} pd_config_t;

esp_err_t pd_config_init(pd_config_t *config);
esp_err_t pd_config_load(pd_config_t *config);
esp_err_t pd_config_save(const pd_config_t *config);

/* Active config pointer — set once during init so other components can read/write layout */
void pd_config_set_active(pd_config_t *config);
pd_config_t *pd_config_get_active(void);

#ifdef __cplusplus
}
#endif

#endif
