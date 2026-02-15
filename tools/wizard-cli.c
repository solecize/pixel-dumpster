/*
 * wizard-cli.c — Setup wizard CLI for pixel-dumpster
 *
 * Two modes:
 *   --port /dev/cu.usbmodem101   Serial client: talks to device via JSON protocol
 *   (no --port)                  Standalone: runs wizard locally with ncurses
 *
 * Common flags:
 *   --log <path>                 Log file (default: ./logs/wizard-input.log)
 *   --script <keys>             Scripted input for testing (standalone only)
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_OPTIONS 64
#define MAX_VALUE_LEN 128
#define SERIAL_LINE_BUF 4096

/* ================================================================
 * Logging
 * ================================================================ */

static FILE *g_log_file = NULL;

static void ensure_logs_dir(const char *log_path) {
    const char *slash = strrchr(log_path, '/');
    if (!slash) { mkdir("logs", 0755); return; }
    size_t dir_len = (size_t)(slash - log_path);
    if (dir_len == 0 || dir_len >= 255) return;
    char dir[256];
    memcpy(dir, log_path, dir_len);
    dir[dir_len] = '\0';
    mkdir(dir, 0755);
}

static void log_event(const char *step, const char *type, const char *value) {
    if (!g_log_file) return;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(g_log_file, "[%s] step=%s %s=%s\n", ts, step ? step : "", type, value ? value : "");
    fflush(g_log_file);
}

/* ================================================================
 * Serial port helpers
 * ================================================================ */

static int serial_open(const char *port, int baud) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", port, strerror(errno));
        return -1;
    }

    /* Clear non-blocking now that open succeeded (avoids hanging on open for CDC) */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfmakeraw(&tty);

    speed_t speed = B115200;
    if (baud == 9600) speed = B9600;
    else if (baud == 57600) speed = B57600;
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;  /* 100ms read timeout */

    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void serial_send(int fd, const char *json) {
    if (fd < 0) return;
    write(fd, json, strlen(json));
    write(fd, "\n", 1);
}

static bool serial_read_line(int fd, char *buf, size_t buf_size, int timeout_ms) {
    size_t pos = 0;
    int elapsed = 0;
    int chunk_ms = 20;
    while (elapsed < timeout_ms) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = chunk_ms * 1000 };
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) {
            elapsed += chunk_ms;
            continue;
        }

        /* read as many bytes as available */
        char tmp[256];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) { elapsed += chunk_ms; continue; }

        for (ssize_t i = 0; i < n; i++) {
            char ch = tmp[i];
            if (ch == '\n') {
                buf[pos] = '\0';
                return pos > 0;
            }
            if (ch != '\r' && pos + 1 < buf_size) {
                buf[pos++] = ch;
            }
        }
        /* got data, reset timeout to allow more data to arrive */
        elapsed = 0;
    }
    buf[pos] = '\0';
    return false;
}

/* ================================================================
 * Minimal JSON helpers (no dependency on cJSON for host tool)
 * ================================================================ */

static const char *json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        p = strstr(json, pattern);
    }
    if (!p) { out[0] = '\0'; return NULL; }
    p = strchr(p, ':');
    if (!p) { out[0] = '\0'; return NULL; }
    p++;
    while (*p == ' ') p++;
    if (*p != '"') { out[0] = '\0'; return NULL; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return out;
}

static int json_get_int(const char *json, const char *key, int def) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    p++;
    while (*p == ' ') p++;
    return atoi(p);
}

static bool json_get_bool(const char *json, const char *key, bool def) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    p++;
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return def;
}

static int json_get_string_array(const char *json, const char *key, char out[][MAX_VALUE_LEN], int max_count) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        snprintf(pattern, sizeof(pattern), "\"%s\": [", key);
        p = strstr(json, pattern);
    }
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p && (*p == ' ' || *p == ',')) p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < MAX_VALUE_LEN) {
                if (*p == '\\' && *(p+1)) p++;
                out[count][i++] = *p++;
            }
            out[count][i] = '\0';
            if (*p == '"') p++;
            count++;
        } else {
            break;
        }
    }
    return count;
}

/* ================================================================
 * Serial client mode — render device state with ncurses
 * ================================================================ */

static void serial_render_menu(const char *title, char options[][MAX_VALUE_LEN], int count, int selected,
                               int step_idx, int step_count) {
    clear();
    mvprintw(0, 0, "[%d/%d] %s", step_idx + 1, step_count, title);
    mvprintw(1, 0, "Up/Down to move, Enter to select. (</> nav, q quit)");
    for (int i = 0; i < count && i < LINES - 3; i++) {
        mvprintw(3 + i, 0, "%s %d. %s", (i == selected) ? ">" : " ", i + 1, options[i]);
    }
    refresh();
}

static void serial_render_text(const char *title, const char *value, bool mask,
                               int step_idx, int step_count) {
    clear();
    mvprintw(0, 0, "[%d/%d] %s", step_idx + 1, step_count, title);
    mvprintw(1, 0, "(</> nav, Enter to submit)");
    mvprintw(3, 0, "> %s", mask ? "********" : (value ? value : ""));
    refresh();
}

static void serial_render_status(const char *msg) {
    clear();
    mvprintw(0, 0, "%s", msg);
    refresh();
}

/* ---------- reztest UI in CLI ---------- */

#define REZTEST_COUNTDOWN_S 10

