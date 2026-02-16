/*
 * dumpster-diver.c — EmulationStation bridge daemon for pixel-dumpster
 *
 * Watches EmulationStation log file for events and sends content
 * commands to the pixel-dumpster device via HTTP API.
 *
 * Usage: dumpster-diver [options]
 *   --config FILE    Path to config file (default: ~/.config/dumpster-diver/config.json)
 *   --log FILE       Path to ES log file (default: ~/.emulationstation/es_log.txt)
 *   --host IP        Device IP address (overrides config)
 *   --port PORT      Device port (default: 8088)
 *   --verbose        Enable verbose logging
 *   --dry-run        Parse events but don't send HTTP requests
 *
 * Config file (config.json):
 * {
 *   "device": { "host": "192.168.1.154", "port": 8088 },
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/inotify.h>
#endif

/* ---- cJSON (embedded minimal version) ---- */
#include "cJSON.h"

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
static char g_default_system_fallback[MAX_PATH] = "images/systems/default.png";
static char g_default_game_fallback[MAX_PATH] = "images/games/default.png";
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

/* ---- logging ---- */

static void log_info(const char *fmt, ...)
{
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    fprintf(stdout, "[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void log_verbose(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    fprintf(stdout, "[%02d:%02d:%02d] [V] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void log_error(const char *fmt, ...)
{
    va_list args;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    fprintf(stderr, "[%02d:%02d:%02d] ERROR: ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ---- signal handler ---- */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
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

    /* lookup tables */
    g_systems = cJSON_GetObjectItem(g_config_root, "systems");
    g_games = cJSON_GetObjectItem(g_config_root, "games");

    log_info("config loaded: device=%s:%d", g_device_host, g_device_port);
    return true;
}

/* ---- content lookup ---- */

static const char *lookup_game_art(const char *game_name, const char *system_name)
{
    /* 1. exact game name match */
    if (g_games && game_name) {
        cJSON *art = cJSON_GetObjectItem(g_games, game_name);
        if (cJSON_IsString(art)) {
            log_verbose("lookup: exact match for '%s'", game_name);
            return art->valuestring;
        }
    }

    /* 2. system game_path + game name */
    if (g_systems && system_name && game_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *game_path = cJSON_GetObjectItem(sys, "game_path");
            if (cJSON_IsString(game_path)) {
                /* TODO: check if file exists on device */
                static char path_buf[MAX_PATH];
                snprintf(path_buf, sizeof(path_buf), "%s%s.png",
                         game_path->valuestring, game_name);
                log_verbose("lookup: trying system path '%s'", path_buf);
                return path_buf;
            }
        }
    }

    /* 3. system art */
    if (g_systems && system_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *art = cJSON_GetObjectItem(sys, "art");
            if (cJSON_IsString(art)) {
                log_verbose("lookup: using system art for '%s'", system_name);
                return art->valuestring;
            }
        }
    }

    /* 4. fallback */
    log_verbose("lookup: using game fallback");
    return g_default_game_fallback;
}

static const char *lookup_system_art(const char *system_name)
{
    if (g_systems && system_name) {
        cJSON *sys = cJSON_GetObjectItem(g_systems, system_name);
        if (sys) {
            cJSON *art = cJSON_GetObjectItem(sys, "art");
            if (cJSON_IsString(art)) {
                return art->valuestring;
            }
        }
    }
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
        content = lookup_game_art(event->game, g_current_system);
        log_info("game select: %s [%s] -> %s", event->game, g_current_system, content);
        break;

    case ES_EVENT_GAME_LAUNCH:
        if (!g_event_game_launch) return;
        /* extract game name from rom path */
        {
            const char *name = strrchr(event->rom_path, '/');
            name = name ? name + 1 : event->rom_path;
            /* remove extension */
            static char game_name[MAX_NAME];
            strncpy(game_name, name, MAX_NAME - 1);
            char *dot = strrchr(game_name, '.');
            if (dot) *dot = '\0';
            content = lookup_game_art(game_name, g_current_system);
            log_info("game launch: %s [%s] -> %s", game_name, g_current_system, content);
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

    if (content) {
        http_post_play(content, g_default_transition, g_default_duration_ms);
    }
}

/* ---- file watching ---- */

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

/* ---- main ---- */

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  --config FILE    Config file path\n");
    fprintf(stderr, "  --log FILE       ES log file path\n");
    fprintf(stderr, "  --host IP        Device IP address\n");
    fprintf(stderr, "  --port PORT      Device port (default: 8088)\n");
    fprintf(stderr, "  --verbose        Enable verbose logging\n");
    fprintf(stderr, "  --dry-run        Parse events without sending HTTP\n");
    fprintf(stderr, "  --help           Show this help\n");
}

int main(int argc, char **argv)
{
    char config_path[MAX_PATH] = "";
    char log_path[MAX_PATH] = "";
    bool host_override = false;

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
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            strncpy(log_path, argv[++i], MAX_PATH - 1);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(g_device_host, argv[++i], sizeof(g_device_host) - 1);
            host_override = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_device_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* init curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* load config */
    if (!load_config(config_path)) {
        return 1;
    }

    /* command line host overrides config */
    if (host_override) {
        /* already set above */
    }

    /* setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    log_info("dumpster-diver starting");
    log_info("device: %s:%d", g_device_host, g_device_port);
    log_info("events: game_select=%d game_launch=%d game_end=%d system_select=%d",
             g_event_game_select, g_event_game_launch, g_event_game_end, g_event_system_select);

    /* check log file exists */
    if (access(log_path, R_OK) != 0) {
        log_error("log file not accessible: %s", log_path);
        log_info("create a test log file or specify --log path");
        curl_global_cleanup();
        return 1;
    }

    /* start watching */
#ifdef __APPLE__
    watch_file_kqueue(log_path);
#else
    watch_file_inotify(log_path);
#endif

    log_info("dumpster-diver stopped");
    if (g_config_root) cJSON_Delete(g_config_root);
    curl_global_cleanup();
    return 0;
}
