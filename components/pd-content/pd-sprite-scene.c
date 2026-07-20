#include "pd-sprite-scene.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lodepng.h"
#include "pd-display.h"

#define PD_SPRITE_MAX_SLOTS 8
#define PD_SPRITE_MAX_PATH  128

static const char *TAG = "pd-sprite";

typedef struct {
    char letter;
    char name[48];
    bool is_dir;
    char path[PD_SPRITE_MAX_PATH]; /* absolute file or directory */
    char pattern[64];
    int  frame_start;
    int  frame_count;
    int  fps;
    bool loop;

    int  w;
    int  h;
    int  park_x;
    int  park_y;

    /* Current pixels: prefer indexed when PNG is paletted. */
    bool     has_palette;
    int      palette_size;
    uint8_t  palette[256][4];
    uint8_t *indices; /* w*h, SPIRAM */
    uint8_t *rgb;     /* w*h*3, SPIRAM — used when !has_palette */

    int  anim_frame; /* absolute frame index for seq slots */
    int64_t anim_last_us;
    bool anim_active;

    float x;
    float y;
    bool  visible;

    bool last_valid;
    int  last_x;
    int  last_y;
    int  last_w;
    int  last_h;
} pd_sprite_slot_t;

typedef enum {
    PD_BUMP_IDLE = 0,
    PD_BUMP_SLIDE,
    PD_BUMP_OUTGOING_EXIT, /* slot0 parked; finish pushing A off-screen */
    PD_BUMP_ELASTIC,
    PD_BUMP_GAP,
    PD_BUMP_ASSEMBLED,
} pd_bump_phase_t;

static bool s_active = false;
static pd_sprite_slot_t s_slots[PD_SPRITE_MAX_SLOTS];
static int s_slot_count = 0;
static int s_scene_fps = 24;
static int s_slide_ms = 420;
static int s_elastic_ms = 160;
static int s_gap_ms = 60;
static int s_overshoot_px = 3;
static int s_enter_extra = 4; /* start this many px past right edge */

static pd_bump_phase_t s_phase = PD_BUMP_IDLE;
static int s_active_slot = 0;
static int64_t s_phase_start_us = 0;
static float s_slide_from_x = 0.0f;
static float s_slide_to_x = 0.0f;
static bool s_need_clear = true;
static bool s_presented_once = false;

/* Frozen outgoing layer (destination-owned bump pushes this off-axis). */
static uint8_t *s_out_rgb = NULL;
static int s_out_w = 0;
static int s_out_h = 0;
static float s_out_x = 0.0f;
static float s_out_y = 0.0f;
static bool s_out_active = false;
static bool s_out_last_valid = false;
static int s_out_last_x = 0;
static int s_out_last_y = 0;
static int s_out_last_w = 0;
static int s_out_last_h = 0;
static float s_out_exit_from = 0.0f;
static float s_out_exit_to = 0.0f;
static int s_out_exit_ms = 200;

static void clear_outgoing(void)
{
    free(s_out_rgb);
    s_out_rgb = NULL;
    s_out_w = 0;
    s_out_h = 0;
    s_out_x = 0.0f;
    s_out_y = 0.0f;
    s_out_active = false;
    s_out_last_valid = false;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_png_name(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return false;
    return strcasecmp(name + len - 4, ".png") == 0;
}

/* Letter block: "A-foo.png" or "B-mai-bounce" (dir). */
static bool parse_letter_prefix(const char *name, char *letter_out, const char **rest_out)
{
    if (!name || !name[0] || name[1] != '-') return false;
    char c = name[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') return false;
    if (letter_out) *letter_out = c;
    if (rest_out) *rest_out = name + 2;
    return true;
}

static void *spiram_or_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    return p;
}

static int count_frames_pattern(const char *dir, const char *pattern, int start)
{
    int count = 0;
    char path[PD_SPRITE_MAX_PATH];
    for (int i = start; i < start + 4096; i++) {
        snprintf(path, sizeof(path), "%s/", dir);
        size_t base = strlen(path);
        snprintf(path + base, sizeof(path) - base, pattern, i);
        if (!path_exists(path)) break;
        count++;
    }
    return count;
}

static bool discover_frames(const char *dir, char *pattern_out, size_t pattern_size,
                            int *start_out, int *count_out)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    char first_name[64] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!is_png_name(ent->d_name)) continue;
        if (first_name[0] == '\0' || strcmp(ent->d_name, first_name) < 0) {
            strlcpy(first_name, ent->d_name, sizeof(first_name));
        }
    }
    closedir(d);
    if (first_name[0] == '\0') return false;

    size_t stem_len = strlen(first_name);
    if (stem_len > 4) stem_len -= 4;
    size_t digits = 0;
    while (stem_len > digits && isdigit((unsigned char)first_name[stem_len - 1 - digits])) {
        digits++;
    }
    if (digits == 0 || digits > 8) {
        int c1 = count_frames_pattern(dir, "%04d.png", 1);
        int c0 = count_frames_pattern(dir, "%04d.png", 0);
        if (c1 <= 0 && c0 <= 0) return false;
        strlcpy(pattern_out, "%04d.png", pattern_size);
        *start_out = (c0 > c1) ? 0 : 1;
        *count_out = (c0 > c1) ? c0 : c1;
        return true;
    }

    size_t prefix_len = stem_len - digits;
    char prefix[64];
    if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
    memcpy(prefix, first_name, prefix_len);
    prefix[prefix_len] = '\0';

    char numbuf[16];
    memcpy(numbuf, first_name + prefix_len, digits);
    numbuf[digits] = '\0';
    int first_num = atoi(numbuf);

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s%%0%dd.png", prefix, (int)digits);
    int count = count_frames_pattern(dir, pattern, first_num);
    if (count <= 0) return false;
    strlcpy(pattern_out, pattern, pattern_size);
    *start_out = first_num;
    *count_out = count;
    return true;
}

