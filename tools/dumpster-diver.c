/*
 * dumpster-diver.c — EmulationStation bridge daemon for pixel-dumpster
 *
 * Watches EmulationStation events via named pipe (FIFO) or log file,
 * parses gamelist.xml for marquee metadata, and sends content commands
 * to the pixel-dumpster device via HTTP API or USB serial.
 *
 * Primary mode: ES scripting hooks write events to a FIFO.
 * Fallback mode: parse es_log.txt (requires ES --debug for cursor events).
 *
 * Usage: dumpster-diver [options]
 *   --config FILE    Path to config file (default: ~/.config/dumpster-diver/config.json)
 *   --fifo PATH      FIFO path for ES events (default: /tmp/dumpster-diver.fifo)
 *   --log FILE       Fallback: ES log file (default: ~/.emulationstation/es_log.txt)
 *   --host IP        Device IP address (overrides config)
 *   --port PORT      Device port (default: 8088)
 *   --serial DEVICE  Use serial transport (e.g. /dev/ttyACM0)
 *   --baud RATE      Serial baud rate (default: 115200)
 *   --roms PATH      Path to ROMs directory (default: ~/RetroPie/roms)
 *   --gamelists PATH Path to gamelists directory
 *   --verbose        Enable verbose logging
 *   --dry-run        Parse events but don't send HTTP requests
 *
 * Config file (config.json):
 * {
 *   "device": { "host": "192.168.1.154", "port": 8088 },
 *   "transport": "wifi",
 *   "serial": { "device": "/dev/ttyACM0", "baud": 115200 },
 *   "es": {
 *     "gamelists_path": "~/.emulationstation/gamelists",
 *     "roms_path": "~/RetroPie/roms"
 *   },
 *   "fifo": "/tmp/dumpster-diver.fifo",
 *   "marquee": { "device_prefix": "marquees" },
 *   "defaults": {
 *     "system_fallback": "images/systems/default.png",
 *     "game_fallback": "images/games/default.png",
 *     "transition": "fade",
 *     "duration_ms": 800
 *   },
 *   "events": {
 *     "game_select": true,
 *     "game_launch": true,
 *     "game_end": true,
 *     "system_select": true
 *   },
 *   "systems": {
 *     "nes": { "art": "images/systems/nes.png", "game_path": "images/games/nes/" }
 *   },
 *   "games": {
 *     "Super Mario Bros": "images/games/nes/smb.png"
 *   }
 * }
 */

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/inotify.h>
#endif

/* ---- cJSON (embedded minimal version) ---- */
#include "cJSON.h"
#include "discovery.h"

/* ---- constants ---- */

#define MAX_PATH 512
#define MAX_LINE 1024
#define MAX_NAME 256
#define POLL_INTERVAL_MS 100

/* ---- global state ---- */

static volatile bool g_running = true;
static bool g_verbose = false;
static bool g_dry_run = false;

/* device config */
static char g_device_host[64] = "192.168.1.154";
static int  g_device_port = 8088;

/* defaults */
static char g_default_system_fallback[MAX_PATH] = "system/default";
static char g_default_game_fallback[MAX_PATH] = "system/default";
static char g_default_transition[64] = "fade";
static int  g_default_duration_ms = 800;

/* event enables */
static bool g_event_game_select = true;
static bool g_event_game_launch = true;
static bool g_event_game_end = true;
static bool g_event_system_select = true;

/* lookup tables */
static cJSON *g_config_root = NULL;
static cJSON *g_systems = NULL;
static cJSON *g_games = NULL;

/* transport mode */
typedef enum {
    TRANSPORT_WIFI,
    TRANSPORT_SERIAL
} transport_mode_t;

static transport_mode_t g_transport = TRANSPORT_WIFI;

/* serial config */
static char g_serial_device[MAX_PATH] = "";
static int  g_serial_baud = 115200;
static int  g_serial_fd = -1;

/* ES paths */
static char g_es_gamelists_path[MAX_PATH] = "";
static char g_es_roms_path[MAX_PATH] = "";

/* marquee settings */
static char g_marquee_device_prefix[MAX_PATH] = "marquees";

/* multi-directory marquee support */
#define MAX_MARQUEE_DIRS 8
typedef struct {
    char path[MAX_PATH];
    int priority;
    bool enabled;
} marquee_dir_t;

static marquee_dir_t g_marquee_dirs[MAX_MARQUEE_DIRS];
static int g_marquee_dir_count = 0;
static bool g_marquee_auto_upload = true;
static char g_marquee_upload_cache[MAX_PATH] = "/tmp/dumpster-diver-cache.json";
static char g_marquee_system_folder[MAX_NAME] = "system";

/* FIFO path for ES scripting events */
static char g_fifo_path[MAX_PATH] = "/tmp/dumpster-diver.fifo";

/* control API */
static int  g_api_port = 7070;
static bool g_api_enabled = true;
static int  g_api_server_fd = -1;

/* log ring buffer */
#define LOG_RING_SIZE 200
#define LOG_LINE_MAX  512
static char g_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static int  g_log_ring_head = 0;
static int  g_log_ring_count = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* last event state (protected by g_state_mutex) */
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_last_event_type[64] = "";
static char g_last_event_detail[MAX_PATH] = "";
static time_t g_start_time = 0;

/* gamelist.xml data */
typedef struct gamelist_entry {
    char system[MAX_NAME];
    char rom_name[MAX_NAME];
    char game_name[MAX_NAME];
    char marquee_path[MAX_PATH];
    char image_path[MAX_PATH];
    struct gamelist_entry *next;
} gamelist_entry_t;

static gamelist_entry_t *g_gamelist = NULL;
static int g_gamelist_count = 0;

/* test-lookup mode */
static bool g_test_lookup = false;
static char g_test_lookup_system[MAX_NAME] = "";
static int  g_test_lookup_limit = 0;

/* ---- logging ---- */

static void log_ring_append(const char *line)
{
    pthread_mutex_lock(&g_log_mutex);
    strncpy(g_log_ring[g_log_ring_head], line, LOG_LINE_MAX - 1);
    g_log_ring[g_log_ring_head][LOG_LINE_MAX - 1] = '\0';
    g_log_ring_head = (g_log_ring_head + 1) % LOG_RING_SIZE;
    if (g_log_ring_count < LOG_RING_SIZE) g_log_ring_count++;
    pthread_mutex_unlock(&g_log_mutex);
}

static void log_info(const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] ",
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, args);
    va_end(args);
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);
    log_ring_append(buf);
}

static void log_verbose(const char *fmt, ...)
{
    if (!g_verbose) return;
    char buf[LOG_LINE_MAX];
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] [V] ",
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, args);
    va_end(args);
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);
    log_ring_append(buf);
}

static void log_error(const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] ERROR: ",
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
    log_ring_append(buf);
}

/* ---- signal handler ---- */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
}

/* ---- file utilities ---- */

static bool file_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

static bool is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/* Check if path exists and is a regular file */
static bool is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

/* ---- gamelist.xml parser ---- */

/* Extract text content between <tag> and </tag>. Returns pointer past </tag> or NULL. */
static const char *xml_extract_tag(const char *xml, const char *tag, char *out, size_t out_sz)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return NULL;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end) return NULL;

    size_t len = (size_t)(end - start);
    if (len >= out_sz) len = out_sz - 1;
    strncpy(out, start, len);
    out[len] = '\0';

    /* trim whitespace */
    while (len > 0 && isspace((unsigned char)out[len - 1]))
        out[--len] = '\0';
    char *p = out;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != out) memmove(out, p, strlen(p) + 1);

    return end + strlen(close_tag);
}

/* Extract ROM name (filename without extension) from a path */
static void extract_rom_name(const char *path, char *out, size_t out_sz)
{
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    size_t len = strlen(name);
    if (len >= out_sz) len = out_sz - 1;
    strncpy(out, name, len);
    out[len] = '\0';

    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* Parse a single gamelist.xml file, append entries to g_gamelist */
static int parse_gamelist_xml(const char *filepath, const char *system_name)
{
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        fclose(f);
        return 0;
    }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int count = 0;
    const char *pos = buf;

    while ((pos = strstr(pos, "<game")) != NULL) {
        const char *game_end = strstr(pos, "</game>");
        if (!game_end) break;

        size_t block_len = (size_t)(game_end - pos) + 7; /* strlen("</game>") */
        char *block = malloc(block_len + 1);
        if (!block) { pos = game_end + 7; continue; }
        memcpy(block, pos, block_len);
        block[block_len] = '\0';

        gamelist_entry_t *entry = calloc(1, sizeof(gamelist_entry_t));
        if (entry) {
            strncpy(entry->system, system_name, MAX_NAME - 1);

            char path_buf[MAX_PATH] = "";
            xml_extract_tag(block, "path", path_buf, sizeof(path_buf));
            xml_extract_tag(block, "name", entry->game_name, sizeof(entry->game_name));
            xml_extract_tag(block, "marquee", entry->marquee_path, sizeof(entry->marquee_path));
            xml_extract_tag(block, "image", entry->image_path, sizeof(entry->image_path));

            if (path_buf[0]) {
                extract_rom_name(path_buf, entry->rom_name, sizeof(entry->rom_name));
            }

            if (entry->rom_name[0] || entry->game_name[0]) {
                entry->next = g_gamelist;
                g_gamelist = entry;
                g_gamelist_count++;
                count++;
            } else {
                free(entry);
            }
        }

        free(block);
        pos = game_end + 7;
    }

    free(buf);
    return count;
}

