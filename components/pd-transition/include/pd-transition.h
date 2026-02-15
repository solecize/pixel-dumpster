#ifndef PD_TRANSITION_H
#define PD_TRANSITION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- framebuffer ---- */

typedef struct {
    uint8_t *data;   /* RGB888: 3 bytes per pixel */
    int      width;
    int      height;
} pd_framebuf_t;

pd_framebuf_t *pd_framebuf_create(int width, int height);
void            pd_framebuf_destroy(pd_framebuf_t *fb);
void            pd_framebuf_clear(pd_framebuf_t *fb);
void            pd_framebuf_copy(pd_framebuf_t *dst, const pd_framebuf_t *src);
void            pd_framebuf_set_pixel(pd_framebuf_t *fb, int x, int y,
                                      uint8_t r, uint8_t g, uint8_t b);
void            pd_framebuf_get_pixel(const pd_framebuf_t *fb, int x, int y,
                                      uint8_t *r, uint8_t *g, uint8_t *b);
void            pd_framebuf_blit_rgb(pd_framebuf_t *fb, const uint8_t *rgb,
                                     int img_w, int img_h);

/* ---- transition types ---- */

typedef enum {
    PD_TRANS_NONE = 0,
    PD_TRANS_WIPE_LEFT,
    PD_TRANS_WIPE_RIGHT,
    PD_TRANS_WIPE_UP,
    PD_TRANS_WIPE_DOWN,
    PD_TRANS_SLIDE_LEFT,
    PD_TRANS_SLIDE_RIGHT,
    PD_TRANS_SLIDE_UP,
    PD_TRANS_SLIDE_DOWN,
    PD_TRANS_ROLL_UP,
    PD_TRANS_ROLL_DOWN,
    PD_TRANS_SPLIT_H,
    PD_TRANS_SPLIT_V,
    PD_TRANS_FADE,
    PD_TRANS_BLOCK_BUILD,
    PD_TRANS_PIXEL_BUILD,
    PD_TRANS_COUNT
} pd_transition_type_t;

/* ---- transition engine ---- */

typedef struct {
    pd_transition_type_t type;
    int                  duration_ms;
    int64_t              start_us;
    bool                 active;
    pd_framebuf_t       *from;
    pd_framebuf_t       *to;
    pd_framebuf_t       *out;
    /* block/pixel build state */
    uint16_t            *shuffle;       /* pixel index shuffle for pixel-build */
    int                  shuffle_count;
} pd_transition_t;

pd_transition_t *pd_transition_create(int width, int height);
void             pd_transition_destroy(pd_transition_t *t);

void pd_transition_start(pd_transition_t *t, pd_transition_type_t type,
                         int duration_ms);
bool pd_transition_tick(pd_transition_t *t);
bool pd_transition_is_active(const pd_transition_t *t);
float pd_transition_progress(const pd_transition_t *t);

const char *pd_transition_type_name(pd_transition_type_t type);
pd_transition_type_t pd_transition_type_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