static bool load_png_pixels(const char *path, pd_sprite_slot_t *slot)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return false; }
    unsigned char *png = malloc((size_t)sz);
    if (!png) { fclose(f); return false; }
    fread(png, 1, (size_t)sz, f);
    fclose(f);

    LodePNGState state;
    lodepng_state_init(&state);
    unsigned w = 0, h = 0;
    unsigned err = lodepng_inspect(&w, &h, &state, png, (size_t)sz);
    bool pal = (!err && state.info_png.color.colortype == LCT_PALETTE);

    free(slot->indices);
    free(slot->rgb);
    slot->indices = NULL;
    slot->rgb = NULL;
    slot->has_palette = false;

    if (pal) {
        state.info_raw.colortype = LCT_PALETTE;
        state.info_raw.bitdepth = 8;
        unsigned char *raw = NULL;
        err = lodepng_decode(&raw, &w, &h, &state, png, (size_t)sz);
        if (!err && raw) {
            int n = (int)state.info_png.color.palettesize;
            if (n > 256) n = 256;
            slot->indices = spiram_or_malloc((size_t)w * h);
            if (slot->indices) {
                memcpy(slot->indices, raw, (size_t)w * h);
                free(raw);
                slot->has_palette = true;
                slot->palette_size = n;
                memcpy(slot->palette, state.info_png.color.palette, (size_t)n * 4);
                slot->w = (int)w;
                slot->h = (int)h;
                lodepng_state_cleanup(&state);
                free(png);
                return true;
            }
            free(raw);
        }
    }
    lodepng_state_cleanup(&state);

    unsigned char *rgba = NULL;
    err = lodepng_decode32(&rgba, &w, &h, png, (size_t)sz);
    free(png);
    if (err || !rgba) return false;

    size_t need = (size_t)w * h * 3;
    slot->rgb = spiram_or_malloc(need);
    if (!slot->rgb) {
        free(rgba);
        return false;
    }
    for (unsigned i = 0; i < w * h; i++) {
        slot->rgb[i * 3 + 0] = rgba[i * 4 + 0];
        slot->rgb[i * 3 + 1] = rgba[i * 4 + 1];
        slot->rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    free(rgba);
    slot->w = (int)w;
    slot->h = (int)h;
    slot->has_palette = false;
    return true;
}

static bool load_slot_frame(pd_sprite_slot_t *slot, int frame_idx)
{
    if (!slot->is_dir) {
        return load_png_pixels(slot->path, slot);
    }
    char fp[PD_SPRITE_MAX_PATH];
    snprintf(fp, sizeof(fp), "%s/", slot->path);
    size_t base = strlen(fp);
    snprintf(fp + base, sizeof(fp) - base, slot->pattern, frame_idx);
    return load_png_pixels(fp, slot);
}

