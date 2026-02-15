#!/usr/bin/env python3
import argparse
import curses
import os
import subprocess
import time

MENU_OPTIONS = [
    "16x16",
    "32x32",
    "64x64",
    "128x128",
    "other",
]

ORIENTATION_OPTIONS = [
    "0",
    "90",
    "180",
    "270",
]

WIFI_MANUAL_OPTION = "<manual entry>"


class WizardLogger:
    def __init__(self, path):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)

    def log(self, message):
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        with open(self.path, "a", encoding="utf-8") as handle:
            handle.write(f"[{timestamp}] {message}\n")


def prompt_line(stdscr, prompt, logger, field):
    curses.echo()
    stdscr.clear()
    stdscr.addstr(0, 0, prompt)
    stdscr.addstr(1, 0, "> ")
    stdscr.refresh()
    value = stdscr.getstr(1, 2).decode("utf-8", errors="ignore")
    curses.noecho()
    logger.log(f"input/{field}: {value}")
    return value


def prompt_text(stdscr, prompt, logger, field, mask=False, initial_value="", replace_on_type=False):
    stdscr.clear()
    stdscr.addstr(0, 0, prompt)
    stdscr.addstr(1, 0, "(← back, → next)")
    stdscr.addstr(3, 0, "> ")
    stdscr.refresh()
    value_chars = list(initial_value)
    touched = False
    display = "*" * len(value_chars) if mask else "".join(value_chars)
    if display:
        stdscr.addstr(3, 2, display)
        stdscr.move(3, 2 + len(display))
        stdscr.refresh()
    while True:
        key = stdscr.getch()
        if key in (curses.KEY_LEFT,):
            return "back", None
        if key in (curses.KEY_RIGHT,):
            value = "".join(value_chars)
            logger.log(f"input/{field}: {value}")
            return "next", value
        if key in (curses.KEY_ENTER, 10, 13):
            value = "".join(value_chars)
            logger.log(f"input/{field}: {value}")
            return "next", value
        if key in (curses.KEY_BACKSPACE, 127, 8):
            if value_chars:
                value_chars.pop()
                touched = True
        elif 32 <= key <= 126:
            if replace_on_type and not touched and value_chars:
                value_chars = []
            touched = True
            value_chars.append(chr(key))

        stdscr.addstr(3, 2, " " * 60)
        display = "*" * len(value_chars) if mask else "".join(value_chars)
        stdscr.addstr(3, 2, display)
        stdscr.move(3, 2 + len(display))
        stdscr.refresh()


def prompt_password(stdscr, prompt, logger, field, initial_value=""):
    action, value = prompt_text(
        stdscr,
        prompt,
        logger,
        field,
        mask=True,
        initial_value=initial_value,
        replace_on_type=True,
    )
    if action == "back":
        return "back", None
    return "next", value


def scan_wifi_ssids():
    candidates = []
    airport_path = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport"
    try:
        if os.path.exists(airport_path):
            result = subprocess.run([airport_path, "-s"], capture_output=True, text=True, check=False)
            if result.returncode == 0:
                lines = result.stdout.splitlines()[1:]
                for line in lines:
                    if not line.strip():
                        continue
                    ssid = line[:32].rstrip()
                    if ssid and ssid not in candidates:
                        candidates.append(ssid)
        else:
            result = subprocess.run(["nmcli", "-t", "-f", "SSID", "dev", "wifi"], capture_output=True, text=True, check=False)
            if result.returncode == 0:
                for line in result.stdout.splitlines():
                    ssid = line.strip()
                    if ssid and ssid not in candidates:
                        candidates.append(ssid)
    except Exception:
        return []

    return candidates


def render_menu(stdscr, title, options, selected_index, help_text):
    stdscr.clear()
    stdscr.addstr(0, 0, title)
    stdscr.addstr(1, 0, help_text)
    for idx, label in enumerate(options):
        prefix = ">" if idx == selected_index else " "
        stdscr.addstr(3 + idx, 0, f"{prefix} {idx + 1}. {label}")
    stdscr.refresh()