static int serial_reztest_run(int fd, const char *initial_line) {
    /* parse initial reztest_status */
    char label[MAX_VALUE_LEN] = {0};
    int idx = json_get_int(initial_line, "index", 0);
    int total = json_get_int(initial_line, "total", 1);
    int rw = json_get_int(initial_line, "width", 0);
    int rh = json_get_int(initial_line, "height", 0);
    int rs = json_get_int(initial_line, "scan_wiring", 0);
    json_get_string(initial_line, "label", label, sizeof(label));

    time_t start = time(NULL);

    while (1) {
        int elapsed = (int)(time(NULL) - start);
        int remaining = REZTEST_COUNTDOWN_S - elapsed;
        if (remaining < 0) remaining = 0;

        clear();
        mvprintw(0, 0, "REZTEST  %d/%d", idx + 1, total);
        mvprintw(2, 0, "Resolution: %s  (%dx%d scan=%d)", label, rw, rh, rs);
        mvprintw(4, 0, "Can you see the border and text on the HUB75 panel?");
        mvprintw(6, 0, "[ENTER] Keep this resolution");
        mvprintw(7, 0, "[any]   Skip to next");
        mvprintw(9, 0, "Auto-skip in %ds...", remaining);
        refresh();

        if (remaining <= 0) {
            /* auto-skip */
            serial_send(fd, "{\"cmd\":\"reztest_skip\"}");
            serial_render_status("Skipping... device rebooting");
            return 1;  /* signal: reconnect needed */
        }

        int key = getch();
        if (key == ERR) continue;

        if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            serial_send(fd, "{\"cmd\":\"reztest_keep\"}");
            /* wait for reztest_locked ack */
            char line[SERIAL_LINE_BUF];
            while (serial_read_line(fd, line, sizeof(line), 3000)) {
                char *js = strchr(line, '{');
                if (!js) continue;
                if (js != line) memmove(line, js, strlen(js) + 1);
                char type[32];
                json_get_string(line, "type", type, sizeof(type));
                if (strcmp(type, "reztest_locked") == 0) {
                    char locked_label[MAX_VALUE_LEN];
                    json_get_string(line, "label", locked_label, sizeof(locked_label));
                    int lw = json_get_int(line, "width", 0);
                    int lh = json_get_int(line, "height", 0);
                    int ls = json_get_int(line, "scan_wiring", 0);
                    clear();
                    mvprintw(0, 0, "Resolution LOCKED!");
                    mvprintw(2, 0, "%s  (%dx%d scan=%d)", locked_label, lw, lh, ls);
                    mvprintw(4, 0, "Device is rebooting to continue wizard...");
                    refresh();
                    break;
                }
            }
            return 2;  /* signal: locked, reconnect for wizard */
        } else if (key == 'q' || key == 'Q') {
            return 0;  /* quit */
        } else {
            /* any other key = skip */
            serial_send(fd, "{\"cmd\":\"reztest_skip\"}");
            serial_render_status("Skipping... device rebooting");
            return 1;  /* reconnect needed */
        }
    }
}

/* ---------- serial client: connect, handle reztest or wizard ---------- */

static int serial_connect_and_hello(int fd, char *first_response, size_t resp_size, char *resp_type, size_t type_size, bool force) {
    /* drain boot output */
    char drain[SERIAL_LINE_BUF];
    while (serial_read_line(fd, drain, sizeof(drain), 500)) { /* drain */ }

    for (int attempt = 0; attempt < 5; attempt++) {
        if (force)
            serial_send(fd, "{\"cmd\":\"hello\",\"force\":true}");
        else
            serial_send(fd, "{\"cmd\":\"hello\"}");
        char line[SERIAL_LINE_BUF];
        while (serial_read_line(fd, line, sizeof(line), 2000)) {
            char *js = strchr(line, '{');
            if (!js) continue;
            if (js != line) memmove(line, js, strlen(js) + 1);
            char type[32];
            json_get_string(line, "type", type, sizeof(type));
            if (strcmp(type, "state") == 0 || strcmp(type, "reztest_status") == 0 || strcmp(type, "complete") == 0) {
                strncpy(first_response, line, resp_size - 1);
                first_response[resp_size - 1] = '\0';
                strncpy(resp_type, type, type_size - 1);
                resp_type[type_size - 1] = '\0';
                return 0;
            }
        }
    }
    return -1;
}

