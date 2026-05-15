/*
 * content-cli — sync and control content on a Pixel Dumpster device over WiFi.
 *
 * Usage:
 *   content-cli --host <ip> [--sync] [--list] [--play <path>] [--stop]
 *
 * Interactive mode (default): menu-driven content browser.
 *
 * Build:
 *   cc -o tools/content-cli tools/content-cli.c -lncurses -lcurl
 */

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ncurses.h>
#include <curl/curl.h>

#define MAX_ENTRIES  64
#define MAX_PATH_LEN 512
#define MAX_NAME_LEN 128
static int g_port = 8088;

/* ---- data types ---- */

typedef struct {
    char path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    bool is_sequence;
    int  frame_count;
    int  fps;
    bool local_only;
} content_entry_t;

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

    curl_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static char *http_post(const char *url, const char *body)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

static bool http_upload_file(const char *url, const char *filepath)
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    FILE *f = fopen(filepath, "rb");
    if (!f) { curl_easy_cleanup(curl); return false; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    uint8_t *data = malloc(sz);
    if (!data) { fclose(f); curl_easy_cleanup(curl); return false; }
    fread(data, 1, sz, f);
    fclose(f);

    curl_buf_t buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, sz);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(data);
    free(buf.data);

    return (res == CURLE_OK);
}

/* ---- JSON helpers (minimal, no dependency) ---- */