/* Scan ES gamelists directories and load all gamelist.xml files */
static void load_all_gamelists(void)
{
    /* Set default paths if not configured */
    if (!g_es_gamelists_path[0]) {
        const char *home = getenv("HOME");
        if (home)
            snprintf(g_es_gamelists_path, sizeof(g_es_gamelists_path),
                     "%s/.emulationstation/gamelists", home);
    }
    if (!g_es_roms_path[0]) {
        const char *home = getenv("HOME");
        if (home)
            snprintf(g_es_roms_path, sizeof(g_es_roms_path),
                     "%s/RetroPie/roms", home);
    }

    /* Free existing list */
    gamelist_entry_t *e = g_gamelist;
    while (e) {
        gamelist_entry_t *next = e->next;
        free(e);
        e = next;
    }
    g_gamelist = NULL;
    g_gamelist_count = 0;

    /* Scan ~/.emulationstation/gamelists/{system}/gamelist.xml */
    DIR *dir = opendir(g_es_gamelists_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char xml_path[MAX_PATH];
            snprintf(xml_path, sizeof(xml_path), "%s/%s/gamelist.xml",
                     g_es_gamelists_path, ent->d_name);
            if (access(xml_path, R_OK) == 0) {
                int n = parse_gamelist_xml(xml_path, ent->d_name);
                if (n > 0)
                    log_verbose("gamelist: loaded %d entries from %s", n, xml_path);
            }
        }
        closedir(dir);
    } else {
        log_verbose("gamelist: directory not found: %s", g_es_gamelists_path);
    }

    /* Also check {roms_path}/{system}/gamelist.xml */
    dir = opendir(g_es_roms_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char xml_path[MAX_PATH];
            snprintf(xml_path, sizeof(xml_path), "%s/%s/gamelist.xml",
                     g_es_roms_path, ent->d_name);
            if (access(xml_path, R_OK) != 0) continue;
            /* skip if already loaded from gamelists dir */
            bool already = false;
            for (gamelist_entry_t *ge = g_gamelist; ge; ge = ge->next) {
                if (strcmp(ge->system, ent->d_name) == 0) { already = true; break; }
            }
            if (!already) {
                int n = parse_gamelist_xml(xml_path, ent->d_name);
                if (n > 0)
                    log_verbose("gamelist: loaded %d entries from %s", n, xml_path);
            }
        }
        closedir(dir);
    }

    log_info("gamelist: %d total entries loaded", g_gamelist_count);
}

/* Find gamelist entry by display name, optionally filtered by system */
static const gamelist_entry_t *gamelist_find_by_name(const char *game_name, const char *system)
{
    for (const gamelist_entry_t *e = g_gamelist; e; e = e->next) {
        if (system && system[0] && strcmp(e->system, system) != 0) continue;
        if (strcmp(e->game_name, game_name) == 0) return e;
    }
    return NULL;
}

/* Find gamelist entry by ROM name, optionally filtered by system */
static const gamelist_entry_t *gamelist_find_by_rom(const char *rom_name, const char *system)
{
    for (const gamelist_entry_t *e = g_gamelist; e; e = e->next) {
        if (system && system[0] && strcmp(e->system, system) != 0) continue;
        if (strcmp(e->rom_name, rom_name) == 0) return e;
    }
    return NULL;
}

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

static bool http_post_play(const char *content_path, const char *transition, int duration_ms)
{
    if (g_dry_run) {
        log_info("[DRY-RUN] POST /api/play path=%s transition=%s duration=%d",
                 content_path, transition, duration_ms);
        return true;
    }

    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/api/play", g_device_host, g_device_port);

    char body[512];
    if (transition && transition[0] && strcmp(transition, "none") != 0) {
        snprintf(body, sizeof(body),
                 "{\"path\":\"%s\",\"transition\":\"%s\",\"duration_ms\":%d}",
                 content_path, transition, duration_ms);
    } else {
        snprintf(body, sizeof(body), "{\"path\":\"%s\"}", content_path);
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("curl_easy_init failed");
        return false;
    }

    curl_buf_t buf = { .data = NULL, .len = 0 };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);

    if (res != CURLE_OK) {
        log_error("HTTP POST failed: %s", curl_easy_strerror(res));
        return false;
    }

    log_verbose("POST %s -> ok", url);
    return true;
}

/* Upload local file to device storage */
static bool http_upload_file(const char *local_path, const char *device_path)
{
    if (g_dry_run) {
        log_info("[DRY-RUN] POST /api/upload local=%s device=%s", local_path, device_path);
        return true;
    }

    /* Read file into memory */
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        log_error("upload: cannot open file: %s", local_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10*1024*1024) {
        log_error("upload: invalid file size: %ld", file_size);
        fclose(f);
        return false;
    }

    char *file_data = malloc(file_size);
    if (!file_data) {
        log_error("upload: malloc failed");
        fclose(f);
        return false;
    }

    size_t read_size = fread(file_data, 1, file_size, f);
    fclose(f);

    if ((long)read_size != file_size) {
        log_error("upload: read failed");
        free(file_data);
        return false;
    }

    /* Build upload URL with path parameter */
    char url[512];
    char encoded_path[MAX_PATH * 3]; /* URL encoding can triple size */
    const char *p = device_path;
    char *o = encoded_path;
    while (*p && (o - encoded_path) < (int)sizeof(encoded_path) - 4) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '/') {
            *o++ = *p;
        } else {
            sprintf(o, "%%%02X", (unsigned char)*p);
            o += 3;
        }
        p++;
    }
    *o = '\0';

    snprintf(url, sizeof(url), "http://%s:%d/api/upload?path=%s",
             g_device_host, g_device_port, encoded_path);

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_error("upload: curl_easy_init failed");
        free(file_data);
        return false;
    }

    curl_buf_t buf = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, file_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)file_size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    free(file_data);
    free(buf.data);

    if (res != CURLE_OK) {
        log_error("upload: HTTP POST failed: %s", curl_easy_strerror(res));
        return false;
    }

    log_verbose("upload: %s -> %s (%ld bytes)", local_path, device_path, file_size);
    return true;
}

/* Upload .seq folder (all PNG frames + meta.json) to device storage */
static bool upload_seq_folder(const char *local_dir, const char *device_dir)
{
    if (g_dry_run) {
        log_info("[DRY-RUN] uploading .seq folder: %s -> %s", local_dir, device_dir);
        return true;
    }

    DIR *d = opendir(local_dir);
    if (!d) {
        log_error("upload_seq: cannot open directory: %s", local_dir);
        return false;
    }

    /* Count total files first */
    int total = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* Skip hidden files */
        size_t len = strlen(ent->d_name);
        if (len >= 4 && strcasecmp(ent->d_name + len - 4, ".png") == 0) {
            total++;
        } else if (strcmp(ent->d_name, "meta.json") == 0) {
            total++;
        }
    }
    rewinddir(d);

    if (total == 0) {
        log_error("upload_seq: no .png or meta.json files in %s", local_dir);
        closedir(d);
        return false;
    }

    /* Upload each file */
    int uploaded = 0;
    bool had_error = false;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        
        size_t len = strlen(ent->d_name);
        bool is_png = (len >= 4 && strcasecmp(ent->d_name + len - 4, ".png") == 0);
        bool is_meta = (strcmp(ent->d_name, "meta.json") == 0);
        
        if (!is_png && !is_meta) continue;

        char local_path[MAX_PATH];
        char device_path[MAX_PATH];
        snprintf(local_path, sizeof(local_path), "%s/%s", local_dir, ent->d_name);
        snprintf(device_path, sizeof(device_path), "%s/%s", device_dir, ent->d_name);

        if (!http_upload_file(local_path, device_path)) {
            log_error("upload_seq: failed to upload %s", ent->d_name);
            had_error = true;
            /* Continue uploading remaining files */
        } else {
            uploaded++;
            if (is_png && total > 10) {
                /* Only log progress for larger sequences */
                log_verbose("upload_seq: %d/%d - %s", uploaded, total, ent->d_name);
            }
        }
    }

    closedir(d);

    if (uploaded > 0) {
        log_info("uploaded .seq folder: %d/%d files", uploaded, total);
    }
    
    return !had_error;
}

/* ---- serial transport ---- */