static int serial_client_run(int fd, const char *port, int baud, bool force) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    serial_render_status("Connecting to device...");

    char first_resp[SERIAL_LINE_BUF] = {0};
    char first_type[32] = {0};
    if (serial_connect_and_hello(fd, first_resp, sizeof(first_resp), first_type, sizeof(first_type), force) != 0) {
        serial_render_status("Failed to connect. Press any key to exit.");
        nodelay(stdscr, FALSE);
        getch();
        endwin();
        return 1;
    }

    /* reztest loop: device reboots for each combo, CLI reconnects */
    while (strcmp(first_type, "reztest_status") == 0) {
        int result = serial_reztest_run(fd, first_resp);
        if (result == 0) {
            endwin();
            return 0;  /* user quit */
        }
        /* device is rebooting — close, wait, reopen serial */
        close(fd);
        fd = -1;
        serial_render_status("Device rebooting... reconnecting");

        for (int retry = 0; retry < 15; retry++) {
            sleep(1);
            char msg[64];
            snprintf(msg, sizeof(msg), "Reconnecting... (%ds)", retry + 1);
            serial_render_status(msg);
            fd = serial_open(port, baud);
            if (fd >= 0) break;
        }
        if (fd < 0) {
            serial_render_status("Failed to reconnect. Press any key to exit.");
            nodelay(stdscr, FALSE);
            getch();
            endwin();
            return 1;
        }

        first_resp[0] = '\0';
        first_type[0] = '\0';
        if (serial_connect_and_hello(fd, first_resp, sizeof(first_resp), first_type, sizeof(first_type), false) != 0) {
            serial_render_status("Lost connection after reboot. Press any key to exit.");
            nodelay(stdscr, FALSE);
            getch();
            close(fd);
            endwin();
            return 1;
        }
    }

    /* if device is already configured, show status and exit */
    if (strcmp(first_type, "complete") == 0) {
        serial_render_status("Device already configured. Use --force to reconfigure.");
        nodelay(stdscr, FALSE);
        getch();
        endwin();
        close(fd);
        return 0;
    }

    /* normal wizard flow from here */
    char state_mode[32] = {0};
    char state_title[MAX_VALUE_LEN] = {0};
    char state_value[MAX_VALUE_LEN] = {0};
    char state_step[64] = {0};
    char options[MAX_OPTIONS][MAX_VALUE_LEN] = {{0}};
    int option_count = 0;
    int selected = 0;
    int step_idx = 0;
    int step_count = 1;
    bool mask = false;
    bool running = true;

    char text_buf[MAX_VALUE_LEN] = {0};
    size_t text_len = 0;
    bool text_editing = false;
    char prev_step[64] = {0};

    /* parse the initial state response */
    json_get_string(first_resp, "mode", state_mode, sizeof(state_mode));
    json_get_string(first_resp, "title", state_title, sizeof(state_title));
    json_get_string(first_resp, "step", state_step, sizeof(state_step));
    json_get_string(first_resp, "value", state_value, sizeof(state_value));
    selected = json_get_int(first_resp, "selected", 0);
    step_idx = json_get_int(first_resp, "step_index", 0);
    step_count = json_get_int(first_resp, "step_count", 1);
    mask = json_get_bool(first_resp, "mask", false);
    option_count = json_get_string_array(first_resp, "options", options, MAX_OPTIONS);
    if (strcmp(state_mode, "text") == 0) {
        if (!mask) { strncpy(text_buf, state_value, sizeof(text_buf)-1); text_len = strlen(text_buf); }
        else { text_buf[0] = '\0'; text_len = 0; }
        text_editing = true;
    } else { text_editing = false; }

    bool await_state = false;  /* true after sending a command — keep reading until state arrives */

    while (running) {
        /* read all available JSON lines from device.
         * After sending a command, use a long timeout and keep reading
         * until we get a 'state' (or 'complete'/'error') message, so
         * blocking operations like wifi scan/test don't desync us. */
        int read_timeout = await_state ? 20000 : 150;
        char line[SERIAL_LINE_BUF];
        while (serial_read_line(fd, line, sizeof(line), read_timeout)) {
            /* find first '{' — ESP log output may prefix the JSON */
            char *json_start = strchr(line, '{');
            if (!json_start) continue;
            if (json_start != line) memmove(line, json_start, strlen(json_start) + 1);

            char type[32];
            json_get_string(line, "type", type, sizeof(type));
            log_event(state_step, "rx", type);

            if (strcmp(type, "state") == 0) {
                json_get_string(line, "mode", state_mode, sizeof(state_mode));
                json_get_string(line, "title", state_title, sizeof(state_title));
                json_get_string(line, "step", state_step, sizeof(state_step));
                json_get_string(line, "value", state_value, sizeof(state_value));
                selected = json_get_int(line, "selected", 0);
                step_idx = json_get_int(line, "step_index", 0);
                step_count = json_get_int(line, "step_count", 1);
                mask = json_get_bool(line, "mask", false);
                option_count = json_get_string_array(line, "options", options, MAX_OPTIONS);

                if (strcmp(state_mode, "text") == 0) {
                    bool step_changed = (strcmp(state_step, prev_step) != 0);
                    if (!mask) {
                        strncpy(text_buf, state_value, sizeof(text_buf) - 1);
                        text_len = strlen(text_buf);
                    } else if (step_changed) {
                        /* only clear on step transition, not on keystroke echoes */
                        text_buf[0] = '\0';
                        text_len = 0;
                    }
                    text_editing = true;
                } else {
                    text_editing = false;
                }
                strncpy(prev_step, state_step, sizeof(prev_step) - 1);
                await_state = false;
                read_timeout = 150;  /* drain any remaining quick messages */
            } else if (strcmp(type, "wifi_scan") == 0) {
                bool scanning = json_get_bool(line, "scanning", false);
                if (scanning) {
                    serial_render_status("Scanning WiFi...");
                }
                /* keep reading — state will follow */
            } else if (strcmp(type, "wifi_test") == 0) {
                bool testing = json_get_bool(line, "testing", false);
                if (testing) {
                    char ssid[MAX_VALUE_LEN];
                    json_get_string(line, "ssid", ssid, sizeof(ssid));
                    clear();
                    mvprintw(0, 0, "Testing WiFi connection...");
                    mvprintw(2, 0, "SSID: %s", ssid);
                    mvprintw(4, 0, "Please wait (up to 15s)...");
                    refresh();
                } else {
                    bool success = json_get_bool(line, "success", false);
                    clear();
                    if (success) {
                        char ip[32];
                        json_get_string(line, "ip", ip, sizeof(ip));
                        mvprintw(0, 0, "WiFi connected!");
                        mvprintw(2, 0, "IP: %s", ip);
                    } else {
                        mvprintw(0, 0, "WiFi connection FAILED!");
                        mvprintw(2, 0, "Check SSID and password.");
                        mvprintw(3, 0, "Returning to WiFi setup...");
                    }
                    refresh();
                }
                /* keep reading — state will follow */
            } else if (strcmp(type, "reztest_starting") == 0) {
                /* device is rebooting into reztest — enter reconnect loop */
                serial_render_status("Reztest starting... device rebooting");
                close(fd);
                fd = -1;

                for (int retry = 0; retry < 15; retry++) {
                    sleep(1);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Reconnecting for reztest... (%ds)", retry + 1);
                    serial_render_status(msg);
                    fd = serial_open(port, baud);
                    if (fd >= 0) break;
                }
                if (fd < 0) {
                    serial_render_status("Failed to reconnect. Press any key to exit.");
                    nodelay(stdscr, FALSE);
                    getch();
                    endwin();
                    return 1;
                }

                /* now run the reztest reconnect loop */
                first_resp[0] = '\0';
                first_type[0] = '\0';
                if (serial_connect_and_hello(fd, first_resp, sizeof(first_resp), first_type, sizeof(first_type), false) != 0) {
                    serial_render_status("Lost connection. Press any key to exit.");
                    nodelay(stdscr, FALSE);
                    getch();
                    close(fd);
                    endwin();
                    return 1;
                }

                while (strcmp(first_type, "reztest_status") == 0) {
                    int result = serial_reztest_run(fd, first_resp);
                    if (result == 0) {
                        endwin();
                        return 0;
                    }
                    close(fd);
                    fd = -1;
                    serial_render_status("Device rebooting... reconnecting");
                    for (int retry = 0; retry < 15; retry++) {
                        sleep(1);
                        char rmsg[64];
                        snprintf(rmsg, sizeof(rmsg), "Reconnecting... (%ds)", retry + 1);
                        serial_render_status(rmsg);
                        fd = serial_open(port, baud);
                        if (fd >= 0) break;
                    }
                    if (fd < 0) {
                        serial_render_status("Failed to reconnect. Press any key to exit.");
                        nodelay(stdscr, FALSE);
                        getch();
                        endwin();
                        return 1;
                    }
                    first_resp[0] = '\0';
                    first_type[0] = '\0';
                    if (serial_connect_and_hello(fd, first_resp, sizeof(first_resp), first_type, sizeof(first_type), false) != 0) {
                        serial_render_status("Lost connection. Press any key to exit.");
                        nodelay(stdscr, FALSE);
                        getch();
                        close(fd);
                        endwin();
                        return 1;
                    }
                }

                /* reztest done — parse the wizard state from first_resp */
                json_get_string(first_resp, "mode", state_mode, sizeof(state_mode));
                json_get_string(first_resp, "title", state_title, sizeof(state_title));
                json_get_string(first_resp, "step", state_step, sizeof(state_step));
                json_get_string(first_resp, "value", state_value, sizeof(state_value));
                selected = json_get_int(first_resp, "selected", 0);
                step_idx = json_get_int(first_resp, "step_index", 0);
                step_count = json_get_int(first_resp, "step_count", 1);
                mask = json_get_bool(first_resp, "mask", false);
                option_count = json_get_string_array(first_resp, "options", options, MAX_OPTIONS);
                if (strcmp(state_mode, "text") == 0) {
                    if (!mask) { strncpy(text_buf, state_value, sizeof(text_buf)-1); text_len = strlen(text_buf); }
                    else { text_buf[0] = '\0'; text_len = 0; }
                    text_editing = true;
                } else { text_editing = false; }
                break;  /* break inner while to re-render */
            } else if (strcmp(type, "wifi_scan") == 0) {
                bool scanning = json_get_bool(line, "scanning", false);
                if (scanning) {
                    serial_render_status("Scanning WiFi...");
                    /* block-read until scan finishes and state arrives */
                    char wline[SERIAL_LINE_BUF];
                    bool got_next_state = false;
                    while (!got_next_state && serial_read_line(fd, wline, sizeof(wline), 10000)) {
                        char *js = strchr(wline, '{');
                        if (!js) continue;
                        if (js != wline) memmove(wline, js, strlen(js) + 1);
                        char wtype[32];
                        json_get_string(wline, "type", wtype, sizeof(wtype));
                        log_event(state_step, "rx", wtype);
                        if (strcmp(wtype, "wifi_scan") == 0) {
                            /* scan result — ignore, state comes next */
                        } else if (strcmp(wtype, "state") == 0) {
                            json_get_string(wline, "mode", state_mode, sizeof(state_mode));
                            json_get_string(wline, "title", state_title, sizeof(state_title));
                            json_get_string(wline, "step", state_step, sizeof(state_step));
                            json_get_string(wline, "value", state_value, sizeof(state_value));
                            selected = json_get_int(wline, "selected", 0);
                            step_idx = json_get_int(wline, "step_index", 0);
                            step_count = json_get_int(wline, "step_count", 1);
                            mask = json_get_bool(wline, "mask", false);
                            option_count = json_get_string_array(wline, "options", options, MAX_OPTIONS);
                            text_editing = (strcmp(state_mode, "text") == 0);
                            if (text_editing) {
                                if (!mask) { strncpy(text_buf, state_value, sizeof(text_buf)-1); text_len = strlen(text_buf); }
                                else { text_buf[0] = '\0'; text_len = 0; }
                            }
                            got_next_state = true;
                        }
                    }
                    break; /* break inner while to re-render with new state */
                }
            } else if (strcmp(type, "wifi_test") == 0) {
                bool testing = json_get_bool(line, "testing", false);
                if (testing) {
                    char ssid[MAX_VALUE_LEN];
                    json_get_string(line, "ssid", ssid, sizeof(ssid));
                    clear();
                    mvprintw(0, 0, "Testing WiFi connection...");
                    mvprintw(2, 0, "SSID: %s", ssid);
                    mvprintw(4, 0, "Please wait (up to 15s)...");
                    refresh();
                    /* block-read until test result and next state arrive */
                    char wline[SERIAL_LINE_BUF];
                    bool got_next_state = false;
                    while (!got_next_state && serial_read_line(fd, wline, sizeof(wline), 20000)) {
                        char *js = strchr(wline, '{');
                        if (!js) continue;
                        if (js != wline) memmove(wline, js, strlen(js) + 1);
                        char wtype[32];
                        json_get_string(wline, "type", wtype, sizeof(wtype));
                        log_event(state_step, "rx", wtype);
                        if (strcmp(wtype, "wifi_test") == 0) {
                            bool success = json_get_bool(wline, "success", false);
                            clear();
                            if (success) {
                                char ip[32];
                                json_get_string(wline, "ip", ip, sizeof(ip));
                                mvprintw(0, 0, "WiFi connected!");
                                mvprintw(2, 0, "IP: %s", ip);
                            } else {
                                mvprintw(0, 0, "WiFi connection FAILED!");
                                mvprintw(2, 0, "Check SSID and password.");
                                mvprintw(3, 0, "Returning to WiFi setup...");
                            }
                            refresh();
                            /* keep reading for the state update */
                        } else if (strcmp(wtype, "state") == 0) {
                            json_get_string(wline, "mode", state_mode, sizeof(state_mode));
                            json_get_string(wline, "title", state_title, sizeof(state_title));
                            json_get_string(wline, "step", state_step, sizeof(state_step));
                            json_get_string(wline, "value", state_value, sizeof(state_value));
                            selected = json_get_int(wline, "selected", 0);
                            step_idx = json_get_int(wline, "step_index", 0);
                            step_count = json_get_int(wline, "step_count", 1);
                            mask = json_get_bool(wline, "mask", false);
                            option_count = json_get_string_array(wline, "options", options, MAX_OPTIONS);
                            text_editing = (strcmp(state_mode, "text") == 0);
                            if (text_editing) {
                                if (!mask) { strncpy(text_buf, state_value, sizeof(text_buf)-1); text_len = strlen(text_buf); }
                                else { text_buf[0] = '\0'; text_len = 0; }
                            }
                            got_next_state = true;
                        }
                    }
                    break; /* break inner while to re-render */
                } else {
                    /* stale wifi_test result — ignore */
                }
            } else if (strcmp(type, "complete") == 0) {
                await_state = false;
                serial_render_status("Setup complete! Device rebooting. Press any key to exit.");
                nodelay(stdscr, FALSE);
                getch();
                running = false;
                break;
            } else if (strcmp(type, "error") == 0) {
                char msg[MAX_VALUE_LEN];
                json_get_string(line, "message", msg, sizeof(msg));
                mvprintw(LINES - 1, 0, "Error: %s", msg);
                refresh();
            }
        }

        /* if await_state timed out, check if the step is the last one —
         * if so, the device likely rebooted after completing the wizard */
        if (await_state && step_idx + 1 >= step_count) {
            await_state = false;
            serial_render_status("Setup complete! Device rebooting. Press any key to exit.");
            nodelay(stdscr, FALSE);
            getch();
            running = false;
            break;
        }
        await_state = false;

        /* render current state */
        if (strcmp(state_mode, "menu") == 0) {
            serial_render_menu(state_title, options, option_count, selected, step_idx, step_count);
        } else if (text_editing) {
            clear();
            mvprintw(0, 0, "[%d/%d] %s", step_idx + 1, step_count, state_title);
            mvprintw(1, 0, "(</> nav, Enter to submit, q quit)");
            mvprintw(3, 0, "> ");
            if (mask) {
                for (size_t i = 0; i < text_len; i++) addch('*');
            } else {
                for (size_t i = 0; i < text_len; i++) addch(text_buf[i]);
            }
            refresh();
        }

        /* handle keyboard input */
        int key = getch();
        if (key == ERR) continue;

        char cmd[256];

        if (key == 'q' || key == 'Q') {
            serial_send(fd, "{\"cmd\":\"goodbye\"}");
            running = false;
            break;
        }

        if (text_editing) {
            if (key == '<' || key == KEY_LEFT) {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"<\"}");
                await_state = true;
            } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
                snprintf(cmd, sizeof(cmd), "{\"cmd\":\"input\",\"value\":\"%s\"}", text_buf);
                serial_send(fd, cmd);
                await_state = true;
            } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
                if (text_len > 0) {
                    text_buf[--text_len] = '\0';
                }
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"backspace\"}");
            } else if (key == '>' || key == KEY_RIGHT) {
                snprintf(cmd, sizeof(cmd), "{\"cmd\":\"input\",\"value\":\"%s\"}", text_buf);
                serial_send(fd, cmd);
                await_state = true;
            } else if (isprint(key)) {
                if (text_len + 1 < sizeof(text_buf)) {
                    text_buf[text_len++] = (char)key;
                    text_buf[text_len] = '\0';
                }
                char code_str[4] = {(char)key, '\0'};
                snprintf(cmd, sizeof(cmd), "{\"cmd\":\"key\",\"code\":\"%s\"}", code_str);
                serial_send(fd, cmd);
            }
        } else {
            if (key == KEY_UP || key == 'w' || key == 'W') {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"up\"}");
            } else if (key == KEY_DOWN || key == 's' || key == 'S') {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"down\"}");
            } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"enter\"}");
                await_state = true;
            } else if (key == '<' || key == KEY_LEFT) {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\"<\"}");
                await_state = true;
            } else if (key == '>' || key == KEY_RIGHT) {
                serial_send(fd, "{\"cmd\":\"key\",\"code\":\">\"}");
                await_state = true;
            } else if (key >= '1' && key <= '9') {
                char code_str[4] = {(char)key, '\0'};
                snprintf(cmd, sizeof(cmd), "{\"cmd\":\"key\",\"code\":\"%s\"}", code_str);
                serial_send(fd, cmd);
                await_state = true;
            }
        }
    }

    endwin();
    return 0;
}