static void erase_rect_diff(int ox, int oy, int ow, int oh,
                            int nx, int ny, int nw, int nh)
{
    if (ow <= 0 || oh <= 0) return;
    int ox1 = ox + ow;
    int oy1 = oy + oh;
    int nx1 = nx + nw;
    int ny1 = ny + nh;
    int ix0 = ox > nx ? ox : nx;
    int iy0 = oy > ny ? oy : ny;
    int ix1 = ox1 < nx1 ? ox1 : nx1;
    int iy1 = oy1 < ny1 ? oy1 : ny1;
    if (ix0 >= ix1 || iy0 >= iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)oh, PD_COLOR_BLACK);
        return;
    }
    if (oy < iy0) {
        pd_display_fill((uint16_t)ox, (uint16_t)oy, (uint16_t)ow, (uint16_t)(iy0 - oy), PD_COLOR_BLACK);
    }
    if (oy1 > iy1) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy1, (uint16_t)ow, (uint16_t)(oy1 - iy1), PD_COLOR_BLACK);
    }
    if (ox < ix0) {
        pd_display_fill((uint16_t)ox, (uint16_t)iy0, (uint16_t)(ix0 - ox), (uint16_t)(iy1 - iy0), PD_COLOR_BLACK);
    }
    if (ox1 > ix1) {
        pd_display_fill((uint16_t)ix1, (uint16_t)iy0, (uint16_t)(ox1 - ix1), (uint16_t)(iy1 - iy0), PD_COLOR_BLACK);
    }
}

static void sync_outgoing_push_from_sprite(float sprite_x)
{
    /* bump-left: sprite left edge pushes outgoing right edge. */
    if (!s_out_active || s_out_w <= 0) return;
    float out_right = s_out_x + (float)s_out_w;
    if (sprite_x < out_right) {
        s_out_x = sprite_x - (float)s_out_w;
    }
}

static void present_slots(void)
{
    if (s_need_clear) {
        pd_display_clear();
        s_need_clear = false;
        for (int i = 0; i < s_slot_count; i++) {
            s_slots[i].last_valid = false;
        }
        s_out_last_valid = false;
    }

    /* Erase moved outgoing + slot rects (old∖new). */
    if (s_out_active && s_out_rgb) {
        int ox = (int)lroundf(s_out_x);
        int oy = (int)lroundf(s_out_y);
        if (s_out_last_valid &&
            (s_out_last_x != ox || s_out_last_y != oy ||
             s_out_last_w != s_out_w || s_out_last_h != s_out_h)) {
            erase_rect_diff(s_out_last_x, s_out_last_y, s_out_last_w, s_out_last_h,
                            ox, oy, s_out_w, s_out_h);
        }
    } else if (s_out_last_valid) {
        erase_rect_diff(s_out_last_x, s_out_last_y, s_out_last_w, s_out_last_h,
                        0, 0, 0, 0);
        s_out_last_valid = false;
    }

    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        if (!s->visible || s->w <= 0) continue;
        int x = (int)lroundf(s->x);
        int y = (int)lroundf(s->y);
        if (s->last_valid &&
            (s->last_x != x || s->last_y != y || s->last_w != s->w || s->last_h != s->h)) {
            erase_rect_diff(s->last_x, s->last_y, s->last_w, s->last_h, x, y, s->w, s->h);
        }
    }

    /* Outgoing under sprites. */
    if (s_out_active && s_out_rgb) {
        int ox = (int)lroundf(s_out_x);
        int oy = (int)lroundf(s_out_y);
        pd_display_render_rgb_at(ox, oy, s_out_rgb, s_out_w, s_out_h);
        s_out_last_x = ox;
        s_out_last_y = oy;
        s_out_last_w = s_out_w;
        s_out_last_h = s_out_h;
        s_out_last_valid = true;
    }

    /* Draw back-to-front (letter order). */
    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        if (!s->visible || s->w <= 0) continue;
        int x = (int)lroundf(s->x);
        int y = (int)lroundf(s->y);
        if (s->has_palette && s->indices) {
            pd_display_render_indexed_at(x, y, s->indices, s->w, s->h,
                                         s->palette, s->palette_size);
        } else if (s->rgb) {
            pd_display_render_rgb_at(x, y, s->rgb, s->w, s->h);
        }
        s->last_x = x;
        s->last_y = y;
        s->last_w = s->w;
        s->last_h = s->h;
        s->last_valid = true;
    }
    s_presented_once = true;
}

