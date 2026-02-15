/*
 * playlist-cli.c — Interactive content browser for pixel-dumpster
 *
 * Connects to the device via WiFi and lets you browse content
 * with arrow keys. Sends play commands with selectable transitions.
 *
 * Usage: playlist-cli [--host IP] [--port PORT]
 *
 * Keys:
 *   Up/Down     Select content item
 *   Left/Right  Select transition
 *   Enter       Play selected item with selected transition
 *   +/-         Adjust transition duration (100ms steps)
 *   Space       Play with no transition (instant)
 *   s           Stop playback
 *   r           Refresh content list
 *   q           Quit
 */

#include <ctype.h>
#include <curl/curl.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 64
#define MAX_PATH_LEN 128
#define MAX_NAME_LEN 64

/* ---- content item ---- */

typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    bool is_sequence;
    int  frames;
    int  fps;
} content_item_t;

/* ---- transition list ---- */

static const char *transitions[] = {
    "none",
    "wipe-left",
    "wipe-right",
    "wipe-up",
    "wipe-down",
    "wipe-diag-tl",
    "wipe-diag-tr",
    "wipe-diag-bl",
    "wipe-diag-br",
    "slide-left",
    "slide-right",
    "slide-up",
    "slide-down",
    "roll-up",
    "roll-down",
    "split-h",
    "split-v",
    "split-diag",
    "fade",
    "block-build",
    "pixel-build",
    "zoom-in",
    "zoom-out",
    "flip-h",
    "flip-v",
};
static const int num_transitions = sizeof(transitions) / sizeof(transitions[0]);

/* ---- curl helpers ---- */