/* ================================================================
 * Standalone mode (original local wizard — kept for offline testing)
 * ================================================================ */

static const char *SA_MATRIX_OPTIONS[] = {"16x16", "32x32", "64x64", "128x128", "other"};
static const int SA_MATRIX_OPTIONS_COUNT = 5;
static const char *SA_ORIENTATION_OPTIONS[] = {"0", "90", "180", "270"};
static const int SA_ORIENTATION_OPTIONS_COUNT = 4;
static const char *SA_WIFI_MANUAL_OPTION = "<manual entry>";

typedef enum { SA_NEXT = 0, SA_BACK = 1, SA_QUIT = 2 } SAAction;

typedef struct {
    char matrix_size[MAX_VALUE_LEN];
    char orientation[MAX_VALUE_LEN];
    char wifi_ssid[MAX_VALUE_LEN];
    char wifi_password[MAX_VALUE_LEN];
    char device_name[MAX_VALUE_LEN];
    char hostname[MAX_VALUE_LEN];
    char timezone[MAX_VALUE_LEN];
    char static_ip[MAX_VALUE_LEN];
    char static_gateway[MAX_VALUE_LEN];
    char static_netmask[MAX_VALUE_LEN];
} SAData;

typedef struct {
    const char *step;
    int *script_keys;
    int script_len;
    int script_index;
    bool script_mode;
} SACtx;