static bool serial_open(void)
{
    if (g_serial_fd >= 0) return true;
    if (!g_serial_device[0]) return false;

    g_serial_fd = open(g_serial_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_serial_fd < 0) {
        log_error("serial: cannot open %s: %s", g_serial_device, strerror(errno));
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(g_serial_fd, &tty) != 0) {
        log_error("serial: tcgetattr failed: %s", strerror(errno));
        close(g_serial_fd);
        g_serial_fd = -1;
        return false;
    }

    speed_t baud;
    switch (g_serial_baud) {
        case 9600:   baud = B9600;   break;
        case 19200:  baud = B19200;  break;
        case 38400:  baud = B38400;  break;
        case 57600:  baud = B57600;  break;
        case 115200: baud = B115200; break;
        case 230400: baud = B230400; break;
        default:     baud = B115200; break;
    }

    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CLOCAL | CREAD;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(g_serial_fd, TCSANOW, &tty) != 0) {
        log_error("serial: tcsetattr failed: %s", strerror(errno));
        close(g_serial_fd);
        g_serial_fd = -1;
        return false;
    }

    log_info("serial: opened %s at %d baud", g_serial_device, g_serial_baud);
    return true;
}

static void serial_close(void)
{
    if (g_serial_fd >= 0) {
        close(g_serial_fd);
        g_serial_fd = -1;
    }
}

static bool serial_send_play(const char *content_path, const char *transition, int duration_ms)
{
    if (g_dry_run) {
        log_info("[DRY-RUN] SERIAL play path=%s transition=%s duration=%d",
                 content_path, transition, duration_ms);
        return true;
    }

    if (g_serial_fd < 0 && !serial_open()) {
        return false;
    }

    char cmd[512];
    if (transition && transition[0] && strcmp(transition, "none") != 0) {
        snprintf(cmd, sizeof(cmd),
                 "{\"cmd\":\"play\",\"path\":\"%s\",\"transition\":\"%s\",\"duration_ms\":%d}\n",
                 content_path, transition, duration_ms);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "{\"cmd\":\"play\",\"path\":\"%s\"}\n", content_path);
    }

    ssize_t len = (ssize_t)strlen(cmd);
    ssize_t written = write(g_serial_fd, cmd, len);
    if (written != len) {
        log_error("serial: write failed (%zd/%zd): %s", written, len, strerror(errno));
        serial_close();
        return false;
    }

    log_verbose("serial: sent %zd bytes", len);
    return true;
}

/* ---- unified transport ---- */

static bool send_play(const char *content_path, const char *transition, int duration_ms)
{
    const char *play_path = content_path;
    static char device_path_buf[MAX_PATH];
    
    /* Check if this is a local path that needs uploading */
    if (g_marquee_auto_upload && content_path[0] == '/') {
        const char *system_start = NULL;
        
        /* Try to find where the system folder starts by looking for known markers */
        for (int i = 0; i < g_marquee_dir_count; i++) {
            if (!g_marquee_dirs[i].enabled) continue;
            size_t dir_len = strlen(g_marquee_dirs[i].path);
            if (strncmp(content_path, g_marquee_dirs[i].path, dir_len) == 0) {
                /* Path starts with this marquee directory */
                system_start = content_path + dir_len;
                while (*system_start == '/') system_start++; /* Skip leading slashes */
                break;
            }
        }

        bool device_path_built = false;

        /* Also recognize scraped marquees under ROMs path:
         *   /home/pi/RetroPie/roms/<system>/<media_subdir>/<file>
         * -> device path: <prefix>/<system>/<file>  (drop the media subdir) */
        if (!system_start && g_es_roms_path[0]) {
            size_t rp_len = strlen(g_es_roms_path);
            if (strncmp(content_path, g_es_roms_path, rp_len) == 0) {
                const char *rel = content_path + rp_len;
                while (*rel == '/') rel++;
                const char *first_slash = strchr(rel, '/');
                const char *basename = strrchr(content_path, '/');
                if (basename) basename++;
                if (first_slash && basename) {
                    size_t sys_len = first_slash - rel;
                    snprintf(device_path_buf, sizeof(device_path_buf),
                             "%s/%.*s/%s",
                             g_marquee_device_prefix,
                             (int)sys_len, rel,
                             basename);
                    device_path_built = true;
                }
            }
        }

        if ((system_start && system_start[0]) || device_path_built) {
            if (!device_path_built) {
                /* Build device path: prefix/system/filename or folder */
                snprintf(device_path_buf, sizeof(device_path_buf), "%s/%s",
                         g_marquee_device_prefix, system_start);
            }

            /* Check if it's a directory (.seq folder) or single file */
            if (is_directory(content_path)) {
                /* Upload entire .seq folder */
                if (!upload_seq_folder(content_path, device_path_buf)) {
                    log_error("failed to upload .seq folder, playing anyway");
                    /* Continue anyway - device might already have it */
                }
            } else if (is_regular_file(content_path)) {
                /* Upload single file */
                if (!http_upload_file(content_path, device_path_buf)) {
                    log_error("failed to upload marquee, playing anyway");
                    /* Continue anyway - device might already have it */
                }
            }
            
            /* Use device path for playing */
            play_path = device_path_buf;
            log_verbose("mapped local -> device: %s -> %s", content_path, play_path);
        }
    }
    
    if (g_transport == TRANSPORT_SERIAL) {
        return serial_send_play(play_path, transition, duration_ms);
    }
    return http_post_play(play_path, transition, duration_ms);
}

/* ---- config loading ---- */

static bool load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_verbose("config file not found: %s (using defaults)", path);
        return true;  /* not an error, use defaults */
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        log_error("config file too large or empty");
        return false;
    }

    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    if (g_config_root) cJSON_Delete(g_config_root);
    g_config_root = cJSON_Parse(buf);
    free(buf);

    if (!g_config_root) {
        log_error("failed to parse config JSON");
        return false;
    }

    /* device */
    cJSON *device = cJSON_GetObjectItem(g_config_root, "device");
    if (device) {
        cJSON *host = cJSON_GetObjectItem(device, "host");
        cJSON *port = cJSON_GetObjectItem(device, "port");
        if (cJSON_IsString(host)) {
            strncpy(g_device_host, host->valuestring, sizeof(g_device_host) - 1);
        }
        if (cJSON_IsNumber(port)) {
            g_device_port = port->valueint;
        }
    }

    /* defaults */
    cJSON *defaults = cJSON_GetObjectItem(g_config_root, "defaults");
    if (defaults) {
        cJSON *sf = cJSON_GetObjectItem(defaults, "system_fallback");
        cJSON *gf = cJSON_GetObjectItem(defaults, "game_fallback");
        cJSON *tr = cJSON_GetObjectItem(defaults, "transition");
        cJSON *dur = cJSON_GetObjectItem(defaults, "duration_ms");
        if (cJSON_IsString(sf)) strncpy(g_default_system_fallback, sf->valuestring, MAX_PATH - 1);
        if (cJSON_IsString(gf)) strncpy(g_default_game_fallback, gf->valuestring, MAX_PATH - 1);
        if (cJSON_IsString(tr)) strncpy(g_default_transition, tr->valuestring, 63);
        if (cJSON_IsNumber(dur)) g_default_duration_ms = dur->valueint;
    }

    /* events */
    cJSON *events = cJSON_GetObjectItem(g_config_root, "events");
    if (events) {
        cJSON *gs = cJSON_GetObjectItem(events, "game_select");
        cJSON *gl = cJSON_GetObjectItem(events, "game_launch");
        cJSON *ge = cJSON_GetObjectItem(events, "game_end");
        cJSON *ss = cJSON_GetObjectItem(events, "system_select");
        if (cJSON_IsBool(gs)) g_event_game_select = cJSON_IsTrue(gs);
        if (cJSON_IsBool(gl)) g_event_game_launch = cJSON_IsTrue(gl);
        if (cJSON_IsBool(ge)) g_event_game_end = cJSON_IsTrue(ge);
        if (cJSON_IsBool(ss)) g_event_system_select = cJSON_IsTrue(ss);
    }

    /* transport */
    cJSON *transport = cJSON_GetObjectItem(g_config_root, "transport");
    if (cJSON_IsString(transport)) {
        if (strcmp(transport->valuestring, "serial") == 0)
            g_transport = TRANSPORT_SERIAL;
        else
            g_transport = TRANSPORT_WIFI;
    }

    /* serial config */
    cJSON *serial = cJSON_GetObjectItem(g_config_root, "serial");
    if (serial) {
        cJSON *dev = cJSON_GetObjectItem(serial, "device");
        cJSON *baud = cJSON_GetObjectItem(serial, "baud");
        if (cJSON_IsString(dev))
            strncpy(g_serial_device, dev->valuestring, MAX_PATH - 1);
        if (cJSON_IsNumber(baud))
            g_serial_baud = baud->valueint;
    }

    /* ES paths */
    cJSON *es = cJSON_GetObjectItem(g_config_root, "es");
    if (es) {
        cJSON *glp = cJSON_GetObjectItem(es, "gamelists_path");
        cJSON *rp = cJSON_GetObjectItem(es, "roms_path");
        if (cJSON_IsString(glp))
            strncpy(g_es_gamelists_path, glp->valuestring, MAX_PATH - 1);
        if (cJSON_IsString(rp))
            strncpy(g_es_roms_path, rp->valuestring, MAX_PATH - 1);
    }

    /* marquee settings */
    cJSON *marquee = cJSON_GetObjectItem(g_config_root, "marquee");
    if (marquee) {
        cJSON *prefix = cJSON_GetObjectItem(marquee, "device_prefix");
        if (cJSON_IsString(prefix))
            strncpy(g_marquee_device_prefix, prefix->valuestring, MAX_PATH - 1);
        
        /* Load marquee directories array */
        cJSON *dirs = cJSON_GetObjectItem(marquee, "directories");
        if (cJSON_IsArray(dirs)) {
            g_marquee_dir_count = 0;
            cJSON *dir_obj = NULL;
            cJSON_ArrayForEach(dir_obj, dirs) {
                if (g_marquee_dir_count >= MAX_MARQUEE_DIRS) break;
                
                cJSON *path = cJSON_GetObjectItem(dir_obj, "path");
                cJSON *priority = cJSON_GetObjectItem(dir_obj, "priority");
                cJSON *enabled = cJSON_GetObjectItem(dir_obj, "enabled");
                
                if (cJSON_IsString(path)) {
                    marquee_dir_t *d = &g_marquee_dirs[g_marquee_dir_count];
                    strncpy(d->path, path->valuestring, MAX_PATH - 1);
                    d->priority = cJSON_IsNumber(priority) ? priority->valueint : g_marquee_dir_count;
                    d->enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
                    g_marquee_dir_count++;
                }
            }
            log_verbose("config: loaded %d marquee directories", g_marquee_dir_count);
        }
        
        /* Auto-upload setting */
        cJSON *auto_upload = cJSON_GetObjectItem(marquee, "auto_upload");
        if (cJSON_IsBool(auto_upload))
            g_marquee_auto_upload = cJSON_IsTrue(auto_upload);
        
        /* Upload cache path */
        cJSON *cache = cJSON_GetObjectItem(marquee, "upload_cache");
        if (cJSON_IsString(cache))
            strncpy(g_marquee_upload_cache, cache->valuestring, MAX_PATH - 1);
        
        /* System folder name */
        cJSON *sys_folder = cJSON_GetObjectItem(marquee, "system_folder");
        if (cJSON_IsString(sys_folder))
            strncpy(g_marquee_system_folder, sys_folder->valuestring, MAX_NAME - 1);
    }

    /* FIFO path */
    cJSON *fifo = cJSON_GetObjectItem(g_config_root, "fifo");
    if (cJSON_IsString(fifo))
        strncpy(g_fifo_path, fifo->valuestring, MAX_PATH - 1);

    /* lookup tables */
    g_systems = cJSON_GetObjectItem(g_config_root, "systems");
    g_games = cJSON_GetObjectItem(g_config_root, "games");

    log_info("config loaded: device=%s:%d transport=%s",
             g_device_host, g_device_port,
             g_transport == TRANSPORT_SERIAL ? "serial" : "wifi");
    return true;
}