static float ease_out_cubic(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static float ease_out_back(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static void compute_park_positions(void)
{
    /* Center the final horizontal stack on the matrix (bump-left axis).
     * origin_x = max(0, (W - stack_w) / 2); park_x[i] = origin + prefix widths. */
    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    int stack_w = 0;
    for (int i = 0; i < s_slot_count; i++) {
        stack_w += s_slots[i].w;
    }
    int origin_x = 0;
    if (stack_w > 0 && stack_w < dw) {
        origin_x = (dw - stack_w) / 2;
    }
    int x = origin_x;
    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        s->park_x = x;
        s->park_y = (s->h < dh) ? (dh - s->h) / 2 : 0;
        x += s->w;
    }
}

static void begin_slide_for_slot(int idx, int64_t now_us)
{
    int dw = pd_display_get_width();
    pd_sprite_slot_t *s = &s_slots[idx];
    s->visible = true;
    s->y = (float)s->park_y;
    s_slide_from_x = (float)(dw + s_enter_extra);
    s_slide_to_x = (float)s->park_x;
    s->x = s_slide_from_x;
    s_phase = PD_BUMP_SLIDE;
    s_phase_start_us = now_us;
    s_active_slot = idx;
}

int pd_sprite_scene_count_slots(const char *dir_path)
{
    if (!dir_path || !path_is_dir(dir_path)) return 0;
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    int letters = 0;
    bool seen[26] = {false};
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char letter = 0;
        if (!parse_letter_prefix(ent->d_name, &letter, NULL)) continue;
        char full[PD_SPRITE_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        bool ok = false;
        if (path_is_dir(full)) ok = true;
        else if (is_png_name(ent->d_name)) ok = true;
        if (!ok) continue;
        int li = letter - 'A';
        if (li < 0 || li >= 26 || seen[li]) continue;
        seen[li] = true;
        letters++;
    }
    closedir(d);
    return letters;
}

bool pd_sprite_scene_detect(const char *dir_path)
{
    return pd_sprite_scene_count_slots(dir_path) >= 2;
}

static void clear_slots(void)
{
    for (int i = 0; i < PD_SPRITE_MAX_SLOTS; i++) {
        free(s_slots[i].indices);
        free(s_slots[i].rgb);
        memset(&s_slots[i], 0, sizeof(s_slots[i]));
        strlcpy(s_slots[i].pattern, "%04d.png", sizeof(s_slots[i].pattern));
        s_slots[i].frame_start = 1;
        s_slots[i].fps = 12;
        s_slots[i].loop = true;
    }
    s_slot_count = 0;
}

static void apply_meta(const char *dir_path)
{
    char path[PD_SPRITE_MAX_PATH];
    snprintf(path, sizeof(path), "%s/meta.json", dir_path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 8192) { fclose(f); return; }
    char *buf = calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *j;
    j = cJSON_GetObjectItem(root, "fps");
    if (cJSON_IsNumber(j) && j->valueint > 0) s_scene_fps = j->valueint;
    j = cJSON_GetObjectItem(root, "slide_ms");
    if (cJSON_IsNumber(j) && j->valueint > 0) s_slide_ms = j->valueint;
    j = cJSON_GetObjectItem(root, "elastic_ms");
    if (cJSON_IsNumber(j) && j->valueint > 0) s_elastic_ms = j->valueint;
    j = cJSON_GetObjectItem(root, "gap_ms");
    if (cJSON_IsNumber(j) && j->valueint >= 0) s_gap_ms = j->valueint;
    j = cJSON_GetObjectItem(root, "overshoot_px");
    if (cJSON_IsNumber(j) && j->valueint >= 0) s_overshoot_px = j->valueint;

    cJSON *slots = cJSON_GetObjectItem(root, "slots");
    if (cJSON_IsArray(slots)) {
        for (int i = 0; i < s_slot_count; i++) {
            cJSON *item = cJSON_GetArrayItem(slots, i);
            if (!item) continue;
            cJSON *jf = cJSON_GetObjectItem(item, "fps");
            if (cJSON_IsNumber(jf) && jf->valueint > 0) s_slots[i].fps = jf->valueint;
            cJSON *jl = cJSON_GetObjectItem(item, "loop");
            if (cJSON_IsBool(jl)) s_slots[i].loop = cJSON_IsTrue(jl);
            cJSON *jp = cJSON_GetObjectItem(item, "play");
            if (cJSON_IsString(jp) && jp->valuestring) {
                if (strcmp(jp->valuestring, "once") == 0) s_slots[i].loop = false;
                else if (strcmp(jp->valuestring, "loop") == 0) s_slots[i].loop = true;
            }
        }
    }
    cJSON_Delete(root);
}

esp_err_t pd_sprite_scene_load(const char *dir_path)
{
    pd_sprite_scene_unload();
    if (!pd_sprite_scene_detect(dir_path)) {
        return ESP_ERR_NOT_FOUND;
    }

    clear_slots();
    s_scene_fps = 24;
    s_slide_ms = 420;
    s_elastic_ms = 160;
    s_gap_ms = 60;
    s_overshoot_px = 3;

    typedef struct {
        char letter;
        char name[64];
        bool is_dir;
    } disc_t;
    disc_t found[PD_SPRITE_MAX_SLOTS];
    int nfound = 0;

    DIR *d = opendir(dir_path);
    if (!d) return ESP_ERR_NOT_FOUND;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && nfound < PD_SPRITE_MAX_SLOTS) {
        if (ent->d_name[0] == '.') continue;
        char letter = 0;
        const char *rest = NULL;
        if (!parse_letter_prefix(ent->d_name, &letter, &rest)) continue;
        char full[PD_SPRITE_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        bool is_dir = path_is_dir(full);
        if (!is_dir && !is_png_name(ent->d_name)) continue;
        /* de-dupe letter: keep lexicographically first name */
        int exist = -1;
        for (int i = 0; i < nfound; i++) {
            if (found[i].letter == letter) { exist = i; break; }
        }
        if (exist >= 0) {
            if (strcmp(ent->d_name, found[exist].name) < 0) {
                strlcpy(found[exist].name, ent->d_name, sizeof(found[exist].name));
                found[exist].is_dir = is_dir;
            }
            continue;
        }
        found[nfound].letter = letter;
        strlcpy(found[nfound].name, ent->d_name, sizeof(found[nfound].name));
        found[nfound].is_dir = is_dir;
        nfound++;
    }
    closedir(d);

    /* Sort by letter. */
    for (int i = 0; i < nfound; i++) {
        for (int j = i + 1; j < nfound; j++) {
            if (found[j].letter < found[i].letter) {
                disc_t tmp = found[i];
                found[i] = found[j];
                found[j] = tmp;
            }
        }
    }

    for (int i = 0; i < nfound; i++) {
        pd_sprite_slot_t *s = &s_slots[s_slot_count];
        s->letter = found[i].letter;
        strlcpy(s->name, found[i].name, sizeof(s->name));
        snprintf(s->path, sizeof(s->path), "%s/%s", dir_path, found[i].name);
        s->is_dir = found[i].is_dir;
        s->fps = 12;
        s->loop = true;
        if (s->is_dir) {
            if (!discover_frames(s->path, s->pattern, sizeof(s->pattern),
                                 &s->frame_start, &s->frame_count) ||
                s->frame_count <= 0) {
                ESP_LOGW(TAG, "slot %c: no frames in %s", s->letter, s->path);
                continue;
            }
            /* Optional per-folder meta.json for fps / loop / pattern. */
            char smeta[PD_SPRITE_MAX_PATH];
            snprintf(smeta, sizeof(smeta), "%s/meta.json", s->path);
            FILE *sf = fopen(smeta, "r");
            if (sf) {
                fseek(sf, 0, SEEK_END);
                long ssz = ftell(sf);
                rewind(sf);
                if (ssz > 0 && ssz <= 4096) {
                    char *sbuf = calloc(1, (size_t)ssz + 1);
                    if (sbuf) {
                        fread(sbuf, 1, (size_t)ssz, sf);
                        cJSON *root = cJSON_Parse(sbuf);
                        free(sbuf);
                        if (root) {
                            cJSON *jf = cJSON_GetObjectItem(root, "fps");
                            if (cJSON_IsNumber(jf) && jf->valueint > 0) s->fps = jf->valueint;
                            cJSON *jl = cJSON_GetObjectItem(root, "loop");
                            if (cJSON_IsBool(jl)) s->loop = cJSON_IsTrue(jl);
                            cJSON *jp = cJSON_GetObjectItem(root, "pattern");
                            if (cJSON_IsString(jp) && jp->valuestring) {
                                strlcpy(s->pattern, jp->valuestring, sizeof(s->pattern));
                                s->frame_count = count_frames_pattern(s->path, s->pattern, s->frame_start);
                            }
                            cJSON *js = cJSON_GetObjectItem(root, "start");
                            if (cJSON_IsNumber(js)) {
                                s->frame_start = js->valueint;
                                s->frame_count = count_frames_pattern(s->path, s->pattern, s->frame_start);
                            }
                            cJSON_Delete(root);
                        }
                    }
                }
                fclose(sf);
            }
            s->anim_frame = s->frame_start;
            if (!load_slot_frame(s, s->anim_frame)) {
                ESP_LOGW(TAG, "slot %c: decode failed", s->letter);
                continue;
            }
        } else {
            s->frame_count = 1;
            s->frame_start = 0;
            s->anim_frame = 0;
            if (!load_slot_frame(s, 0)) {
                ESP_LOGW(TAG, "slot %c: decode failed", s->letter);
                continue;
            }
        }
        s_slot_count++;
    }

    if (s_slot_count < 2) {
        pd_sprite_scene_unload();
        return ESP_ERR_NOT_FOUND;
    }

    apply_meta(dir_path);
    compute_park_positions();
    s_active = true;
    s_phase = PD_BUMP_IDLE;
    s_need_clear = true;
    s_presented_once = false;
    ESP_LOGI(TAG, "loaded sprite scene %s slots=%d", dir_path, s_slot_count);
    return ESP_OK;
}

void pd_sprite_scene_unload(void)
{
    clear_slots();
    clear_outgoing();
    s_active = false;
    s_phase = PD_BUMP_IDLE;
    s_active_slot = 0;
    s_presented_once = false;
}

bool pd_sprite_scene_active(void)
{
    return s_active && s_slot_count > 0;
}

bool pd_sprite_scene_set_outgoing(const uint8_t *rgb, int w, int h, int x, int y)
{
    clear_outgoing();
    if (!rgb || w <= 0 || h <= 0) return false;
    size_t need = (size_t)w * (size_t)h * 3;
    s_out_rgb = spiram_or_malloc(need);
    if (!s_out_rgb) return false;
    memcpy(s_out_rgb, rgb, need);
    s_out_w = w;
    s_out_h = h;
    s_out_x = (float)x;
    s_out_y = (float)y;
    s_out_active = true;
    s_out_last_valid = false;
    return true;
}

void pd_sprite_scene_start_bump(void)
{
    if (!pd_sprite_scene_active()) return;
    for (int i = 0; i < s_slot_count; i++) {
        s_slots[i].visible = false;
        s_slots[i].anim_active = false;
        s_slots[i].last_valid = false;
        if (s_slots[i].is_dir) {
            s_slots[i].anim_frame = s_slots[i].frame_start;
            load_slot_frame(&s_slots[i], s_slots[i].anim_frame);
        }
    }
    /* Keep outgoing on screen; clear only if we have no snapshot. */
    s_need_clear = !s_out_active;
    s_out_last_valid = false;
    compute_park_positions();
    begin_slide_for_slot(0, esp_timer_get_time());
    if (s_out_active) {
        sync_outgoing_push_from_sprite(s_slots[0].x);
    }
    present_slots();
}

void pd_sprite_scene_enter_assembled(void)
{
    if (!pd_sprite_scene_active()) return;
    clear_outgoing();
    compute_park_positions();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        s->visible = true;
        s->x = (float)s->park_x;
        s->y = (float)s->park_y;
        s->last_valid = false;
        s->anim_active = false;
        if (s->is_dir && s->frame_count > 1) {
            s->anim_active = true;
            s->anim_last_us = now;
        }
    }
    s_phase = PD_BUMP_ASSEMBLED;
    s_need_clear = true;
    present_slots();
    ESP_LOGI(TAG, "sprite scene assembled (no bump) slots=%d", s_slot_count);
}