static int sa_next_key(SACtx *ctx) {
    if (ctx && ctx->script_mode) {
        if (ctx->script_index >= ctx->script_len) return 'q';
        return ctx->script_keys[ctx->script_index++];
    }
    return getch();
}

static void sa_draw_menu(const char *title, const char **options, int count, int sel) {
    clear();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "Up/Down or 1-%d, Enter. (</> nav)", count);
    for (int i = 0; i < count; i++)
        mvprintw(3 + i, 0, "%s %d. %s", (i == sel) ? ">" : " ", i + 1, options[i]);
    refresh();
}

static SAAction sa_menu(SACtx *ctx, const char *title, const char **options, int count, int *sel) {
    int cur = *sel;
    keypad(stdscr, TRUE);
    while (true) {
        sa_draw_menu(title, options, count, cur);
        int key = sa_next_key(ctx);
        log_event(ctx->step, "key", "");
        if (key == 'q' || key == 'Q') return SA_QUIT;
        if (key == KEY_LEFT || key == '<') return SA_BACK;
        if (key == KEY_RIGHT || key == '>') { *sel = cur; return SA_NEXT; }
        if (key == KEY_UP || key == 'w') { cur = (cur - 1 + count) % count; continue; }
        if (key == KEY_DOWN || key == 's') { cur = (cur + 1) % count; continue; }
        if (key >= '1' && key <= '9') { int i = key - '1'; if (i < count) { *sel = i; return SA_NEXT; } }
        if (key == '\n' || key == '\r' || key == KEY_ENTER) { *sel = cur; return SA_NEXT; }
    }
}