static const char *json_find_key(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    return strstr(json, search);
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = json_find_key(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key) + 2, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
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
    p = strchr(p + strlen(key) + 2, ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static bool json_get_bool(const char *json, const char *key, bool def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    p = strchr(p + strlen(key) + 2, ':');
    if (!p) return def;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

/* ---- local content scanning ---- */

static bool is_png(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return false;
    return strcasecmp(name + len - 4, ".png") == 0;
}

static int scan_local_images(const char *content_dir, content_entry_t *entries, int max)
{
    char img_dir[MAX_PATH_LEN];
    snprintf(img_dir, sizeof(img_dir), "%s/images", content_dir);

    DIR *d = opendir(img_dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (ent->d_name[0] == '.') continue;

        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", img_dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        content_entry_t *e = &entries[count];
        snprintf(e->path, sizeof(e->path), "images/%s", ent->d_name);
        strlcpy(e->name, ent->d_name, sizeof(e->name));
        e->local_only = false;

        if (S_ISDIR(st.st_mode)) {
            e->is_sequence = true;
            e->fps = 12;
            e->frame_count = 0;
            /* count frames */
            char fp[MAX_PATH_LEN];
            for (int i = 1; i <= 9999; i++) {
                snprintf(fp, sizeof(fp), "%s/%04d.png", full, i);
                if (access(fp, F_OK) != 0) break;
                e->frame_count = i;
            }
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

/* ---- URL encoding ---- */

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_sz - 3; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            dst[j++] = c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
}

/* ---- sync ---- */

static void collect_files(const char *base, const char *rel, char files[][MAX_PATH_LEN], int *count, int max)
{
    char full[MAX_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", base, rel);

    DIR *d = opendir(full);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        if (ent->d_name[0] == '.') continue;

        char rel_path[MAX_PATH_LEN];
        if (rel[0])
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel, ent->d_name);
        else
            snprintf(rel_path, sizeof(rel_path), "%s", ent->d_name);

        char child[MAX_PATH_LEN];
        snprintf(child, sizeof(child), "%s/%s", full, ent->d_name);

        struct stat st;
        if (stat(child, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_files(base, rel_path, files, count, max);
        } else {
            strlcpy(files[*count], rel_path, MAX_PATH_LEN);
            (*count)++;
        }
    }
    closedir(d);
}

static int sync_content(const char *host, const char *content_dir)
{
    char base[MAX_PATH_LEN];
    snprintf(base, sizeof(base), "%s/images", content_dir);

    /* collect all local files under images/ */
    #define MAX_FILES 256
    static char files[MAX_FILES][MAX_PATH_LEN];
    int file_count = 0;
    collect_files(base, "", files, &file_count, MAX_FILES);

    if (file_count == 0) {
        printf("No files to sync.\n");
        return 0;
    }

    printf("Syncing %d files to %s...\n", file_count, host);

    int uploaded = 0;
    for (int i = 0; i < file_count; i++) {
        char rel_content[MAX_PATH_LEN];
        snprintf(rel_content, sizeof(rel_content), "images/%s", files[i]);

        char local_path[MAX_PATH_LEN];
        snprintf(local_path, sizeof(local_path), "%s/%s", base, files[i]);

        char url[MAX_PATH_LEN];
        char encoded[MAX_PATH_LEN];
        url_encode(rel_content, encoded, sizeof(encoded));
        snprintf(url, sizeof(url), "http://%s:%d/api/upload?path=%s", host, g_port, encoded);

        /* progress bar */
        int pct = (i + 1) * 100 / file_count;
        int bar = pct * 30 / 100;
        printf("\r[");
        for (int b = 0; b < 30; b++) printf(b < bar ? "#" : " ");
        printf("] %3d%%  %s", pct, files[i]);
        fflush(stdout);

        if (http_upload_file(url, local_path)) {
            uploaded++;
        } else {
            printf("\n  FAILED: %s\n", files[i]);
        }
    }
    printf("\n\nSynced %d/%d files.\n", uploaded, file_count);
    return uploaded;
}

/* ---- interactive mode ---- */

static void interactive_mode(const char *host, const char *content_dir)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    content_entry_t entries[MAX_ENTRIES];
    int count = scan_local_images(content_dir, entries, MAX_ENTRIES);

    int selected = 0;
    bool running = true;
    bool playing = false;
    char now_playing[MAX_NAME_LEN] = "";

    while (running) {
        clear();
        mvprintw(0, 0, "Pixel Dumpster Content  [%s:%d]", host, g_port);
        mvprintw(1, 0, "Up/Down: select  Enter: play  s: sync  x: stop  q: quit");

        if (playing) {
            attron(A_BOLD);
            mvprintw(2, 0, "Now playing: %s", now_playing);
            attroff(A_BOLD);
        }

        int start_y = 4;
        for (int i = 0; i < count; i++) {
            if (i == selected) attron(A_REVERSE);
            mvprintw(start_y + i, 2, "%s%s  %s",
                     entries[i].is_sequence ? "[seq] " : "      ",
                     entries[i].name,
                     entries[i].is_sequence ?
                         (char[64]){0} : "");
            if (entries[i].is_sequence) {
                printw("(%d frames @ %d fps)", entries[i].frame_count, entries[i].fps);
            }
            if (i == selected) attroff(A_REVERSE);
        }

        if (count == 0) {
            mvprintw(start_y, 2, "(no images found in %s/images/)", content_dir);
        }

        refresh();

        int key = getch();
        switch (key) {
            case 'q': case 'Q':
                running = false;
                break;
            case KEY_UP:
                if (selected > 0) selected--;
                break;
            case KEY_DOWN:
                if (selected < count - 1) selected++;
                break;
            case '\n': case '\r': case KEY_ENTER:
                if (count > 0) {
                    char body[MAX_PATH_LEN];
                    snprintf(body, sizeof(body), "{\"path\":\"%s\"}", entries[selected].path);
                    char url[MAX_PATH_LEN];
                    snprintf(url, sizeof(url), "http://%s:%d/api/play", host, g_port);

                    nodelay(stdscr, TRUE);
                    mvprintw(LINES - 1, 0, "Sending play command...");
                    refresh();

                    char *resp = http_post(url, body);
                    if (resp) {
                        playing = true;
                        strlcpy(now_playing, entries[selected].name, sizeof(now_playing));
                        free(resp);
                    } else {
                        mvprintw(LINES - 1, 0, "Failed to send play command!");
                        refresh();
                        nodelay(stdscr, FALSE);
                        napms(1500);
                    }
                    nodelay(stdscr, FALSE);
                }
                break;
            case 'x': case 'X': {
                char url[MAX_PATH_LEN];
                snprintf(url, sizeof(url), "http://%s:%d/api/stop", host, g_port);
                char *resp = http_post(url, "{}");
                free(resp);
                playing = false;
                now_playing[0] = '\0';
                break;
            }
            case 's': case 'S':
                endwin();
                sync_content(host, content_dir);
                printf("\nPress Enter to continue...");
                getchar();
                /* re-init ncurses */
                initscr();
                cbreak();
                noecho();
                keypad(stdscr, TRUE);
                curs_set(0);
                /* re-scan */
                count = scan_local_images(content_dir, entries, MAX_ENTRIES);
                break;
        }
    }

    endwin();
}

/* ---- main ---- */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --host <ip> [options]\n", prog);
    fprintf(stderr, "  --host <ip>       Device IP address\n");
    fprintf(stderr, "  --port <port>     Device port (default: 8088)\n");
    fprintf(stderr, "  --content <dir>   Content directory (default: ./content)\n");
    fprintf(stderr, "  --sync            Sync local content to device\n");
    fprintf(stderr, "  --list            List content on device\n");
    fprintf(stderr, "  --play <path>     Play content by path\n");
    fprintf(stderr, "  --stop            Stop playback\n");
    fprintf(stderr, "  (no options)      Interactive mode\n");
}

int main(int argc, char **argv)
{
    char host[128] = "";
    char content_dir[MAX_PATH_LEN] = "./content";
    int port_arg = 0;
    bool do_sync = false;
    bool do_list = false;
    bool do_stop = false;
    char play_path[MAX_PATH_LEN] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strlcpy(host, argv[++i], sizeof(host));
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--content") == 0 && i + 1 < argc) {
            strlcpy(content_dir, argv[++i], sizeof(content_dir));
        } else if (strcmp(argv[i], "--sync") == 0) {
            do_sync = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            do_list = true;
        } else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
            strlcpy(play_path, argv[++i], sizeof(play_path));
        } else if (strcmp(argv[i], "--stop") == 0) {
            do_stop = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (host[0] == '\0') {
        fprintf(stderr, "Error: --host is required\n");
        usage(argv[0]);
        return 1;
    }

    if (port_arg > 0) g_port = port_arg;

    curl_global_init(CURL_GLOBAL_ALL);

    bool did_command = false;

    if (do_sync) {
        sync_content(host, content_dir);
        did_command = true;
    }

    if (do_list) {
        char url[MAX_PATH_LEN];
        snprintf(url, sizeof(url), "http://%s:%d/api/content", host, g_port);
        char *resp = http_get(url);
        if (resp) {
            printf("%s\n", resp);
            free(resp);
        } else {
            fprintf(stderr, "Failed to list content\n");
        }
        did_command = true;
    }

    if (play_path[0]) {
        char body[MAX_PATH_LEN];
        snprintf(body, sizeof(body), "{\"path\":\"%s\"}", play_path);
        char url[MAX_PATH_LEN];
        snprintf(url, sizeof(url), "http://%s:%d/api/play", host, g_port);
        char *resp = http_post(url, body);
        if (resp) {
            printf("Playing: %s\n", play_path);
            free(resp);
        } else {
            fprintf(stderr, "Failed to play\n");
        }
        did_command = true;
    }

    if (do_stop) {
        char url[MAX_PATH_LEN];
        snprintf(url, sizeof(url), "http://%s:%d/api/stop", host, g_port);
        char *resp = http_post(url, "{}");
        if (resp) {
            printf("Playback stopped\n");
            free(resp);
        } else {
            fprintf(stderr, "Failed to stop\n");
        }
        did_command = true;
    }

    if (!did_command) {
        interactive_mode(host, content_dir);
    }

    curl_global_cleanup();
    return 0;
}
