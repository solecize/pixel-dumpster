# Wizard Serial Protocol

## Overview

The setup wizard uses a **thin-client architecture**: the host sends user input
to the ESP32 over USB serial or BLE NUS; the device owns all state, performs
hardware operations (Wi-Fi scan, config save), and drives both:

1. **NDJSON responses** — JSON lines sent back to the host for UI rendering.
2. **HUB75 display** — the same wizard state mirrored on the LED matrix in
   real time.

Primary host today is **pd-control** (`WizardPanel`, opened from Settings →
Layout → “Configure panels over USB/BLE”). A standalone `wizard-cli` / serial
terminal still works with the same framing.

```
┌──────────────────┐  USB or BLE NUS   ┌──────────────────────┐
│  pd-control /    │ ────────────────► │  ESP32 (pd-wizard)   │
│  wizard-cli      │ ◄──────────────── │                      │
│  (host)          │   JSON lines      │  state machine       │
└──────────────────┘                   │  ├─ NDJSON responder  │
                                       │  └─ HUB75 renderer    │
                                       └──────────────────────┘
```

After setup completes, the same NDJSON pipe carries content commands from
`pd-serial-cmd` (`list` / `play` / `stop` / `status` / `set_playback` /
`upload_*`) — see [dumpster-diver.md](dumpster-diver.md) and
[ble-transport.md](ble-transport.md).

## Transport

- **Physical**: USB (USB-Serial-JTAG or USB-CDC), or BLE Nordic UART Service
  (same line framing — [ble-transport.md](ble-transport.md))
- **Baud** (USB): 115200 (default ESP-IDF monitor rate)
- **Framing**: newline-delimited JSON (`\n`-terminated lines)
- **Direction**:
  - **Host → Device**: single-line JSON commands
  - **Device → Host**: single-line JSON responses/events

Non-JSON lines from the device (e.g. ESP_LOG output) must be ignored by the
host. Filter for lines starting with `{`.

## Default Display Assumptions

On first boot the device does **not** know the panel resolution. The wizard
must still render on the HUB75 display, so:

- **Default assumed resolution: 32×32** — this is the smallest common HUB75
  panel size. On a physically larger panel (e.g. 64×64) the 32×32 content
  will appear in the upper-left quadrant, which is acceptable for setup.
- Once the user selects a resolution in the wizard, the device **reinitializes
  the display driver** at the chosen resolution and re-renders the current
  wizard step at full size.
- On very small or unusual panels where 32×32 doesn't fit, the serial CLI
  remains the primary interface. The HUB75 output is best-effort during setup.

## Message Format

### CLI → Device (Commands)

Every command is a JSON object with a `"cmd"` field:

```json
{"cmd": "<command>", ...params}
```

#### `hello` — Handshake

Sent when the CLI connects. Device responds with its current state.

```json
{"cmd": "hello"}
```

#### `nav` — Navigation

Move forward or backward in the wizard.

```json
{"cmd": "nav", "dir": "next"}
{"cmd": "nav", "dir": "back"}
```

#### `select` — Menu Selection

Select a menu item by index (0-based).

```json
{"cmd": "select", "index": 2}
```

#### `input` — Text Input

Submit a text value for the current step.

```json
{"cmd": "input", "value": "my-device-name"}
```

#### `key` — Raw Keypress

Forward a keypress for fine-grained control (arrow keys for menu cursor, typing
characters). Used for real-time menu navigation.

```json
{"cmd": "key", "code": "up"}
{"cmd": "key", "code": "down"}
{"cmd": "key", "code": "enter"}
{"cmd": "key", "code": "backspace"}
{"cmd": "key", "code": "a"}
```

Valid `code` values:
- `"up"`, `"down"` — menu cursor movement
- `"enter"` — confirm selection / submit input
- `"backspace"` — delete character in text input
- `"<"` — navigate back
- `">"` — navigate forward
- Any single printable character — text input

#### `scan_wifi` — Request Wi-Fi Scan

Ask the device to scan for nearby SSIDs. The device performs
`esp_wifi_scan_start()` and responds with results.

```json
{"cmd": "scan_wifi"}
```

### Device → CLI (Responses)

Every response is a JSON object with a `"type"` field:

#### `state` — Current Wizard State

Sent in response to `hello`, after navigation, or after any state change.
This is the primary message the CLI uses to render its UI.

```json
{
  "type": "state",
  "step": "matrix_size",
  "step_index": 0,
  "step_count": 9,
  "mode": "menu",
  "title": "Matrix size selection",
  "options": ["16x16", "32x32", "64x64", "128x128", "other"],
  "selected": 2,
  "value": "64x64",
  "nav": {"back": false, "next": true}
}
```

For text input steps:

```json
{
  "type": "state",
  "step": "device_name",
  "step_index": 3,
  "step_count": 9,
  "mode": "text",
  "title": "Type a name for this device",
  "value": "pixel-dumpster",
  "mask": false,
  "nav": {"back": true, "next": true}
}
```

For password input:

```json
{
  "type": "state",
  "step": "wifi_password",
  "step_index": 3,
  "step_count": 9,
  "mode": "text",
  "title": "Enter WiFi password (blank for open)",
  "value": "********",
  "mask": true,
  "nav": {"back": true, "next": true}
}
```

#### `wifi_scan` — Wi-Fi Scan Results