/* ---- content lookup ---- */

/* Search for marquee file in configured directories by priority.
 * Checks for: .png, .seq/, .gif, .mp4 in order.
 * Returns static buffer with path if found, NULL otherwise. */
static const char *find_local_marquee(const char *system_name, const char *filename)
{
    static char path_buf[MAX_PATH];
    
    if (!system_name || !filename || !filename[0]) return NULL;
    
    /* Sort directories by priority (bubble sort - small list) */
    marquee_dir_t sorted[MAX_MARQUEE_DIRS];
    memcpy(sorted, g_marquee_dirs, sizeof(sorted));
    for (int i = 0; i < g_marquee_dir_count - 1; i++) {
        for (int j = 0; j < g_marquee_dir_count - i - 1; j++) {
            if (sorted[j].priority > sorted[j+1].priority) {
                marquee_dir_t temp = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = temp;
            }
        }
    }
    
    /* Search each directory by priority */
    for (int i = 0; i < g_marquee_dir_count; i++) {
        if (!sorted[i].enabled) continue;
        
        /* Try .png */
        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s.png",
                 sorted[i].path, system_name, filename);
        if (is_regular_file(path_buf)) {
            log_verbose("find_local: found PNG: %s", path_buf);
            return path_buf;
        }
        
        /* Try .seq folder (animated sequence) */
        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s.seq",
                 sorted[i].path, system_name, filename);
        if (is_directory(path_buf)) {
            log_verbose("find_local: found SEQ: %s", path_buf);
            return path_buf;
        }
        
        /* Try .gif */
        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s.gif",
                 sorted[i].path, system_name, filename);
        if (is_regular_file(path_buf)) {
            log_verbose("find_local: found GIF: %s", path_buf);
            return path_buf;
        }
        
        /* Try .mp4 */
        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s.mp4",
                 sorted[i].path, system_name, filename);
        if (is_regular_file(path_buf)) {
            log_verbose("find_local: found MP4: %s", path_buf);
            return path_buf;
        }
    }
    
    return NULL;
}

static const char *lookup_game_art(const char *game_name, const char *system_name)
{
    static char path_buf[MAX_PATH];
    char rom_name[MAX_NAME] = "";

    /* Resolve gamelist entry via display name (try rom-name fallback below) */
    const gamelist_entry_t *gl = NULL;
    if (game_name) {
        gl = gamelist_find_by_name(game_name, system_name);
    }
    if (!gl && game_name) {
        /* maybe the caller passed the rom name as game_name */
        gl = gamelist_find_by_rom(game_name, system_name);
    }
    if (gl && gl->rom_name[0]) {
        strncpy(rom_name, gl->rom_name, MAX_NAME - 1);
        log_verbose("lookup: resolved '%s' -> rom '%s' via gamelist", game_name, rom_name);
    }

    /* 0. Scraped marquee from gamelist.xml — resolve path relative to
     * roms_path/<system>/ (typical scraper convention: "./wheel/<rom>.png"). */
    if (gl && gl->marquee_path[0] && g_es_roms_path[0] && system_name && system_name[0]) {
        const char *rel = gl->marquee_path;
        if (rel[0] == '.' && rel[1] == '/') rel += 2;  /* strip leading "./" */
        while (*rel == '/') rel++;                      /* strip any leading slashes */
        snprintf(path_buf, sizeof(path_buf), "%s/%s/%s",
                 g_es_roms_path, system_name, rel);
        if (is_regular_file(path_buf)) {
            log_verbose("lookup[0]: gamelist scraped marquee '%s'", path_buf);
            return path_buf;
        }
        log_verbose("lookup[0]: gamelist marquee_path '%s' not present on disk", path_buf);
    }

    /* 1. local marquee by ROM name (highest priority if found) */
    if (system_name && rom_name[0]) {
        const char *local_path = find_local_marquee(system_name, rom_name);
        if (local_path) {
            log_verbose("lookup[1]: local ROM marquee '%s'", local_path);
            return local_path;
        }
    }

    /* 2. local marquee by display name */
    if (system_name && game_name) {
        const char *local_path = find_local_marquee(system_name, game_name);
        if (local_path) {
            log_verbose("lookup[2]: local game marquee '%s'", local_path);
            return local_path;
        }
    }

    /* 3. config games exact match by display name */
    if (g_games && game_name) {
        cJSON *art = cJSON_GetObjectItem(g_games, game_name);
        if (cJSON_IsString(art)) {
            log_verbose("lookup[3]: config match '%s'", game_name);
            return art->valuestring;
        }
    }

    /* 4. config games exact match by ROM name */
    if (g_games && rom_name[0]) {
        cJSON *art = cJSON_GetObjectItem(g_games, rom_name);
        if (cJSON_IsString(art)) {
            log_verbose("lookup[4]: config match rom '%s'", rom_name);
            return art->valuestring;
        }
    }

    /* 5. (removed) blind convention path — it produced fake device paths that
     * silently failed on the device. Real hits come from steps 0-1 (gamelist
     * scraped or local library with auto-upload). Unmatched games fall through
     * to the default sentinel below. */

    /* 6. system game_path + name */
    if (g_systems && system_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *game_path = cJSON_GetObjectItem(sys, "game_path");
            if (cJSON_IsString(game_path)) {
                const char *name = rom_name[0] ? rom_name : game_name;
                if (name && name[0]) {
                    snprintf(path_buf, sizeof(path_buf), "%s%s",
                             game_path->valuestring, name);
                    log_verbose("lookup[6]: system game_path '%s'", path_buf);
                    return path_buf;
                }
            }
        }
    }

    /* 7. system art */
    if (g_systems && system_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *art = cJSON_GetObjectItem(sys, "art");
            if (cJSON_IsString(art)) {
                log_verbose("lookup[7]: system art '%s'", system_name);
                return art->valuestring;
            }
        }
    }

    /* 8. fallback */
    log_verbose("lookup[8]: fallback");
    return g_default_game_fallback;
}

