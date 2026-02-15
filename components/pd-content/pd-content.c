#include "pd-content.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lodepng.h"
#include "pd-display.h"
#include "pd-transition.h"

static const char *TAG = "pd-content";

static char content_base[PD_CONTENT_MAX_PATH] = "";
static bool content_playing = false;
static bool content_is_seq = false;
static char content_current[PD_CONTENT_MAX_PATH] = "";
static int  content_frame = 0;
static int  content_total_frames = 0;
static int  content_fps = 12;
static bool content_loop = true;
static int64_t content_last_frame_us = 0;

/* ---- transition state ---- */
static pd_transition_t *content_transition = NULL;
static pd_framebuf_t   *content_fb = NULL;  /* current display framebuffer */

/* ---- helpers ---- */

static bool is_png(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return false;
    return strcasecmp(name + len - 4, ".png") == 0;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool path_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

static int count_sequence_frames(const char *dir_path)
{
    int count = 0;
    char path[PD_CONTENT_MAX_PATH];
    for (int i = 1; i <= 9999; i++) {
        snprintf(path, sizeof(path), "%s/%04d.png", dir_path, i);
        if (!path_exists(path)) break;
        count = i;
    }
    return count;
}

static bool load_sequence_meta(const char *dir_path, int *fps, bool *loop_out, int *frame_count)
{
    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/meta.json", dir_path);

    FILE *f = fopen(path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4096) { fclose(f); return false; }

    char *buf = calloc(1, sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *j_fps = cJSON_GetObjectItem(root, "fps");
    cJSON *j_loop = cJSON_GetObjectItem(root, "loop");

    if (fps && cJSON_IsNumber(j_fps)) *fps = j_fps->valueint;
    if (loop_out) *loop_out = cJSON_IsBool(j_loop) ? cJSON_IsTrue(j_loop) : true;
    if (frame_count) *frame_count = count_sequence_frames(dir_path);

    cJSON_Delete(root);
    return true;
}

/* ---- PNG decode ---- */

static uint8_t *decode_png_file(const char *path, unsigned *w, unsigned *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) { fclose(f); return NULL; }

    unsigned char *png_data = malloc(sz);
    if (!png_data) { fclose(f); return NULL; }
    fread(png_data, 1, sz, f);
    fclose(f);

    unsigned char *rgb = NULL;
    unsigned error = lodepng_decode24(&rgb, w, h, png_data, sz);
    free(png_data);

    if (error) {
        ESP_LOGE(TAG, "lodepng error %u: %s", error, lodepng_error_text(error));
        return NULL;
    }

    return rgb;
}

/* ---- public API ---- */

esp_err_t pd_content_init(const char *base_path)
{
    snprintf(content_base, sizeof(content_base), "%s/content", base_path);
    ensure_dir(content_base);

    char path[PD_CONTENT_MAX_PATH];
    snprintf(path, sizeof(path), "%s/images", content_base);
    ensure_dir(path);

    int dw = pd_display_get_width();
    int dh = pd_display_get_height();
    if (dw > 0 && dh > 0) {
        content_transition = pd_transition_create(dw, dh);
        content_fb = pd_framebuf_create(dw, dh);
        if (!content_transition || !content_fb) {
            ESP_LOGW(TAG, "failed to allocate transition engine");
        }
    }

    ESP_LOGI(TAG, "content initialized at %s", content_base);
    return ESP_OK;
}

int pd_content_list_images(pd_content_entry_t *entries, int max_entries)
{
    char img_dir[PD_CONTENT_MAX_PATH];
    snprintf(img_dir, sizeof(img_dir), "%s/images", content_base);

    DIR *d = opendir(img_dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", img_dir);
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        if (ent->d_name[0] == '.') continue;

        char full[PD_CONTENT_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", img_dir, ent->d_name);

        pd_content_entry_t *e = &entries[count];
        snprintf(e->path, sizeof(e->path), "images/%s", ent->d_name);
        strlcpy(e->name, ent->d_name, sizeof(e->name));

        if (path_is_dir(full)) {
            e->is_sequence = true;
            e->fps = 12;
            e->frame_count = 0;
            load_sequence_meta(full, &e->fps, NULL, &e->frame_count);
        } else if (is_png(ent->d_name)) {
            e->is_sequence = false;
            e->fps = 0;
            e->frame_count = 1;
        } else {
            continue;
        }
        count++;
    }
    closedir(d);
    return count;
}

/* helper: decode first frame of content into a framebuffer */
static bool content_decode_first_frame(const char *full_path, pd_framebuf_t *fb)
{
    if (path_is_dir(full_path)) {
        char frame_path[PD_CONTENT_MAX_PATH];
        snprintf(frame_path, sizeof(frame_path), "%s/%04d.png", full_path, 1);
        unsigned w, h;
        uint8_t *rgb = decode_png_file(frame_path, &w, &h);
        if (!rgb) return false;
        pd_framebuf_blit_rgb(fb, rgb, (int)w, (int)h);
        free(rgb);
        return true;
    } else if (path_exists(full_path) && is_png(full_path)) {
        unsigned w, h;
        uint8_t *rgb = decode_png_file(full_path, &w, &h);
        if (!rgb) return false;
        pd_framebuf_blit_rgb(fb, rgb, (int)w, (int)h);
        free(rgb);
        return true;
    }
    return false;
}

/* helper: set up playback state for a content path */
static esp_err_t content_setup_playback(const char *path, const char *full)
{
    if (path_is_dir(full)) {
        int fps = 12;
        bool loop = true;
        int frames = 0;
        load_sequence_meta(full, &fps, &loop, &frames);

        if (frames == 0) {
            ESP_LOGW(TAG, "no frames in %s", full);
            return ESP_ERR_NOT_FOUND;
        }

        strlcpy(content_current, full, sizeof(content_current));
        content_is_seq = true;
        content_fps = fps > 0 ? fps : 12;
        content_loop = loop;
        content_total_frames = frames;
        content_frame = 1;
        content_playing = true;
        content_last_frame_us = 0;

        ESP_LOGI(TAG, "playing sequence: %s (%d frames @ %d fps)", path, frames, fps);
    } else if (path_exists(full) && is_png(full)) {
        strlcpy(content_current, full, sizeof(content_current));
        content_is_seq = false;
        content_playing = true;
        content_frame = 0;
        content_total_frames = 1;

        ESP_LOGI(TAG, "displaying static: %s", path);
    } else {
        ESP_LOGW(TAG, "content not found: %s", full);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t pd_content_play(const char *path)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    /* decode first frame into content_fb and display it */
    if (content_fb) {
        if (!content_decode_first_frame(full, content_fb)) {
            return ESP_ERR_NOT_FOUND;
        }
        pd_display_render_framebuf(content_fb->data);
    } else {
        /* fallback: no framebuffer, render directly */
        unsigned w, h;
        uint8_t *rgb = NULL;
        if (path_is_dir(full)) {
            char fp[PD_CONTENT_MAX_PATH];
            snprintf(fp, sizeof(fp), "%s/%04d.png", full, 1);
            rgb = decode_png_file(fp, &w, &h);
        } else {
            rgb = decode_png_file(full, &w, &h);
        }
        if (!rgb) return ESP_ERR_NOT_FOUND;
        pd_display_render_rgb(rgb, (int)w, (int)h);
        free(rgb);
    }

    return content_setup_playback(path, full);
}

esp_err_t pd_content_play_with_transition(const char *path, const char *transition,
                                          int duration_ms)
{
    if (!content_transition || !content_fb) {
        return pd_content_play(path);
    }

    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, path);

    pd_transition_type_t type = pd_transition_type_from_name(transition);
    if (type == PD_TRANS_NONE) {
        return pd_content_play(path);
    }

    /* capture current display as "from" */
    pd_framebuf_copy(content_transition->from, content_fb);

    /* decode new content into "to" */
    if (!content_decode_first_frame(full, content_transition->to)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* start the transition */
    pd_transition_start(content_transition, type, duration_ms);

    return content_setup_playback(path, full);
}

esp_err_t pd_content_stop(void)
{
    content_playing = false;
    content_current[0] = '\0';
    pd_display_clear();
    ESP_LOGI(TAG, "playback stopped");
    return ESP_OK;
}

pd_content_status_t pd_content_get_status(void)
{
    pd_content_status_t s = {0};
    s.playing = content_playing;
    if (content_playing) {
        strlcpy(s.current_path, content_current, sizeof(s.current_path));
        s.is_sequence = content_is_seq;
        s.current_frame = content_frame;
        s.total_frames = content_total_frames;
        s.fps = content_fps;
    }
    return s;
}

void pd_content_tick(void)
{
    /* drive active transition */
    if (content_transition && pd_transition_is_active(content_transition)) {
        bool still_going = pd_transition_tick(content_transition);
        pd_display_render_framebuf(content_transition->out->data);
        if (!still_going) {
            /* transition finished — update content_fb to final state */
            pd_framebuf_copy(content_fb, content_transition->to);
        }
        return;  /* don't advance sequence frames during transition */
    }

    if (!content_playing || !content_is_seq) return;

    int64_t now = esp_timer_get_time();
    int64_t frame_interval = 1000000 / content_fps;

    if (content_last_frame_us == 0) {
        content_last_frame_us = now;
        return;
    }

    if ((now - content_last_frame_us) < frame_interval) return;

    content_last_frame_us = now;
    int next = content_frame + 1;

    if (next > content_total_frames) {
        if (content_loop) {
            next = 1;
        } else {
            return;
        }
    }

    char frame_path[PD_CONTENT_MAX_PATH];
    snprintf(frame_path, sizeof(frame_path), "%s/%04d.png", content_current, next);

    unsigned w, h;
    uint8_t *rgb = decode_png_file(frame_path, &w, &h);
    if (rgb) {
        if (content_fb) {
            pd_framebuf_blit_rgb(content_fb, rgb, (int)w, (int)h);
            pd_display_render_framebuf(content_fb->data);
        } else {
            pd_display_render_rgb(rgb, (int)w, (int)h);
        }
        free(rgb);
        content_frame = next;
    }
}

/* ---- file storage ---- */

esp_err_t pd_content_store_file(const char *rel_path, const uint8_t *data, size_t len)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);

    /* ensure parent directories exist */
    char dir[PD_CONTENT_MAX_PATH];
    strlcpy(dir, full, sizeof(dir));
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        /* create nested dirs one level at a time */
        for (char *p = dir + strlen(content_base) + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                ensure_dir(dir);
                *p = '/';
            }
        }
        ensure_dir(dir);
    }

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    fwrite(data, 1, len, f);
    fclose(f);

    ESP_LOGI(TAG, "stored %s (%d bytes)", rel_path, (int)len);
    return ESP_OK;
}