static SAAction sa_text(SACtx *ctx, const char *title, char *buf, size_t buf_len, bool mask) {
    clear();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "(</> nav, Enter submit)");
    mvprintw(3, 0, "> ");
    size_t len = strlen(buf);
    for (size_t i = 0; i < len; i++) addch(mask ? '*' : buf[i]);
    refresh();
    while (true) {
        int key = sa_next_key(ctx);
        if (key == KEY_LEFT || key == '<') return SA_BACK;
        if (key == KEY_RIGHT || key == '>' || key == '\n' || key == '\r' || key == KEY_ENTER) return SA_NEXT;
        if (key == KEY_BACKSPACE || key == 127 || key == 8) { if (len > 0) buf[--len] = '\0'; }
        else if (isprint(key) && len + 1 < buf_len) { buf[len++] = (char)key; buf[len] = '\0'; }
        mvprintw(3, 2, "%*s", 60, "");
        for (size_t i = 0; i < len; i++) mvaddch(3, 2 + (int)i, mask ? '*' : buf[i]);
        move(3, 2 + (int)len);
        refresh();
    }
}

static int sa_resolve(const char **opts, int cnt, const char *val, int fb) {
    if (!val || !val[0]) return fb;
    for (int i = 0; i < cnt; i++) if (strcmp(opts[i], val) == 0) return i;
    return fb;
}