static const char *lookup_system_art(const char *system_name)
{
    /* 1. local system marquee (check system folder in each directory) */
    if (system_name) {
        const char *local_path = find_local_marquee(g_marquee_system_folder, system_name);
        if (local_path) {
            log_verbose("lookup_system[1]: local system art '%s'", local_path);
            return local_path;
        }
    }
    
    /* 2. config system art */
    if (g_systems && system_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *art = cJSON_GetObjectItem(sys, "art");
            if (cJSON_IsString(art)) {
                log_verbose("lookup_system[2]: config system art '%s'", system_name);
                return art->valuestring;
            }
        }
    }
    
    /* 3. fallback */
    log_verbose("lookup_system[3]: fallback");
    return g_default_system_fallback;
}

/* ---- ES log parsing ---- */

typedef enum {
    ES_EVENT_NONE,
    ES_EVENT_GAME_SELECT,
    ES_EVENT_GAME_LAUNCH,
    ES_EVENT_GAME_END,
    ES_EVENT_SYSTEM_SELECT,
} es_event_type_t;

typedef struct {
    es_event_type_t type;
    char system[MAX_NAME];
    char game[MAX_NAME];
    char rom_path[MAX_PATH];
} es_event_t;

static bool parse_es_log_line(const char *line, es_event_t *event)
{
    memset(event, 0, sizeof(*event));

    /* ES log patterns (varies by ES version):
     * "SystemView::onCursorChanged(): cursor changed to <system>"
     * "GamelistView::onCursorChanged(): cursor changed to <game>"
     * "Running game: <rom_path>"
     * "Game ended"
     */

    /* system select */
    const char *p = strstr(line, "SystemView::onCursorChanged()");
    if (p) {
        p = strstr(line, "cursor changed to ");
        if (p) {
            p += strlen("cursor changed to ");
            /* extract system name */
            char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= MAX_NAME) len = MAX_NAME - 1;
            strncpy(event->system, p, len);
            event->system[len] = '\0';
            /* trim trailing whitespace */
            while (len > 0 && isspace((unsigned char)event->system[len-1])) {
                event->system[--len] = '\0';
            }
            event->type = ES_EVENT_SYSTEM_SELECT;
            return true;
        }
    }

    /* game select */
    p = strstr(line, "GamelistView::onCursorChanged()");
    if (p) {
        p = strstr(line, "cursor changed to ");
        if (p) {
            p += strlen("cursor changed to ");
            char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= MAX_NAME) len = MAX_NAME - 1;
            strncpy(event->game, p, len);
            event->game[len] = '\0';
            while (len > 0 && isspace((unsigned char)event->game[len-1])) {
                event->game[--len] = '\0';
            }
            event->type = ES_EVENT_GAME_SELECT;
            return true;
        }
    }

    /* game launch */
    p = strstr(line, "Running game:");
    if (p) {
        p += strlen("Running game:");
        while (*p == ' ') p++;
        char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        strncpy(event->rom_path, p, len);
        event->rom_path[len] = '\0';
        while (len > 0 && isspace((unsigned char)event->rom_path[len-1])) {
            event->rom_path[--len] = '\0';
        }
        event->type = ES_EVENT_GAME_LAUNCH;
        return true;
    }

    /* game end */
    if (strstr(line, "Game ended") || strstr(line, "game ended")) {
        event->type = ES_EVENT_GAME_END;
        return true;
    }

    return false;
}

/* ---- event handling ---- */

static char g_current_system[MAX_NAME] = "";

static void handle_event(const es_event_t *event)
{
    const char *content = NULL;

    switch (event->type) {
    case ES_EVENT_SYSTEM_SELECT:
        if (!g_event_system_select) return;
        strncpy(g_current_system, event->system, MAX_NAME - 1);
        content = lookup_system_art(event->system);
        log_info("system select: %s -> %s", event->system, content);
        break;

    case ES_EVENT_GAME_SELECT:
        if (!g_event_game_select) return;
        /* FIFO events include system — keep g_current_system in sync */
        if (event->system[0])
            strncpy(g_current_system, event->system, MAX_NAME - 1);
        /* Try ROM name FIRST if available (local marquees use ROM names) */
        if (event->rom_path[0]) {
            static char sel_rom_buf[MAX_NAME];
            extract_rom_name(event->rom_path, sel_rom_buf, sizeof(sel_rom_buf));
            content = lookup_game_art(sel_rom_buf, g_current_system);
            log_verbose("lookup: tried ROM name '%s' from path '%s'", sel_rom_buf, event->rom_path);
        } else {
            /* Fallback to display name if no ROM path */
            content = lookup_game_art(event->game, g_current_system);
        }
        log_info("game select: %s (rom=%s) [%s] -> %s", 
                 event->game, 
                 event->rom_path[0] ? strrchr(event->rom_path, '/') ? strrchr(event->rom_path, '/') + 1 : event->rom_path : "none",
                 g_current_system, content);
        break;

    case ES_EVENT_GAME_LAUNCH:
        if (!g_event_game_launch) return;
        /* update current system if provided in event */
        if (event->system[0]) {
            strncpy(g_current_system, event->system, MAX_NAME - 1);
            g_current_system[MAX_NAME - 1] = '\0';
        }
        /* extract game/ROM name from rom path */
        {
            static char rom_name_buf[MAX_NAME];
            extract_rom_name(event->rom_path, rom_name_buf, sizeof(rom_name_buf));
            /* try gamelist lookup by ROM name to get display name */
            const gamelist_entry_t *gl = gamelist_find_by_rom(rom_name_buf, g_current_system);
            const char *lookup_name = (gl && gl->game_name[0]) ? gl->game_name : rom_name_buf;
            content = lookup_game_art(lookup_name, g_current_system);
            log_info("game launch: %s (rom=%s) [%s] -> %s",
                     lookup_name, rom_name_buf, g_current_system, content);
        }
        break;

    case ES_EVENT_GAME_END:
        if (!g_event_game_end) return;
        /* return to system art or attract mode */
        content = lookup_system_art(g_current_system);
        log_info("game end -> %s", content);
        break;

    default:
        return;
    }

    /* update last-event state for control API */
    {
        const char *type_str = "unknown";
        char detail[MAX_PATH] = "";
        switch (event->type) {
        case ES_EVENT_SYSTEM_SELECT: type_str = "system-select";
            snprintf(detail, sizeof(detail), "%s", event->system); break;
        case ES_EVENT_GAME_SELECT: type_str = "game-select";
            snprintf(detail, sizeof(detail), "%s|%s", event->game, g_current_system); break;
        case ES_EVENT_GAME_LAUNCH: type_str = "game-start";
            snprintf(detail, sizeof(detail), "%s", event->rom_path); break;
        case ES_EVENT_GAME_END: type_str = "game-end"; break;
        default: break;
        }
        pthread_mutex_lock(&g_state_mutex);
        strncpy(g_last_event_type, type_str, sizeof(g_last_event_type) - 1);
        strncpy(g_last_event_detail, detail, sizeof(g_last_event_detail) - 1);
        pthread_mutex_unlock(&g_state_mutex);
    }

    if (content) {
        send_play(content, g_default_transition, g_default_duration_ms);
    }
}

/* ---- FIFO event source (primary) ---- */

static bool parse_fifo_line(const char *line, es_event_t *event)
{
    memset(event, 0, sizeof(*event));

    /* format: event_type|field1|field2|field3 */
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* trim trailing whitespace/newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
        buf[--len] = '\0';
    }
    if (len == 0) return false;

    /* split on '|' — up to 4 fields */
    char *fields[4] = {NULL, NULL, NULL, NULL};
    int nfields = 0;
    char *p = buf;
    while (nfields < 4) {
        fields[nfields++] = p;
        char *sep = strchr(p, '|');
        if (!sep) break;
        *sep = '\0';
        p = sep + 1;
    }

    const char *etype = fields[0];
    if (!etype) return false;

    if (strcmp(etype, "game-select") == 0) {
        /* fields: system | rom_path | game_name */
        event->type = ES_EVENT_GAME_SELECT;
        if (fields[1]) strncpy(event->system, fields[1], MAX_NAME - 1);
        if (fields[2]) strncpy(event->rom_path, fields[2], MAX_PATH - 1);
        if (fields[3]) strncpy(event->game, fields[3], MAX_NAME - 1);
        return true;
    } else if (strcmp(etype, "system-select") == 0) {
        event->type = ES_EVENT_SYSTEM_SELECT;
        if (fields[1]) strncpy(event->system, fields[1], MAX_NAME - 1);
        return true;
    } else if (strcmp(etype, "game-start") == 0) {
        /* fields: system | rom_path | game_name */
        event->type = ES_EVENT_GAME_LAUNCH;
        if (fields[1]) strncpy(event->system, fields[1], MAX_NAME - 1);
        if (fields[2]) strncpy(event->rom_path, fields[2], MAX_PATH - 1);
        if (fields[3]) strncpy(event->game, fields[3], MAX_NAME - 1);
        return true;
    } else if (strcmp(etype, "game-end") == 0) {
        /* fields: system | rom_path | game_name */
        event->type = ES_EVENT_GAME_END;
        if (fields[1]) strncpy(event->system, fields[1], MAX_NAME - 1);
        if (fields[2]) strncpy(event->rom_path, fields[2], MAX_PATH - 1);
        if (fields[3]) strncpy(event->game, fields[3], MAX_NAME - 1);
        return true;
    }

    return false;
}