esp_err_t pd_content_delete_file(const char *rel_path)
{
    char full[PD_CONTENT_MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", content_base, rel_path);

    if (remove(full) != 0) {
        ESP_LOGW(TAG, "cannot delete %s: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted %s", rel_path);
    return ESP_OK;
}

/* ---- HTTP handlers ---- */

static esp_err_t http_content_list(httpd_req_t *req)
{
    static pd_content_entry_t entries[PD_CONTENT_MAX_LIST];
    int count = pd_content_list_images(entries, PD_CONTENT_MAX_LIST);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "images");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path", entries[i].path);
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddBoolToObject(item, "sequence", entries[i].is_sequence);
        cJSON_AddNumberToObject(item, "frames", entries[i].frame_count);
        if (entries[i].is_sequence) {
            cJSON_AddNumberToObject(item, "fps", entries[i].fps);
        }
        cJSON_AddItemToArray(arr, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_content_play(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *path = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_FAIL;
    }

    cJSON *j_trans = cJSON_GetObjectItem(root, "transition");
    cJSON *j_dur   = cJSON_GetObjectItem(root, "duration_ms");

    esp_err_t err;
    if (cJSON_IsString(j_trans)) {
        int dur = cJSON_IsNumber(j_dur) ? j_dur->valueint : 500;
        err = pd_content_play_with_transition(path->valuestring,
                                              j_trans->valuestring, dur);
    } else {
        err = pd_content_play(path->valuestring);
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "content not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_stop(httpd_req_t *req)
{
    pd_content_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_content_status(httpd_req_t *req)
{
    pd_content_status_t s = pd_content_get_status();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", s.playing);
    if (s.playing) {
        cJSON_AddStringToObject(root, "path", s.current_path);
        cJSON_AddBoolToObject(root, "sequence", s.is_sequence);
        cJSON_AddNumberToObject(root, "frame", s.current_frame);
        cJSON_AddNumberToObject(root, "total_frames", s.total_frames);
        cJSON_AddNumberToObject(root, "fps", s.fps);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t http_content_upload(httpd_req_t *req)
{
    /* path comes from query string: ?path=images/foo.png */
    char query[256] = "";
    char rel_path[PD_CONTENT_MAX_PATH] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", rel_path, sizeof(rel_path));
    }

    if (rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ?path=");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total <= 0 || total > 512 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    uint8_t *data = malloc(total);
    if (!data) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, (char *)data + received, total - received);
        if (ret <= 0) {
            free(data);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }

    esp_err_t err = pd_content_store_file(rel_path, data, total);
    free(data);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t pd_content_register_http(httpd_handle_t server)
{
    httpd_uri_t list_uri = {
        .uri = "/api/content",
        .method = HTTP_GET,
        .handler = http_content_list
    };
    httpd_uri_t play_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = http_content_play
    };
    httpd_uri_t stop_uri = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = http_content_stop
    };
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = http_content_status
    };
    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = http_content_upload
    };

    httpd_register_uri_handler(server, &list_uri);
    httpd_register_uri_handler(server, &play_uri);
    httpd_register_uri_handler(server, &stop_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "HTTP endpoints registered");
    return ESP_OK;
}