static void blit_rgb_rect(uint8_t *dst, int canvas_w, int canvas_h,
                          const uint8_t *src, int sw, int sh, int ox, int oy)
{
    if (!dst || !src || sw <= 0 || sh <= 0) return;
    for (int row = 0; row < sh; row++) {
        int dy = oy + row;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int col = 0; col < sw; col++) {
            int dx = ox + col;
            if (dx < 0 || dx >= canvas_w) continue;
            size_t sp = ((size_t)row * (size_t)sw + (size_t)col) * 3;
            size_t dp = ((size_t)dy * (size_t)canvas_w + (size_t)dx) * 3;
            dst[dp] = src[sp];
            dst[dp + 1] = src[sp + 1];
            dst[dp + 2] = src[sp + 2];
        }
    }
}

static void blit_slot_at(uint8_t *dst, int canvas_w, int canvas_h,
                         const pd_sprite_slot_t *s, int ox, int oy)
{
    if (!dst || !s || s->w <= 0 || s->h <= 0) return;
    for (int row = 0; row < s->h; row++) {
        int dy = oy + row;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int col = 0; col < s->w; col++) {
            int dx = ox + col;
            if (dx < 0 || dx >= canvas_w) continue;
            uint8_t r = 0, g = 0, b = 0;
            if (s->has_palette && s->indices) {
                unsigned idx = s->indices[row * s->w + col];
                if ((int)idx >= s->palette_size) idx = (unsigned)(s->palette_size - 1);
                r = s->palette[idx][0];
                g = s->palette[idx][1];
                b = s->palette[idx][2];
            } else if (s->rgb) {
                size_t p = ((size_t)row * (size_t)s->w + (size_t)col) * 3;
                r = s->rgb[p];
                g = s->rgb[p + 1];
                b = s->rgb[p + 2];
            } else {
                continue;
            }
            size_t op = ((size_t)dy * (size_t)canvas_w + (size_t)dx) * 3;
            dst[op] = r;
            dst[op + 1] = g;
            dst[op + 2] = b;
        }
    }
}