static void watch_fifo(const char *fifo_path)
{
    /* create FIFO if it doesn't exist */
    struct stat st;
    if (stat(fifo_path, &st) != 0) {
        if (mkfifo(fifo_path, 0666) != 0) {
            log_error("cannot create FIFO: %s (%s)", fifo_path, strerror(errno));
            return;
        }
        log_info("created FIFO: %s", fifo_path);
    } else if (!S_ISFIFO(st.st_mode)) {
        log_error("%s exists but is not a FIFO", fifo_path);
        return;
    }

    /* O_RDWR prevents blocking on open (process is both reader and writer).
     * Actual writes come from ES event scripts. */
    int fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        log_error("cannot open FIFO: %s (%s)", fifo_path, strerror(errno));
        return;
    }

    log_info("listening on FIFO: %s", fifo_path);

    char line_buf[MAX_LINE];
    char partial[MAX_LINE] = "";  /* accumulates data between newlines */
    size_t partial_len = 0;

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int n = select(fd + 1, &fds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("select on FIFO failed: %s", strerror(errno));
            break;
        }
        if (n == 0) continue;  /* timeout — check g_running */

        ssize_t r = read(fd, line_buf, sizeof(line_buf) - 1);
        if (r <= 0) continue;
        line_buf[r] = '\0';

        /* process complete lines (may contain multiple) */
        char *start = line_buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';

            /* prepend any leftover partial data */
            char full_line[MAX_LINE];
            if (partial_len > 0) {
                snprintf(full_line, sizeof(full_line), "%s%s", partial, start);
                partial[0] = '\0';
                partial_len = 0;
            } else {
                strncpy(full_line, start, sizeof(full_line) - 1);
                full_line[sizeof(full_line) - 1] = '\0';
            }

            log_verbose("fifo: %s", full_line);
            es_event_t ev;
            if (parse_fifo_line(full_line, &ev)) {
                handle_event(&ev);
            }
            start = nl + 1;
        }

        /* save any remaining partial line */
        if (*start) {
            size_t left = strlen(start);
            if (partial_len + left < sizeof(partial) - 1) {
                memcpy(partial + partial_len, start, left);
                partial_len += left;
                partial[partial_len] = '\0';
            }
        }
    }

    close(fd);
    unlink(fifo_path);
}

/* ---- file watching (fallback) ---- */

#ifdef __APPLE__
/* macOS: kqueue-based file watching */
static void watch_file_kqueue(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("cannot open log file: %s", path);
        return;
    }

    /* seek to end */
    lseek(fd, 0, SEEK_END);
    off_t last_pos = lseek(fd, 0, SEEK_CUR);

    int kq = kqueue();
    if (kq < 0) {
        log_error("kqueue failed");
        close(fd);
        return;
    }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, NULL);

    if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) {
        log_error("kevent registration failed");
        close(kq);
        close(fd);
        return;
    }

    log_info("watching: %s", path);

    char line_buf[MAX_LINE];
    while (g_running) {
        struct timespec timeout = { .tv_sec = 0, .tv_nsec = POLL_INTERVAL_MS * 1000000 };
        struct kevent event;
        int n = kevent(kq, NULL, 0, &event, 1, &timeout);

        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("kevent wait failed");
            break;
        }

        if (n > 0 || true) {  /* also poll periodically */
            /* check for new content */
            off_t cur_pos = lseek(fd, 0, SEEK_END);
            if (cur_pos > last_pos) {
                lseek(fd, last_pos, SEEK_SET);
                ssize_t bytes = cur_pos - last_pos;
                while (bytes > 0) {
                    ssize_t to_read = bytes > (ssize_t)(sizeof(line_buf) - 1) 
                                      ? (ssize_t)(sizeof(line_buf) - 1) : bytes;
                    ssize_t r = read(fd, line_buf, to_read);
                    if (r <= 0) break;
                    line_buf[r] = '\0';
                    bytes -= r;

                    /* process lines */
                    char *line = line_buf;
                    char *nl;
                    while ((nl = strchr(line, '\n')) != NULL) {
                        *nl = '\0';
                        log_verbose("log: %s", line);
                        es_event_t ev;
                        if (parse_es_log_line(line, &ev)) {
                            handle_event(&ev);
                        }
                        line = nl + 1;
                    }
                }
                last_pos = cur_pos;
            } else if (cur_pos < last_pos) {
                /* file was truncated, reset */
                last_pos = cur_pos;
            }
        }
    }

    close(kq);
    close(fd);
}
#else
/* Linux: inotify-based file watching */
static void watch_file_inotify(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_error("cannot open log file: %s", path);
        return;
    }

    /* seek to end */
    lseek(fd, 0, SEEK_END);
    off_t last_pos = lseek(fd, 0, SEEK_CUR);

    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        log_error("inotify_init failed");
        close(fd);
        return;
    }

    int wd = inotify_add_watch(ifd, path, IN_MODIFY);
    if (wd < 0) {
        log_error("inotify_add_watch failed");
        close(ifd);
        close(fd);
        return;
    }

    log_info("watching: %s", path);

    char line_buf[MAX_LINE];
    char event_buf[4096];
    while (g_running) {
        /* poll for inotify events */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ifd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = POLL_INTERVAL_MS * 1000 };
        int n = select(ifd + 1, &fds, NULL, NULL, &tv);

        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("select failed");
            break;
        }

        if (n > 0) {
            /* consume inotify events */
            read(ifd, event_buf, sizeof(event_buf));
        }

        /* check for new content */
        off_t cur_pos = lseek(fd, 0, SEEK_END);
        if (cur_pos > last_pos) {
            lseek(fd, last_pos, SEEK_SET);
            ssize_t bytes = cur_pos - last_pos;
            while (bytes > 0) {
                ssize_t to_read = bytes > (ssize_t)(sizeof(line_buf) - 1)
                                  ? (ssize_t)(sizeof(line_buf) - 1) : bytes;
                ssize_t r = read(fd, line_buf, to_read);
                if (r <= 0) break;
                line_buf[r] = '\0';
                bytes -= r;

                /* process lines */
                char *line = line_buf;
                char *nl;
                while ((nl = strchr(line, '\n')) != NULL) {
                    *nl = '\0';
                    log_verbose("log: %s", line);
                    es_event_t ev;
                    if (parse_es_log_line(line, &ev)) {
                        handle_event(&ev);
                    }
                    line = nl + 1;
                }
            }
            last_pos = cur_pos;
        } else if (cur_pos < last_pos) {
            /* file was truncated, reset */
            last_pos = cur_pos;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    close(fd);
}
#endif

/* ---- control API (HTTP server) ---- */

/* send a full HTTP response */
static void api_send(int fd, int code, const char *status, const char *ctype,
                     const char *body, size_t body_len)
{
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n", code, status, ctype, body_len);
    write(fd, hdr, hlen);
    if (body && body_len > 0) write(fd, body, body_len);
}

static void api_send_json(int fd, int code, const char *status, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        api_send(fd, code, status, "application/json", json, strlen(json));
        free(json);
    }
    cJSON_Delete(root);
}

/* GET /api/status */
static void api_handle_status(int fd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", g_running);

    pthread_mutex_lock(&g_state_mutex);
    cJSON_AddStringToObject(root, "current_system", g_current_system);
    cJSON_AddStringToObject(root, "last_event", g_last_event_type);
    cJSON_AddStringToObject(root, "last_event_detail", g_last_event_detail);
    pthread_mutex_unlock(&g_state_mutex);

    cJSON_AddStringToObject(root, "transport",
                            g_transport == TRANSPORT_SERIAL ? "serial" : "wifi");
    if (g_transport == TRANSPORT_SERIAL) {
        cJSON_AddStringToObject(root, "serial_device", g_serial_device);
    } else {
        cJSON_AddStringToObject(root, "device_host", g_device_host);
        cJSON_AddNumberToObject(root, "device_port", g_device_port);
    }

    time_t now = time(NULL);
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(now - g_start_time));
    cJSON_AddBoolToObject(root, "verbose", g_verbose);
    cJSON_AddBoolToObject(root, "dry_run", g_dry_run);

    cJSON *ev = cJSON_AddObjectToObject(root, "events");
    cJSON_AddBoolToObject(ev, "game_select", g_event_game_select);
    cJSON_AddBoolToObject(ev, "game_launch", g_event_game_launch);
    cJSON_AddBoolToObject(ev, "game_end", g_event_game_end);
    cJSON_AddBoolToObject(ev, "system_select", g_event_system_select);

    api_send_json(fd, 200, "OK", root);
}

