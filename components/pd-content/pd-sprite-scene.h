#ifndef PD_SPRITE_SCENE_H
#define PD_SPRITE_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Letter-prefixed sprite-block scenes (Phase 2).
 * Appearance order = A, B, C, … Each block is a PNG file or a frame folder. */

bool pd_sprite_scene_detect(const char *dir_path);
/* Count lettered blocks without decoding pixels (for list APIs). */
int pd_sprite_scene_count_slots(const char *dir_path);

esp_err_t pd_sprite_scene_load(const char *dir_path);
void pd_sprite_scene_unload(void);
bool pd_sprite_scene_active(void);

/* Attach a frozen full-canvas (or rect) RGB888 snapshot of the outgoing
 * content. Copied into SPIRAM; call after load(), before start_bump(). */
bool pd_sprite_scene_set_outgoing(const uint8_t *rgb, int w, int h, int x, int y);

/* Start sprite-bump-left (enter from right; push outgoing off left). */
void pd_sprite_scene_start_bump(void);

/* Skip bump: show centered stack and allow anim (after a classic FB wipe). */
void pd_sprite_scene_enter_assembled(void);

/* Rasterize the centered final stack into a tightly packed RGB888 canvas. */
bool pd_sprite_scene_rasterize_stack(uint8_t *rgb_out, int canvas_w, int canvas_h);

/* Capture the currently visible scene (outgoing under + slots at current
 * x/y) into a full-canvas RGB888 buffer for transition "from" freezes. */
bool pd_sprite_scene_capture_to(uint8_t *rgb_out, int canvas_w, int canvas_h);

/* Returns true when the panel was updated this tick. */
bool pd_sprite_scene_tick(int64_t now_us);
int pd_sprite_scene_ms_until_next(void);

bool pd_sprite_scene_bump_active(void);

int pd_sprite_scene_slot_count(void);
/* Rough status: max frames among animated slots (0 if all static). */
int pd_sprite_scene_max_frames(void);
int pd_sprite_scene_fps(void);

#ifdef __cplusplus
}
#endif

#endif