bool pd_sprite_scene_rasterize_stack(uint8_t *rgb_out, int canvas_w, int canvas_h)
{
    if (!rgb_out || canvas_w <= 0 || canvas_h <= 0 || !pd_sprite_scene_active()) {
        return false;
    }
    size_t need = (size_t)canvas_w * (size_t)canvas_h * 3;
    memset(rgb_out, 0, need);
    compute_park_positions();
    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        if (s->w <= 0 || s->h <= 0) continue;
        blit_slot_at(rgb_out, canvas_w, canvas_h, s, s->park_x, s->park_y);
    }
    return true;
}

bool pd_sprite_scene_capture_to(uint8_t *rgb_out, int canvas_w, int canvas_h)
{
    if (!rgb_out || canvas_w <= 0 || canvas_h <= 0 || !pd_sprite_scene_active()) {
        return false;
    }
    size_t need = (size_t)canvas_w * (size_t)canvas_h * 3;
    memset(rgb_out, 0, need);

    /* Outgoing under sprites (same draw order as present_slots). */
    if (s_out_active && s_out_rgb) {
        blit_rgb_rect(rgb_out, canvas_w, canvas_h, s_out_rgb, s_out_w, s_out_h,
                      (int)lroundf(s_out_x), (int)lroundf(s_out_y));
    }

    for (int i = 0; i < s_slot_count; i++) {
        pd_sprite_slot_t *s = &s_slots[i];
        if (!s->visible || s->w <= 0) continue;
        blit_slot_at(rgb_out, canvas_w, canvas_h, s,
                     (int)lroundf(s->x), (int)lroundf(s->y));
    }
    return true;
}