/* GET /api/config */
static void api_handle_config_get(int fd)
{
    if (g_config_root) {
        char *json = cJSON_Print(g_config_root);
        if (json) {
            api_send(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
            return;
        }
    }
    api_send(fd, 200, "OK", "application/json", "{}", 2);
}

/* POST /api/reload */
static void api_handle_reload(int fd)
{
    char config_path[MAX_PATH] = "";
    const char *home = getenv("HOME");
    if (home)
        snprintf(config_path, sizeof(config_path),
                 "%s/.config/dumpster-diver/config.json", home);

    cJSON *root = cJSON_CreateObject();
    if (load_config(config_path)) {
        load_all_gamelists();
        cJSON_AddBoolToObject(root, "ok", true);
        log_info("control-api: config reloaded");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "failed to parse config");
    }
    api_send_json(fd, 200, "OK", root);
}

/* POST /api/event — inject a test event into the FIFO */
static void api_handle_event(int fd, const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "invalid JSON");
        api_send_json(fd, 400, "Bad Request", err);
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "missing type field");
        api_send_json(fd, 400, "Bad Request", err);
        return;
    }

    /* build FIFO line from JSON fields */
    char fifo_line[MAX_LINE];
    const char *type_str = type_item->valuestring;
    cJSON *sys = cJSON_GetObjectItem(root, "system");
    cJSON *game = cJSON_GetObjectItem(root, "game");
    cJSON *rom = cJSON_GetObjectItem(root, "rom_path");

    if (strcmp(type_str, "system-select") == 0) {
        snprintf(fifo_line, sizeof(fifo_line), "system-select|%s",
                 cJSON_IsString(sys) ? sys->valuestring : "");
    } else if (strcmp(type_str, "game-select") == 0) {
        snprintf(fifo_line, sizeof(fifo_line), "game-select|%s|%s|%s",
                 cJSON_IsString(sys) ? sys->valuestring : "",
                 cJSON_IsString(rom) ? rom->valuestring : "",
                 cJSON_IsString(game) ? game->valuestring : "");
    } else if (strcmp(type_str, "game-start") == 0) {
        snprintf(fifo_line, sizeof(fifo_line), "game-start|%s|%s",
                 cJSON_IsString(rom) ? rom->valuestring : "",
                 cJSON_IsString(game) ? game->valuestring : "");
    } else if (strcmp(type_str, "game-end") == 0) {
        snprintf(fifo_line, sizeof(fifo_line), "game-end");
    } else {
        cJSON_Delete(root);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "unknown event type");
        api_send_json(fd, 400, "Bad Request", err);
        return;
    }
    cJSON_Delete(root);

    /* write to FIFO so the main loop picks it up naturally */
    int fifo_fd = open(g_fifo_path, O_WRONLY | O_NONBLOCK);
    cJSON *resp = cJSON_CreateObject();
    if (fifo_fd >= 0) {
        strcat(fifo_line, "\n");
        ssize_t w = write(fifo_fd, fifo_line, strlen(fifo_line));
        close(fifo_fd);
        if (w > 0) {
            cJSON_AddBoolToObject(resp, "ok", true);
            log_info("control-api: injected event: %s", type_str);
        } else {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "FIFO write failed");
        }
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "cannot open FIFO");
    }
    api_send_json(fd, 200, "OK", resp);
}

/* GET /api/log */
static void api_handle_log(int fd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "lines");

    pthread_mutex_lock(&g_log_mutex);
    int start;
    if (g_log_ring_count < LOG_RING_SIZE)
        start = 0;
    else
        start = g_log_ring_head;  /* oldest entry */

    for (int i = 0; i < g_log_ring_count; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        cJSON_AddItemToArray(arr, cJSON_CreateString(g_log_ring[idx]));
    }
    pthread_mutex_unlock(&g_log_mutex);

    cJSON_AddNumberToObject(root, "count", g_log_ring_count);
    api_send_json(fd, 200, "OK", root);
}

/* handle one HTTP request on a connected socket */
static void api_handle_request(int client_fd)
{
    char buf[4096] = "";
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    /* parse request line: "METHOD /path HTTP/1.x\r\n" */
    char method[16] = "", path[256] = "";
    sscanf(buf, "%15s %255s", method, path);

    /* CORS preflight */
    if (strcasecmp(method, "OPTIONS") == 0) {
        api_send(client_fd, 204, "No Content", "text/plain", "", 0);
        close(client_fd);
        return;
    }

    /* find request body (after \r\n\r\n) */
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4; else body = "";

    /* route */
    if (strcmp(path, "/api/status") == 0 && strcasecmp(method, "GET") == 0) {
        api_handle_status(client_fd);
    } else if (strcmp(path, "/api/config") == 0 && strcasecmp(method, "GET") == 0) {
        api_handle_config_get(client_fd);
    } else if (strcmp(path, "/api/reload") == 0 && strcasecmp(method, "POST") == 0) {
        api_handle_reload(client_fd);
    } else if (strcmp(path, "/api/event") == 0 && strcasecmp(method, "POST") == 0) {
        api_handle_event(client_fd, body);
    } else if (strcmp(path, "/api/log") == 0 && strcasecmp(method, "GET") == 0) {
        api_handle_log(client_fd);
    } else {
        const char *msg = "{\"error\":\"not found\"}";
        api_send(client_fd, 404, "Not Found", "application/json", msg, strlen(msg));
    }
    close(client_fd);
}

/* server accept loop — runs in a separate thread */
static void *api_server_thread(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_error("control-api: socket() failed: %s", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(g_api_port),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("control-api: bind() port %d failed: %s", g_api_port, strerror(errno));
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 4) < 0) {
        log_error("control-api: listen() failed: %s", strerror(errno));
        close(server_fd);
        return NULL;
    }

    g_api_server_fd = server_fd;
    log_info("control-api: listening on port %d", g_api_port);

    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int ready = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;  /* timeout — check g_running */

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        /* set a read timeout so we don't hang on slow/malicious clients */
        struct timeval rto = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));

        api_handle_request(client_fd);
    }

    close(server_fd);
    g_api_server_fd = -1;
    return NULL;
}

/* ---- main ---- */

typedef enum {
    INPUT_MODE_FIFO,
    INPUT_MODE_LOG
} input_mode_t;

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  --config FILE    Config file path\n");
    fprintf(stderr, "  --fifo PATH      FIFO for ES events (default: /tmp/dumpster-diver.fifo)\n");
    fprintf(stderr, "  --log FILE       Fallback: watch ES log file instead of FIFO\n");
    fprintf(stderr, "  --host IP        Device IP address\n");
    fprintf(stderr, "  --port PORT      Device port (default: 8088)\n");
    fprintf(stderr, "  --serial DEVICE  Use serial transport (e.g. /dev/ttyACM0)\n");
    fprintf(stderr, "  --baud RATE      Serial baud rate (default: 115200)\n");
    fprintf(stderr, "  --roms PATH      Path to ROMs directory\n");
    fprintf(stderr, "  --gamelists PATH Path to gamelists directory\n");
    fprintf(stderr, "  --verbose        Enable verbose logging\n");
    fprintf(stderr, "  --dry-run        Parse events without sending\n");
    fprintf(stderr, "  --api-port PORT  Control API port (default: 7070)\n");
    fprintf(stderr, "  --no-api         Disable control API server\n");
    fprintf(stderr, "  --test-lookup [SYS|LIMIT]\n");
    fprintf(stderr, "                   Run lookup against all gamelist entries and print coverage.\n");
    fprintf(stderr, "                   Optionally filter by system and/or limit N entries.\n");
    fprintf(stderr, "  --help           Show this help\n");
}