static int sa_scan_wifi(char ssids[][MAX_VALUE_LEN], int max) {
    const char *airport = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport";
    char cmd[256];
    if (access(airport, X_OK) == 0) snprintf(cmd, sizeof(cmd), "%s -s", airport);
    else snprintf(cmd, sizeof(cmd), "nmcli -t -f SSID dev wifi");
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[256]; int count = 0; bool first = true;
    while (fgets(line, sizeof(line), fp)) {
        if (first) { first = false; if (strstr(line, "SSID")) continue; }
        char s[MAX_VALUE_LEN] = {0}; strncpy(s, line, sizeof(s)-1);
        for (int i = (int)strlen(s)-1; i >= 0; i--) { if (s[i]=='\n'||s[i]=='\r'||s[i]==' ') s[i]='\0'; else break; }
        if (!s[0]) continue;
        bool dup = false;
        for (int i = 0; i < count; i++) if (strcmp(ssids[i], s)==0) { dup=true; break; }
        if (!dup && count < max) { strncpy(ssids[count], s, MAX_VALUE_LEN-1); count++; }
    }
    pclose(fp);
    return count;
}

typedef SAAction (*SAStepFn)(SACtx*, SAData*);
typedef bool (*SAWhenFn)(const SAData*);

static SAAction sa_step_matrix(SACtx *c, SAData *d) {
    c->step = "matrix";
    int sel = sa_resolve(SA_MATRIX_OPTIONS, SA_MATRIX_OPTIONS_COUNT, d->matrix_size, 2);
    SAAction a = sa_menu(c, "Matrix size", SA_MATRIX_OPTIONS, SA_MATRIX_OPTIONS_COUNT, &sel);
    if (a != SA_NEXT) return a;
    if (strcmp(SA_MATRIX_OPTIONS[sel], "other") == 0)
        return sa_text(c, "Custom size (e.g. 128x64)", d->matrix_size, sizeof(d->matrix_size), false);
    strncpy(d->matrix_size, SA_MATRIX_OPTIONS[sel], sizeof(d->matrix_size)-1);
    return SA_NEXT;
}
static SAAction sa_step_orient(SACtx *c, SAData *d) {
    c->step = "orientation";
    int sel = sa_resolve(SA_ORIENTATION_OPTIONS, SA_ORIENTATION_OPTIONS_COUNT, d->orientation, 0);
    SAAction a = sa_menu(c, "Orientation", SA_ORIENTATION_OPTIONS, SA_ORIENTATION_OPTIONS_COUNT, &sel);
    if (a != SA_NEXT) return a;
    strncpy(d->orientation, SA_ORIENTATION_OPTIONS[sel], sizeof(d->orientation)-1);
    return SA_NEXT;
}
static SAAction sa_step_wifi(SACtx *c, SAData *d) {
    c->step = "wifi";
    char ssids[MAX_OPTIONS][MAX_VALUE_LEN] = {{0}};
    int sc = sa_scan_wifi(ssids, MAX_OPTIONS-1);
    const char *opts[MAX_OPTIONS]; int cnt = 0;
    for (int i = 0; i < sc; i++) opts[cnt++] = ssids[i];
    opts[cnt++] = SA_WIFI_MANUAL_OPTION;
    int sel = cnt - 1;
    for (int i = 0; i < cnt-1; i++) if (strcmp(d->wifi_ssid, opts[i])==0) { sel=i; break; }
    SAAction a = sa_menu(c, "WiFi SSID", opts, cnt, &sel);
    if (a != SA_NEXT) return a;
    if (strcmp(opts[sel], SA_WIFI_MANUAL_OPTION)==0) {
        a = sa_text(c, "Enter WiFi SSID", d->wifi_ssid, sizeof(d->wifi_ssid), false);
        if (a != SA_NEXT) return a;
    } else { strncpy(d->wifi_ssid, opts[sel], sizeof(d->wifi_ssid)-1); }
    return sa_text(c, "WiFi password (blank=open)", d->wifi_password, sizeof(d->wifi_password), true);
}
static SAAction sa_step_devname(SACtx *c, SAData *d) { c->step="device_name"; return sa_text(c,"Device name",d->device_name,sizeof(d->device_name),false); }
static SAAction sa_step_host(SACtx *c, SAData *d) { c->step="hostname"; return sa_text(c,"Hostname",d->hostname,sizeof(d->hostname),false); }
static SAAction sa_step_tz(SACtx *c, SAData *d) { c->step="timezone"; return sa_text(c,"Timezone (e.g. CST6CDT)",d->timezone,sizeof(d->timezone),false); }
static SAAction sa_step_ip(SACtx *c, SAData *d) {
    c->step="static_ip";
    SAAction a = sa_text(c,"Static IP (blank=DHCP)",d->static_ip,sizeof(d->static_ip),false);
    if (a==SA_NEXT && !d->static_ip[0]) { d->static_gateway[0]='\0'; d->static_netmask[0]='\0'; }
    return a;
}
static SAAction sa_step_gw(SACtx *c, SAData *d) { c->step="static_gw"; return sa_text(c,"Static gateway",d->static_gateway,sizeof(d->static_gateway),false); }
static SAAction sa_step_nm(SACtx *c, SAData *d) { c->step="static_nm"; return sa_text(c,"Static netmask",d->static_netmask,sizeof(d->static_netmask),false); }
static bool sa_when_ip(const SAData *d) { return d->static_ip[0] != '\0'; }