bool pd_sprite_scene_bump_active(void)
{
    return s_active && s_phase != PD_BUMP_IDLE && s_phase != PD_BUMP_ASSEMBLED;
}

static void finish_slot_park(int idx, int64_t now_us)
{
    pd_sprite_slot_t *s = &s_slots[idx];
    s->x = (float)s->park_x;
    s->y = (float)s->park_y;
    if (idx + 1 < s_slot_count) {
        if (s_gap_ms > 0) {
            s_phase = PD_BUMP_GAP;
            s_phase_start_us = now_us;
        } else {
            begin_slide_for_slot(idx + 1, now_us);
        }
    } else {
        s_phase = PD_BUMP_ASSEMBLED;
        clear_outgoing();
        for (int i = 0; i < s_slot_count; i++) {
            if (s_slots[i].is_dir && s_slots[i].frame_count > 1) {
                s_slots[i].anim_active = true;
                s_slots[i].anim_last_us = now_us;
            }
        }
        ESP_LOGI(TAG, "sprite-bump-left assembled (%d slots)", s_slot_count);
    }
}

static void begin_outgoing_exit_if_needed(int64_t now_us)
{
    if (!s_out_active || s_out_w <= 0) {
        finish_slot_park(0, now_us);
        return;
    }
    float right = s_out_x + (float)s_out_w;
    if (right <= 0.0f) {
        clear_outgoing();
        finish_slot_park(0, now_us);
        return;
    }
    s_out_exit_from = s_out_x;
    s_out_exit_to = -(float)s_out_w;
    int dw = pd_display_get_width();
    if (dw < 1) dw = 1;
    int dist = (int)ceilf(right);
    s_out_exit_ms = (dist * s_slide_ms) / dw;
    if (s_out_exit_ms < 80) s_out_exit_ms = 80;
    if (s_out_exit_ms > s_slide_ms) s_out_exit_ms = s_slide_ms;
    s_phase = PD_BUMP_OUTGOING_EXIT;
    s_phase_start_us = now_us;
}