def run_menu(stdscr, logger, title, options, default_index=0):
    selected = default_index
    stdscr.keypad(True)
    while True:
        max_index = len(options)
        render_menu(
            stdscr,
            title,
            options,
            selected,
            f"Use ↑/↓ or 1-{max_index}, Enter to select. (← back, → next)",
        )
        key = stdscr.getch()
        logger.log(f"menu/key: {key}")
        if key in (ord("q"), ord("Q")):
            logger.log("wizard/quit")
            return "quit", None
        if key in (curses.KEY_LEFT,):
            return "back", None
        if key in (curses.KEY_RIGHT,):
            return "select", options[selected]
        if key in (curses.KEY_UP, ord("w"), ord("W")):
            selected = (selected - 1) % len(options)
            continue
        if key in (curses.KEY_DOWN, ord("s"), ord("S")):
            selected = (selected + 1) % len(options)
            continue
        if ord("1") <= key <= ord("9"):
            idx = key - ord("1")
            if 0 <= idx < len(options):
                selected = idx
                return "select", options[selected]
        if key in (curses.KEY_ENTER, 10, 13):
            return "select", options[selected]


def resolve_index(options, value, fallback=0):
    if value is None:
        return fallback
    try:
        return options.index(value)
    except ValueError:
        return fallback


def step_matrix_size(stdscr, logger, data):
    while True:
        current_value = data.get("matrix_size")
        if current_value and current_value not in MENU_OPTIONS:
            default_index = len(MENU_OPTIONS) - 1
        else:
            default_index = resolve_index(MENU_OPTIONS, current_value, 2)
        action, selection = run_menu(
            stdscr,
            logger,
            "Matrix size selection",
            MENU_OPTIONS,
            default_index=default_index,
        )
        if action == "back":
            return "back"
        if action == "quit":
            return "quit"
        logger.log(f"menu/selection: {selection}")
        if selection == "other":
            action, value = prompt_text(
                stdscr,
                "Enter custom matrix size (e.g., 128x64)",
                logger,
                "matrix_size",
                initial_value=data.get("matrix_size", ""),
            )
            if action == "back":
                continue
            data["matrix_size"] = value
            return "next"
        data["matrix_size"] = selection
        logger.log(f"input/matrix_size: {selection}")
        return "next"


def step_orientation(stdscr, logger, data):
    action, selection = run_menu(
        stdscr,
        logger,
        "Orientation selection",
        ORIENTATION_OPTIONS,
        default_index=resolve_index(ORIENTATION_OPTIONS, data.get("orientation"), 0),
    )
    if action == "back":
        return "back"
    if action == "quit":
        return "quit"
    data["orientation"] = selection
    logger.log(f"menu/orientation: {selection}")
    return "next"


def step_wifi(stdscr, logger, data):
    ssids = scan_wifi_ssids()
    if ssids:
        ssid_options = ssids + [WIFI_MANUAL_OPTION]
        action, wifi_ssid = run_menu(
            stdscr,
            logger,
            "WiFi SSID selection",
            ssid_options,
            default_index=resolve_index(ssid_options, data.get("wifi_ssid"), 0),
        )
        if action == "back":
            return "back"
        if action == "quit":
            return "quit"
        if wifi_ssid == WIFI_MANUAL_OPTION:
            action, wifi_ssid = prompt_text(
                stdscr,
                "Enter WiFi SSID",
                logger,
                "wifi_ssid",
                initial_value=data.get("wifi_ssid", ""),
            )
            if action == "back":
                return "back"
        else:
            logger.log(f"menu/wifi_ssid: {wifi_ssid}")
    else:
        action, wifi_ssid = prompt_text(
            stdscr,
            "Enter WiFi SSID",
            logger,
            "wifi_ssid",
            initial_value=data.get("wifi_ssid", ""),
        )
        if action == "back":
            return "back"

    data["wifi_ssid"] = wifi_ssid
    action, wifi_password = prompt_password(
        stdscr,
        "Enter WiFi password (blank for open)",
        logger,
        "wifi_password",
        initial_value=data.get("wifi_password", ""),
    )
    if action == "back":
        return "back"
    data["wifi_password"] = wifi_password
    return "next"


def step_device_name(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Type a name for this device",
        logger,
        "device_name",
        initial_value=data.get("device_name", ""),
    )
    if action == "back":
        return "back"
    data["device_name"] = value
    return "next"


