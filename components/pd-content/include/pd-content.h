#ifndef PD_CONTENT_H
#define PD_CONTENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PD_CONTENT_MAX_PATH 128
#define PD_CONTENT_MAX_NAME  64
#define PD_CONTENT_MAX_LIST  16

typedef struct {
    char path[PD_CONTENT_MAX_PATH];
    char name[PD_CONTENT_MAX_NAME];
    bool is_sequence;
    int  frame_count;
    int  fps;
} pd_content_entry_t;

typedef struct {
    bool playing;
    char current_path[PD_CONTENT_MAX_PATH];
    bool is_sequence;
    int  current_frame;
    int  total_frames;
    int  fps;
} pd_content_status_t;

/* transition mode */
typedef enum {
    PD_TRANS_MODE_RANDOM,
    PD_TRANS_MODE_BASELINE,
    PD_TRANS_MODE_PER_ITEM,
} pd_trans_mode_t;

/* global content configuration */
typedef struct {
    /* transition settings */
    pd_trans_mode_t trans_mode;
    char trans_baseline[32];
    int  trans_duration_ms;

    /* display settings */
    int  hold_ms;
    bool loop_sequences;

    /* attract mode */
    bool attract_enabled;
    char attract_path[PD_CONTENT_MAX_PATH];
    bool attract_shuffle;
    int  attract_idle_timeout_ms;
} pd_content_config_t;

const pd_content_config_t *pd_content_get_config(void);
esp_err_t pd_content_set_config(const pd_content_config_t *cfg);
esp_err_t pd_content_save_config(void);

esp_err_t pd_content_init(const char *base_path);

int pd_content_list_images(pd_content_entry_t *entries, int max_entries);

esp_err_t pd_content_play(const char *path);
esp_err_t pd_content_play_with_transition(const char *path, const char *transition,
                                          int duration_ms);
esp_err_t pd_content_stop(void);
pd_content_status_t pd_content_get_status(void);

void pd_content_tick(void);

esp_err_t pd_content_register_http(httpd_handle_t server);

esp_err_t pd_content_store_file(const char *rel_path, const uint8_t *data, size_t len);
esp_err_t pd_content_delete_file(const char *rel_path);

#ifdef __cplusplus
}
#endif

#endif