bool pd_sprite_scene_tick(int64_t now_us)
{
    if (!pd_sprite_scene_active()) return false;

    bool dirty = false;

    if (s_phase == PD_BUMP_SLIDE) {
        pd_sprite_slot_t *s = &s_slots[s_active_slot];
        float t = (float)(now_us - s_phase_start_us) / (float)(s_slide_ms * 1000);
        if (t >= 1.0f) {
            s->x = s_slide_to_x;
            if (s_active_slot == 0) {
                sync_outgoing_push_from_sprite(s->x);
                begin_outgoing_exit_if_needed(now_us);
            } else if (s_elastic_ms > 0 && s_overshoot_px > 0) {
                /* Followers get elastic settle; first parks hard. */
                s_phase = PD_BUMP_ELASTIC;
                s_phase_start_us = now_us;
            } else {
                finish_slot_park(s_active_slot, now_us);
            }
        } else {
            float e = ease_out_cubic(t);
            s->x = s_slide_from_x + (s_slide_to_x - s_slide_from_x) * e;
            if (s_active_slot == 0) {
                sync_outgoing_push_from_sprite(s->x);
            }
        }
        dirty = true;
    } else if (s_phase == PD_BUMP_OUTGOING_EXIT) {
        pd_sprite_slot_t *s = &s_slots[0];
        s->x = (float)s->park_x;
        s->y = (float)s->park_y;
        float t = (float)(now_us - s_phase_start_us) / (float)(s_out_exit_ms * 1000);
        if (t >= 1.0f || !s_out_active) {
            clear_outgoing();
            finish_slot_park(0, now_us);
        } else {
            float e = ease_out_cubic(t);
            s_out_x = s_out_exit_from + (s_out_exit_to - s_out_exit_from) * e;
        }
        dirty = true;
    } else if (s_phase == PD_BUMP_ELASTIC) {
        pd_sprite_slot_t *s = &s_slots[s_active_slot];
        float t = (float)(now_us - s_phase_start_us) / (float)(s_elastic_ms * 1000);
        float park = (float)s->park_x;
        if (t >= 1.0f) {
            finish_slot_park(s_active_slot, now_us);
        } else {
            /* Pull slightly past park (left), then settle with ease-out-back. */
            float e = ease_out_back(t);
            float over = park - (float)s_overshoot_px;
            /* Map: start at park, dip to over, return to park. */
            float dip = sinf(e * 3.14159265f) * (park - over);
            s->x = park - dip;
        }
        dirty = true;
    } else if (s_phase == PD_BUMP_GAP) {
        if ((now_us - s_phase_start_us) >= (int64_t)s_gap_ms * 1000) {
            begin_slide_for_slot(s_active_slot + 1, now_us);
            dirty = true;
        }
    } else if (s_phase == PD_BUMP_ASSEMBLED) {
        for (int i = 0; i < s_slot_count; i++) {
            pd_sprite_slot_t *s = &s_slots[i];
            if (!s->anim_active || !s->is_dir || s->frame_count <= 1) continue;
            int fps = s->fps > 0 ? s->fps : 12;
            int64_t interval = 1000000 / fps;
            if (s->anim_last_us == 0) {
                s->anim_last_us = now_us;
                continue;
            }
            if ((now_us - s->anim_last_us) < interval) continue;
            s->anim_last_us += interval;
            if ((now_us - s->anim_last_us) > interval) s->anim_last_us = now_us;

            int next = s->anim_frame + 1;
            int last = s->frame_start + s->frame_count - 1;
            if (next > last) {
                if (s->loop) next = s->frame_start;
                else {
                    s->anim_active = false;
                    continue;
                }
            }
            if (load_slot_frame(s, next)) {
                s->anim_frame = next;
                dirty = true;
            }
        }
    }

    if (dirty || !s_presented_once) {
        present_slots();
        return true;
    }
    return false;
}

int pd_sprite_scene_ms_until_next(void)
{
    if (!pd_sprite_scene_active()) return -1;
    if (s_phase == PD_BUMP_SLIDE || s_phase == PD_BUMP_OUTGOING_EXIT ||
        s_phase == PD_BUMP_ELASTIC || s_phase == PD_BUMP_GAP) {
        return 0; /* motion wants tight loop */
    }
    if (s_phase == PD_BUMP_ASSEMBLED) {
        int64_t now = esp_timer_get_time();
        int64_t soonest = -1;
        for (int i = 0; i < s_slot_count; i++) {
            pd_sprite_slot_t *s = &s_slots[i];
            if (!s->anim_active) continue;
            int fps = s->fps > 0 ? s->fps : 12;
            int64_t interval = 1000000 / fps;
            int64_t due = s->anim_last_us + interval;
            int64_t remain = due - now;
            if (remain < 0) remain = 0;
            if (soonest < 0 || remain < soonest) soonest = remain;
        }
        if (soonest < 0) return 10;
        int ms = (int)((soonest + 999) / 1000);
        if (ms > 10) ms = 10;
        return ms;
    }
    return 0;
}

int pd_sprite_scene_slot_count(void)
{
    return s_slot_count;
}

int pd_sprite_scene_max_frames(void)
{
    int m = 0;
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].frame_count > m) m = s_slots[i].frame_count;
    }
    return m;
}

int pd_sprite_scene_fps(void)
{
    return s_scene_fps > 0 ? s_scene_fps : 24;
}