typedef struct {
    char *data;
    size_t len;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_buf_t *buf = (curl_buf_t *)userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *http_get(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_buf_t buf = { .data = NULL, .len = 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static char *http_post_json(const char *url, const char *json)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_buf_t buf = { .data = NULL, .len = 0 };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ---- minimal JSON parsing (no dependency on cJSON) ---- */

static const char *json_find_key(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    return p;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = json_find_key(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static int json_get_int(const char *json, const char *key, int def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    if (*p == '"') return def;
    return atoi(p);
}

static bool json_get_bool(const char *json, const char *key, bool def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

/* ---- content list parsing ---- */

static int parse_content_list(const char *json, content_item_t *items, int max)
{
    int count = 0;
    /* find "images" array */
    const char *arr = strstr(json, "[");
    if (!arr) return 0;

    const char *p = arr + 1;
    while (*p && count < max) {
        /* find next object */
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        /* extract into temp buffer */
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        char obj[512];
        if (obj_len >= sizeof(obj)) { p = obj_end + 1; continue; }
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        content_item_t *item = &items[count];
        memset(item, 0, sizeof(*item));
        json_get_str(obj, "path", item->path, sizeof(item->path));
        json_get_str(obj, "name", item->name, sizeof(item->name));
        item->is_sequence = json_get_bool(obj, "sequence", false);
        item->frames = json_get_int(obj, "frames", 1);
        item->fps = json_get_int(obj, "fps", 0);

        if (item->path[0]) count++;
        p = obj_end + 1;
    }
    return count;
}

/* ---- display ---- */

static void draw_ui(content_item_t *items, int count, int selected,
                    int trans_idx, int duration_ms, const char *status,
                    const char *host, int port, bool playing)
{
    clear();
    int row = 0;

    attron(A_BOLD);
    mvprintw(row++, 0, "pixel-dumpster playlist-cli");
    attroff(A_BOLD);
    mvprintw(row++, 0, "Device: %s:%d", host, port);
    row++;

    /* content list */
    attron(A_BOLD);
    mvprintw(row++, 0, "Content:");
    attroff(A_BOLD);

    int max_rows = LINES - 12;
    if (max_rows < 5) max_rows = 5;

    int start = 0;
    if (selected >= max_rows) start = selected - max_rows + 1;

    for (int i = start; i < count && (i - start) < max_rows; i++) {
        if (i == selected) attron(A_REVERSE);
        char label[80];
        if (items[i].is_sequence) {
            snprintf(label, sizeof(label), "  %-30s  [seq %d frames @ %d fps]",
                     items[i].name, items[i].frames, items[i].fps);
        } else {
            snprintf(label, sizeof(label), "  %-30s  [static]", items[i].name);
        }
        mvprintw(row++, 0, "%-*s", COLS, label);
        if (i == selected) attroff(A_REVERSE);
    }

    if (count == 0) {
        mvprintw(row++, 0, "  (no content — press 'r' to refresh)");
    }

    row = LINES - 7;

    /* transition selector */
    attron(A_BOLD);
    mvprintw(row++, 0, "Transition: ");
    attroff(A_BOLD);
    printw("< %s >  (%d ms)", transitions[trans_idx], duration_ms);

    /* controls */
    row++;
    attron(A_DIM);
    mvprintw(row++, 0, "Up/Down: select item   Left/Right: transition   +/-: duration");
    mvprintw(row++, 0, "Enter: play   Space: play instant   s: stop   r: refresh   q: quit");
    attroff(A_DIM);

    /* status */
    row++;
    if (status && status[0]) {
        mvprintw(row, 0, "%s", status);
    }

    refresh();
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    const char *host = "192.168.1.154";
    int port = 8088;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: playlist-cli [--host IP] [--port PORT]\n");
            return 0;
        }
    }

    curl_global_init(CURL_GLOBAL_ALL);

    /* fetch content list */
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/content", host, port);
    char *json = http_get(url);

    content_item_t items[MAX_ITEMS];
    int count = 0;
    if (json) {
        count = parse_content_list(json, items, MAX_ITEMS);
        free(json);
    }

    /* ncurses setup */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, FALSE);
    curs_set(0);

    int selected = 0;
    int trans_idx = 0;
    int duration_ms = 1000;
    char status[256] = "";
    bool playing = false;

    if (count == 0) {
        snprintf(status, sizeof(status), "No content found. Is device online?");
    }

    bool running = true;
    while (running) {
        draw_ui(items, count, selected, trans_idx, duration_ms, status, host, port, playing);

        int ch = getch();
        status[0] = '\0';

        switch (ch) {
        case KEY_UP:
            if (selected > 0) selected--;
            break;
        case KEY_DOWN:
            if (selected < count - 1) selected++;
            break;
        case KEY_LEFT:
            if (trans_idx > 0) trans_idx--;
            else trans_idx = num_transitions - 1;
            break;
        case KEY_RIGHT:
            if (trans_idx < num_transitions - 1) trans_idx++;
            else trans_idx = 0;
            break;
        case '+':
        case '=':
            duration_ms += 100;
            if (duration_ms > 5000) duration_ms = 5000;
            break;
        case '-':
        case '_':
            duration_ms -= 100;
            if (duration_ms < 100) duration_ms = 100;
            break;
        case '\n':
        case KEY_ENTER:
            if (count > 0) {
                char body[512];
                if (trans_idx == 0) {
                    /* "none" = no transition */
                    snprintf(body, sizeof(body),
                             "{\"path\":\"%s\"}", items[selected].path);
                } else {
                    snprintf(body, sizeof(body),
                             "{\"path\":\"%s\",\"transition\":\"%s\",\"duration_ms\":%d}",
                             items[selected].path, transitions[trans_idx], duration_ms);
                }
                char play_url[256];
                snprintf(play_url, sizeof(play_url), "http://%s:%d/api/play", host, port);

                /* show sending status */
                snprintf(status, sizeof(status), "Playing: %s (%s, %dms)...",
                         items[selected].name, transitions[trans_idx], duration_ms);
                draw_ui(items, count, selected, trans_idx, duration_ms, status, host, port, playing);

                char *resp = http_post_json(play_url, body);
                if (resp) {
                    snprintf(status, sizeof(status), "Playing: %s  [%s %dms]",
                             items[selected].name, transitions[trans_idx], duration_ms);
                    playing = true;
                    free(resp);
                } else {
                    snprintf(status, sizeof(status), "ERROR: failed to send play command");
                }
            }
            break;
        case ' ':
            /* instant play, no transition */
            if (count > 0) {
                char body[512];
                snprintf(body, sizeof(body),
                         "{\"path\":\"%s\"}", items[selected].path);
                char play_url[256];
                snprintf(play_url, sizeof(play_url), "http://%s:%d/api/play", host, port);
                char *resp = http_post_json(play_url, body);
                if (resp) {
                    snprintf(status, sizeof(status), "Playing: %s  [instant]",
                             items[selected].name);
                    playing = true;
                    free(resp);
                } else {
                    snprintf(status, sizeof(status), "ERROR: failed to send play command");
                }
            }
            break;
        case 's':
        case 'S': {
            char stop_url[256];
            snprintf(stop_url, sizeof(stop_url), "http://%s:%d/api/stop", host, port);
            char *resp = http_post_json(stop_url, "{}");
            if (resp) {
                snprintf(status, sizeof(status), "Stopped");
                playing = false;
                free(resp);
            } else {
                snprintf(status, sizeof(status), "ERROR: failed to stop");
            }
            break;
        }
        case 'r':
        case 'R': {
            snprintf(status, sizeof(status), "Refreshing...");
            draw_ui(items, count, selected, trans_idx, duration_ms, status, host, port, playing);
            char *rjson = http_get(url);
            if (rjson) {
                count = parse_content_list(rjson, items, MAX_ITEMS);
                free(rjson);
                if (selected >= count) selected = count > 0 ? count - 1 : 0;
                snprintf(status, sizeof(status), "Refreshed: %d items", count);
            } else {
                snprintf(status, sizeof(status), "ERROR: cannot reach device");
            }
            break;
        }
        case 'q':
        case 'Q':
            running = false;
            break;
        }
    }

    endwin();
    curl_global_cleanup();
    return 0;
}