static void run_test_lookup(const char *filter_system, int limit)
{
    int total = 0, hit = 0, miss = 0;
    int per_step[8] = {0};  /* steps 0..7 */
    int miss_by_sys_cap = 16;
    char miss_systems[16][MAX_NAME];
    int miss_by_sys_count[16] = {0};
    int miss_sys_n = 0;

    printf("%-14s %-32s %-40s %-8s %s\n",
           "SYSTEM", "ROM", "GAME_NAME", "RESULT", "RESOLVED_PATH");
    printf("------------------------------------------------------------------------------------------------\n");

    for (const gamelist_entry_t *e = g_gamelist; e; e = e->next) {
        if (filter_system && filter_system[0] && strcmp(e->system, filter_system) != 0) continue;
        if (limit > 0 && total >= limit) break;
        total++;

        /* Capture which step matched by parsing the verbose log is unreliable;
         * simpler: call lookup and inspect the returned path to infer the step. */
        const char *path = lookup_game_art(e->game_name[0] ? e->game_name : e->rom_name,
                                            e->system);
        const char *result = "MISS";
        int step = -1;
        if (path) {
            if (strncmp(path, "system/", 7) == 0) {
                step = 7; result = "DFLT";   /* fell through to default sentinel */
            } else if (g_es_roms_path[0] && strstr(path, g_es_roms_path) == path) {
                step = 0; result = "OK[0]";
            } else {
                for (int i = 0; i < g_marquee_dir_count; i++) {
                    if (strncmp(path, g_marquee_dirs[i].path,
                                strlen(g_marquee_dirs[i].path)) == 0) {
                        step = 1; result = "OK[1]"; break;
                    }
                }
                if (step < 0) {
                    step = 5;  /* convention path — device path only, file not verified */
                    result = "CONV";
                }
            }
            if (step >= 0 && step < 8) per_step[step]++;
            if (step == 5 || step == 7) {
                /* convention or default sentinel — neither is a real local hit */
                miss++;
                /* track which systems miss most */
                int found_i = -1;
                for (int i = 0; i < miss_sys_n; i++) {
                    if (strcmp(miss_systems[i], e->system) == 0) { found_i = i; break; }
                }
                if (found_i < 0 && miss_sys_n < miss_by_sys_cap) {
                    strncpy(miss_systems[miss_sys_n], e->system, MAX_NAME - 1);
                    miss_by_sys_count[miss_sys_n] = 1;
                    miss_sys_n++;
                } else if (found_i >= 0) {
                    miss_by_sys_count[found_i]++;
                }
            } else {
                hit++;
            }
        } else {
            miss++;
        }

        printf("%-14s %-32s %-40.40s %-8s %s\n",
               e->system,
               e->rom_name[0] ? e->rom_name : "-",
               e->game_name[0] ? e->game_name : "-",
               result,
               path ? path : "(null)");
    }

    printf("------------------------------------------------------------------------------------------------\n");
    printf("TOTAL: %d   HITS: %d (%.1f%%)   MISSES: %d (%.1f%%)\n",
           total,
           hit,  total ? (100.0 * hit  / total) : 0.0,
           miss, total ? (100.0 * miss / total) : 0.0);
    printf("Per-step breakdown:\n");
    printf("  step[0] gamelist scraped : %d\n", per_step[0]);
    printf("  step[1] local marquee dir: %d\n", per_step[1]);
    printf("  step[5] convention (miss): %d\n", per_step[5]);
    printf("  step[7] default sentinel : %d\n", per_step[7]);
    if (miss_sys_n > 0) {
        printf("Top miss systems:\n");
        for (int i = 0; i < miss_sys_n; i++) {
            printf("  %-14s : %d misses\n", miss_systems[i], miss_by_sys_count[i]);
        }
    }
}

int main(int argc, char **argv)
{
    char config_path[MAX_PATH] = "";
    char log_path[MAX_PATH] = "";
    bool host_override = false;
    bool serial_override = false;
    bool fifo_override = false;
    bool log_override = false;
    input_mode_t input_mode = INPUT_MODE_FIFO;

    /* default paths */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(config_path, sizeof(config_path),
                 "%s/.config/dumpster-diver/config.json", home);
        snprintf(log_path, sizeof(log_path),
                 "%s/.emulationstation/es_log.txt", home);
    }

    /* parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            strncpy(config_path, argv[++i], MAX_PATH - 1);
        } else if (strcmp(argv[i], "--fifo") == 0 && i + 1 < argc) {
            strncpy(g_fifo_path, argv[++i], MAX_PATH - 1);
            fifo_override = true;
            input_mode = INPUT_MODE_FIFO;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            strncpy(log_path, argv[++i], MAX_PATH - 1);
            log_override = true;
            input_mode = INPUT_MODE_LOG;
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(g_device_host, argv[++i], sizeof(g_device_host) - 1);
            host_override = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_device_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) {
            strncpy(g_serial_device, argv[++i], MAX_PATH - 1);
            g_transport = TRANSPORT_SERIAL;
            serial_override = true;
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            g_serial_baud = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--roms") == 0 && i + 1 < argc) {
            strncpy(g_es_roms_path, argv[++i], MAX_PATH - 1);
        } else if (strcmp(argv[i], "--gamelists") == 0 && i + 1 < argc) {
            strncpy(g_es_gamelists_path, argv[++i], MAX_PATH - 1);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = true;
        } else if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_api_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-api") == 0) {
            g_api_enabled = false;
        } else if (strcmp(argv[i], "--test-lookup") == 0) {
            g_test_lookup = true;
            /* optional next arg: system name or integer limit */
            if (i + 1 < argc && argv[i+1][0] != '-') {
                const char *arg = argv[++i];
                char *endp = NULL;
                long v = strtol(arg, &endp, 10);
                if (endp && *endp == '\0' && v > 0) {
                    g_test_lookup_limit = (int)v;
                } else {
                    strncpy(g_test_lookup_system, arg, sizeof(g_test_lookup_system) - 1);
                }
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* if --log given without --fifo, use log mode; otherwise default FIFO */
    if (log_override && !fifo_override) {
        input_mode = INPUT_MODE_LOG;
    }

    /* init curl (needed for wifi transport) */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* load config */
    if (!load_config(config_path)) {
        return 1;
    }

    /* command line overrides config */
    (void)host_override;
    (void)serial_override;

    /* setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* load gamelist.xml data */
    load_all_gamelists();

    /* test-lookup mode: iterate gamelist, print coverage, exit */
    if (g_test_lookup) {
        run_test_lookup(g_test_lookup_system[0] ? g_test_lookup_system : NULL,
                        g_test_lookup_limit);
        curl_global_cleanup();
        return 0;
    }

    /* detect ES features and initialize discovery */
    char hostname[64] = "playBox";
    gethostname(hostname, sizeof(hostname));
    
    /* Run detection script */
    bool browsing_events = false;
    bool launch_events = true;  /* Always supported via runcommand */
    char es_version[32] = "unknown";
    const char *methods[3] = {"runcommand", NULL, NULL};
    int method_count = 1;
    
    FILE *detect = popen("./detect-es-features.sh 2>/dev/null", "r");
    if (detect) {
        char detect_buf[2048];
        size_t read_size = fread(detect_buf, 1, sizeof(detect_buf) - 1, detect);
        pclose(detect);
        
        if (read_size > 0) {
            detect_buf[read_size] = '\0';
            cJSON *detect_json = cJSON_Parse(detect_buf);
            if (detect_json) {
                cJSON *ver = cJSON_GetObjectItem(detect_json, "es_version");
                if (ver && ver->valuestring) {
                    strncpy(es_version, ver->valuestring, sizeof(es_version) - 1);
                }
                
                cJSON *scripting = cJSON_GetObjectItem(detect_json, "scripting_support");
                if (scripting && cJSON_IsTrue(scripting)) {
                    browsing_events = true;
                    methods[method_count++] = "scripting";
                }
                
                cJSON *log_file = cJSON_GetObjectItem(detect_json, "log_file");
                if (log_file && log_file->valuestring && strcmp(log_file->valuestring, "none") != 0) {
                    methods[method_count++] = "log";
                }
                
                cJSON_Delete(detect_json);
            }
        }
    }
    
    /* Initialize and start discovery */
    if (discovery_init(hostname, "1.0.0", es_version, browsing_events, launch_events, methods, method_count)) {
        discovery_start();
    }

    /* open serial port if serial transport */
    if (g_transport == TRANSPORT_SERIAL && g_serial_device[0]) {
        if (!serial_open()) {
            log_error("serial: failed to open %s (will retry on first send)", g_serial_device);
        }
    }

    g_start_time = time(NULL);

    /* start control API server thread */
    pthread_t api_thread;
    bool api_thread_started = false;
    if (g_api_enabled) {
        if (pthread_create(&api_thread, NULL, api_server_thread, NULL) == 0) {
            pthread_detach(api_thread);
            api_thread_started = true;
        } else {
            log_error("failed to start control-api thread");
        }
    }
    (void)api_thread_started;

    log_info("dumpster-diver starting");
    log_info("input: %s", input_mode == INPUT_MODE_FIFO ? "fifo" : "log");
    log_info("transport: %s", g_transport == TRANSPORT_SERIAL ? "serial" : "wifi");
    if (g_transport == TRANSPORT_SERIAL) {
        log_info("serial: %s @ %d baud", g_serial_device, g_serial_baud);
    } else {
        log_info("device: %s:%d", g_device_host, g_device_port);
    }
    log_info("events: game_select=%d game_launch=%d game_end=%d system_select=%d",
             g_event_game_select, g_event_game_launch, g_event_game_end, g_event_system_select);

    if (input_mode == INPUT_MODE_FIFO) {
        /* primary: read structured events from FIFO */
        watch_fifo(g_fifo_path);
    } else {
        /* fallback: parse ES log file */
        if (access(log_path, R_OK) != 0) {
            log_error("log file not accessible: %s", log_path);
            log_info("create a test log file or specify --log path");
            curl_global_cleanup();
            return 1;
        }
#ifdef __APPLE__
        watch_file_kqueue(log_path);
#else
        watch_file_inotify(log_path);
#endif
    }

    log_info("dumpster-diver stopped");
    discovery_stop();
    serial_close();
    /* free gamelist */
    {
        gamelist_entry_t *e = g_gamelist;
        while (e) {
            gamelist_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }
    if (g_config_root) cJSON_Delete(g_config_root);
    curl_global_cleanup();
    return 0;
}