Sent in response to `scan_wifi` or automatically when entering the Wi-Fi step.

```json
{
  "type": "wifi_scan",
  "ssids": ["HomeNetwork", "Office5G", "Guest"],
  "scanning": false
}
```

While scanning is in progress:

```json
{
  "type": "wifi_scan",
  "ssids": [],
  "scanning": true
}
```

#### `complete` — Wizard Finished

Sent when the wizard completes and config is saved.

```json
{
  "type": "complete",
  "config": {
    "matrix_width": 64,
    "matrix_height": 64,
    "orientation_deg": 0,
    "wifi_ssid": "HomeNetwork",
    "wifi_password": "secret",
    "device_name": "pixel-dumpster",
    "hostname": "pixel-dumpster",
    "timezone": "CST6CDT",
    "static_ip": "",
    "static_gateway": "",
    "static_netmask": ""
  }
}
```

#### `error` — Error

```json
{
  "type": "error",
  "message": "Wi-Fi scan failed"
}
```

#### `display_reinit` — Display Reinitialized

Sent after the user selects a new resolution and the HUB75 driver is
restarted at the new size. Informational only.

```json
{
  "type": "display_reinit",
  "width": 64,
  "height": 64
}
```

## Wizard Steps

Step IDs and order match `pd_step_defs` in `components/pd-wizard/pd-wizard.c`.
Conditional steps (custom sizes, multi-panel chain, static IP, etc.) are skipped
when inactive. `step_index` / `step_count` in state messages count only
*active* steps.

| Step ID | Mode | Notes |
|---------|------|-------|
| `multi_panel` | menu | Single panel vs multi-panel chain |
| `matrix_size` | menu | Presets (e.g. 16×16 … 128×128) + other / reztest |
| `matrix_custom` | text | Custom matrix size when "other" selected |
| `panel_res` | menu | Per-panel resolution (multi-panel path) |
| `panel_res_custom` | text | Custom panel size |
| `panel_rows` / `panel_cols` | text | Chain grid |
| `chain_pattern` | menu | How panels are chained |
| `panel_rotation` | menu | Per-panel rotation |
| `panel_layout` | menu | Layout test pattern helper |
| `orientation` | menu | 0 / 90 / 180 / 270 |
| `scan_wiring` | menu | Scan mode / wiring |
| `color_order` | menu | RGB / colour order |
| `demo_patterns` | menu | On-device demo patterns |
| `wifi_ssid` | menu | Scan results + manual entry |
| `wifi_ssid_manual` | text | Manual SSID |
| `wifi_password` | text | Masked; blank = open |
| `device_name` / `hostname` | text | Identity |
| `timezone` | text | POSIX TZ (e.g. CST6CDT) |
| `static_ip` / `static_gateway` / `static_netmask` | text | Blank IP = DHCP |

## HUB75 Display Rendering

The device mirrors the wizard state on the HUB75 panel:

- **Boot (no config)**: display "SETUP" in upper-left using 5×7 bitmap font
  at 32×32 assumed resolution.
- **Menu steps**: show step title on line 1, highlight selected option with
  a different color. Scroll if options exceed visible rows.
- **Text steps**: show step title on line 1, show current input value on
  line 2 (masked with `*` for passwords).
- **Resolution change**: when the user confirms a matrix size, the driver
  reinitializes and the current step re-renders at the new resolution.
- **Completion**: show "DONE" or device name briefly, then transition to
  normal idle display.

### Font & Layout

- **Font**: 5×7 bitmap font (fits 6×8 cells with 1px spacing)
- **At 32×32**: 5 columns × 4 rows of text
- **At 64×64**: 10 columns × 8 rows of text
- **Colors**: white text on black, green highlight for selected item,
  yellow for title, red for errors

## Sequence Diagrams

### Normal Flow

```
CLI                          Device
 │                              │
 │──── {"cmd":"hello"} ────────►│
 │◄─── {type:state, step:matrix_size, ...} ──│
 │                              │  (HUB75 shows matrix menu)
 │──── {"cmd":"key","code":"down"} ──►│
 │◄─── {type:state, selected:3} ──│
 │                              │  (HUB75 updates highlight)
 │──── {"cmd":"key","code":"enter"} ─►│
 │◄─── {type:display_reinit, w:128, h:128} ──│
 │◄─── {type:state, step:orientation, ...} ──│
 │                              │  (HUB75 reinits + shows orientation)
 │  ...                         │
 │──── {"cmd":"key","code":"enter"} ─►│
 │◄─── {type:complete, config:{...}} ──│
 │                              │  (HUB75 shows "DONE")
```

### Wi-Fi Scan Flow

```
CLI                          Device
 │                              │
 │◄─── {type:state, step:wifi_ssid} ──│
 │◄─── {type:wifi_scan, scanning:true} ──│
 │                              │  (device runs esp_wifi_scan_start)
 │◄─── {type:wifi_scan, ssids:[...], scanning:false} ──│
 │◄─── {type:state, step:wifi_ssid, options:[...]} ──│
 │                              │  (HUB75 shows SSID list)
```

### Back Navigation

```
CLI                          Device
 │                              │
 │──── {"cmd":"key","code":"<"} ──►│
 │◄─── {type:state, step:matrix_size, value:"64x64"} ──│
 │                              │  (previous selection remembered)
```