def step_hostname(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Enter hostname",
        logger,
        "hostname",
        initial_value=data.get("hostname", ""),
    )
    if action == "back":
        return "back"
    data["hostname"] = value
    return "next"


def step_timezone(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Enter timezone (e.g., UTC0 or PST8PDT)",
        logger,
        "timezone",
        initial_value=data.get("timezone", ""),
    )
    if action == "back":
        return "back"
    data["timezone"] = value
    return "next"


def step_static_ip(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Enter static IP (blank for DHCP)",
        logger,
        "static_ip",
        initial_value=data.get("static_ip", ""),
    )
    if action == "back":
        return "back"
    data["static_ip"] = value
    if not value.strip():
        data["static_gateway"] = ""
        data["static_netmask"] = ""
    return "next"


def step_static_gateway(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Enter static gateway",
        logger,
        "static_gateway",
        initial_value=data.get("static_gateway", ""),
    )
    if action == "back":
        return "back"
    data["static_gateway"] = value
    return "next"


def step_static_netmask(stdscr, logger, data):
    action, value = prompt_text(
        stdscr,
        "Enter static netmask",
        logger,
        "static_netmask",
        initial_value=data.get("static_netmask", ""),
    )
    if action == "back":
        return "back"
    data["static_netmask"] = value
    return "next"


def render_summary(stdscr, data):
    stdscr.clear()
    stdscr.addstr(0, 0, "Wizard complete. Summary:")
    stdscr.addstr(2, 0, f"Matrix size: {data.get('matrix_size', '')}")
    stdscr.addstr(3, 0, f"Orientation: {data.get('orientation', '')}")
    stdscr.addstr(4, 0, f"WiFi SSID: {data.get('wifi_ssid', '')}")
    stdscr.addstr(5, 0, f"WiFi password: {data.get('wifi_password', '')}")
    stdscr.addstr(6, 0, f"Device name: {data.get('device_name', '')}")
    stdscr.addstr(7, 0, f"Hostname: {data.get('hostname', '')}")
    stdscr.addstr(8, 0, f"Timezone: {data.get('timezone', '')}")
    if data.get("static_ip", "").strip():
        stdscr.addstr(9, 0, f"Static IP: {data.get('static_ip', '')}")
        stdscr.addstr(10, 0, f"Static gateway: {data.get('static_gateway', '')}")
        stdscr.addstr(11, 0, f"Static netmask: {data.get('static_netmask', '')}")
    else:
        stdscr.addstr(9, 0, "Static IP: DHCP")
    stdscr.addstr(13, 0, "Press any key to exit.")
    stdscr.refresh()
    stdscr.getch()


def run_wizard(stdscr, log_path):
    logger = WizardLogger(log_path)
    logger.log("wizard/start")
    data = {}
    steps = [
        {"name": "matrix", "run": step_matrix_size},
        {"name": "orientation", "run": step_orientation},
        {"name": "wifi", "run": step_wifi},
        {"name": "device_name", "run": step_device_name},
        {"name": "hostname", "run": step_hostname},
        {"name": "timezone", "run": step_timezone},
        {"name": "static_ip", "run": step_static_ip},
        {"name": "static_gateway", "run": step_static_gateway, "when": lambda d: bool(d.get("static_ip", "").strip())},
        {"name": "static_netmask", "run": step_static_netmask, "when": lambda d: bool(d.get("static_ip", "").strip())},
    ]

    index = 0
    while 0 <= index < len(steps):
        step = steps[index]
        if step.get("when") and not step["when"](data):
            index += 1
            continue
        action = step["run"](stdscr, logger, data)
        if action == "quit":
            return
        if action == "back":
            index -= 1
            continue
        index += 1

    logger.log("wizard/complete")
    render_summary(stdscr, data)


def main():
    parser = argparse.ArgumentParser(description="Pixel Dumpster CLI wizard (offline)")
    parser.add_argument(
        "--log",
        default="./logs/wizard-input.log",
        help="Path to log file (default: ./logs/wizard-input.log)",
    )
    args = parser.parse_args()
    try:
        curses.wrapper(run_wizard, args.log)
    except KeyboardInterrupt:
        print("\nAborted.")


if __name__ == "__main__":
    main()