static int parse_key_token(const char *t) {
    if (strcasecmp(t,"UP")==0) return KEY_UP;
    if (strcasecmp(t,"DOWN")==0) return KEY_DOWN;
    if (strcasecmp(t,"LEFT")==0) return KEY_LEFT;
    if (strcasecmp(t,"RIGHT")==0) return KEY_RIGHT;
    if (strcasecmp(t,"ENTER")==0) return '\n';
    if (strcasecmp(t,"ESC")==0) return 27;
    if (strcasecmp(t,"BACKSPACE")==0) return KEY_BACKSPACE;
    if (strlen(t)==1) return t[0];
    if (strncmp(t,"0x",2)==0) return (int)strtol(t,NULL,16);
    return atoi(t);
}

static int *parse_script(const char *script, int *out_len) {
    char *copy = strdup(script);
    int cap = 32, cnt = 0;
    int *keys = malloc(sizeof(int)*cap);
    char *tok = strtok(copy, ",");
    while (tok) {
        while (*tok==' ') tok++;
        if (cnt >= cap) { cap *= 2; keys = realloc(keys, sizeof(int)*cap); }
        keys[cnt++] = parse_key_token(tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
    *out_len = cnt;
    return keys;
}

static int standalone_run(const char *script) {
    SACtx ctx = {0};
    if (script) {
        ctx.script_keys = parse_script(script, &ctx.script_len);
        ctx.script_mode = true;
    }
    SAData data = {0};
    initscr(); cbreak(); noecho();

    struct { const char *name; SAStepFn run; SAWhenFn when; } steps[] = {
        {"matrix", sa_step_matrix, NULL},
        {"orientation", sa_step_orient, NULL},
        {"wifi", sa_step_wifi, NULL},
        {"device_name", sa_step_devname, NULL},
        {"hostname", sa_step_host, NULL},
        {"timezone", sa_step_tz, NULL},
        {"static_ip", sa_step_ip, NULL},
        {"static_gw", sa_step_gw, sa_when_ip},
        {"static_nm", sa_step_nm, sa_when_ip},
    };
    int step_count = (int)(sizeof(steps)/sizeof(steps[0]));
    int idx = 0;
    log_event("", "wizard", "start");
    while (idx >= 0 && idx < step_count) {
        if (steps[idx].when && !steps[idx].when(&data)) { idx++; continue; }
        SAAction a = steps[idx].run(&ctx, &data);
        if (a == SA_QUIT) break;
        idx += (a == SA_BACK) ? -1 : 1;
    }
    log_event("", "wizard", "complete");
    if (idx >= step_count) {
        clear();
        mvprintw(0, 0, "Wizard complete!");
        mvprintw(2, 0, "Matrix: %s  Orient: %s", data.matrix_size, data.orientation);
        mvprintw(3, 0, "SSID: %s", data.wifi_ssid);
        mvprintw(4, 0, "Device: %s  Host: %s", data.device_name, data.hostname);
        mvprintw(5, 0, "TZ: %s  IP: %s", data.timezone, data.static_ip[0] ? data.static_ip : "DHCP");
        mvprintw(7, 0, "Press any key to exit.");
        refresh(); getch();
    }
    endwin();
    free(ctx.script_keys);
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char **argv) {
    const char *log_path = "./logs/wizard-input.log";
    const char *port = NULL;
    const char *script = NULL;
    int baud = 115200;
    bool force = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0 && i+1 < argc) log_path = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i+1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "--baud") == 0 && i+1 < argc) baud = atoi(argv[++i]);
        else if (strcmp(argv[i], "--script") == 0 && i+1 < argc) script = argv[++i];
        else if (strcmp(argv[i], "--force") == 0) force = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: wizard-cli [--port /dev/ttyUSB0] [--baud 115200] [--log path] [--script keys] [--force]\n");
            printf("  --port   Serial port to connect to device (serial client mode)\n");
            printf("  --baud   Baud rate (default 115200)\n");
            printf("  --log    Log file path\n");
            printf("  --script Comma-separated keys for standalone testing\n");
            printf("  --force  Re-run wizard even if device is already configured\n");
            return 0;
        }
    }

    ensure_logs_dir(log_path);
    g_log_file = fopen(log_path, "a");
    if (!g_log_file) {
        fprintf(stderr, "Failed to open log: %s\n", log_path);
        return 1;
    }

    int ret;
    if (port) {
        int fd = serial_open(port, baud);
        if (fd < 0) { fclose(g_log_file); return 1; }
        log_event("", "mode", "serial");
        log_event("", "port", port);
        ret = serial_client_run(fd, port, baud, force);
        close(fd);
    } else {
        log_event("", "mode", "standalone");
        ret = standalone_run(script);
    }

    fclose(g_log_file);
    return ret;
}
