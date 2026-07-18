# API Reference

This document describes the ESP32 firmware's HTTP API, as actually implemented in
`components/pd-content/pd-content.c` and `components/pd-network/pd-network.c`.

> Related Documentation:
> [readme.md](../readme.md) | [development.md](development.md) | [transitions.md](transitions.md) | [wizard-protocol.md](wizard-protocol.md) | [dumpster-diver.md](dumpster-diver.md) | [pd-control/README.md](../pd-control/README.md)

## Overview

There are **two HTTP endpoint families** registered on the same server (port 8088).
They coexist for historical reasons — only one of them actually drives the LED
matrix.

| Family | Prefix | Registered by | Drives the display? |
|--------|--------|----------------|----------------------|
| **Content API** (current) | `/api/*` | `pd_content_register_http()` in `pd-content.c` | **Yes** — this is what `pd-control` and `dumpster-diver` use |
| **Legacy artifact API** | bare paths (`/reload`, `/state`, `/list`, `/upload`, `/wizard`) | `pd_network_start_http()` in `pd-network.c` | **No** — kept for backward compatibility; see [Legacy Artifact API](#legacy-artifact-api-does-not-drive-the-display) below |

If you're integrating a new client, use the **Content API**.

### Base URL

```
http://<device-ip-or-hostname>.local:8088
```

The device advertises itself via mDNS as `_pdumpster._tcp` on this port, with TXT
records `version`, `width`, `height`, and (if set) `name` (`pd-network.c`, function
`pd_network_start_mdns`).

### Content API registration timing

`/api/*` routes are only registered once WiFi is connected and the underlying HTTP
server exists — this happens in the main loop in `main/app-main.c`, shortly after
boot. Until then, only the legacy routes below respond.

---

## Content API (`/api/*`)

All request/response bodies are JSON unless noted. There is no authentication, no
rate limiting, and no CORS handling on any endpoint — errors are returned as plain
text via `httpd_resp_send_err`, not as structured JSON.

### GET /api/content

List playable content. Scans the device's content root (`/pd/content/`, see
[Content root](#content-root)).

**Response:**
```json
{
  "images": [
    { "path": "pacman.png", "name": "pacman.png", "sequence": false, "frames": 1 },
    { "path": "fire", "name": "fire", "sequence": true, "frames": 24, "fps": 12 }
  ]
}
```

```bash
curl http://pixel-dumpster.local:8088/api/content
```

### POST /api/play

Plays a static image or an animated sequence, with an optional transition.

**Request:**
```json
{
  "path": "fire",
  "transition": "wipe_left",
  "duration_ms": 800
}
```

- `path` (required) — relative to the content root. A directory plays as a PNG
  sequence (using its `meta.json`); a file plays as a static image.
- `transition` (optional) — name of any transition from
  [transitions.md](transitions.md). If omitted, the transition is chosen from the
  device's configured transition mode (`baseline`/`random`/`per-item`, see
  `GET /api/config` below).
- `duration_ms` (optional) — overrides the configured transition duration.
- Two special paths are handled outside the normal content root:
  `system/default` (renders the built-in default marquee) and `system/idle`
  (renders the idle/config screen with the device's current IP).

**Response:** `{"ok":true}`, or `404` if the content path doesn't exist.

```bash
curl -X POST http://pixel-dumpster.local:8088/api/play \
  -d '{"path":"fire","transition":"fade","duration_ms":600}'
```

> Note: per-transition parameters described in [transitions.md](transitions.md)
> (e.g. `zoom_vanish`, `bounce_*`, `block_size`) are **not** read from this
> request — those values are currently hardcoded in `pd-transition.c`.

### POST /api/stop

Stops playback and returns to idle.

**Response:** `{"ok":true}`

```bash
curl -X POST http://pixel-dumpster.local:8088/api/stop
```

### GET /api/status

Returns current playback status.

**Response (playing):**
```json
{
  "playing": true,
  "path": "fire",
  "sequence": true,
  "frame": 12,
  "total_frames": 24,
  "fps": 12
}
```

**Response (idle):** `{"playing": false}`

```bash
curl http://pixel-dumpster.local:8088/api/status
```

### POST /api/status/show

Shows an on-demand "source status" overlay (used by `pd-control`/`dumpster-diver`
to confirm connectivity) for a duration, then resumes whatever was playing before.

**Request (optional body):** `{"duration_ms": 5000}` — defaults to `5000` if
omitted or body is absent.

**Response:** `{"ok":true}`

### POST /api/upload

Uploads a file into the content root. Unlike the legacy `/upload` endpoint, this
one takes a **raw request body** (not multipart), streamed to disk in 8KB chunks,
and the destination path is given via a query parameter.

**Request:**
```
POST /api/upload?path=fire/0001.png
Content-Type: application/octet-stream

<raw file bytes>
```

Parent directories under the content root are created automatically. Max upload
size is 2MB; requests without a `?path=` query param or with an invalid
`Content-Length` are rejected with `400`.

**Response:** `{"ok":true}` (see `pd-content.c`, `http_content_upload` — the
literal success body may vary; check firmware for the exact string if you need to
parse it strictly).

```bash
curl -X POST --data-binary @fire/0001.png \
  "http://pixel-dumpster.local:8088/api/upload?path=fire/0001.png"
```

### POST /api/ota

Pushes a firmware image over WiFi into the inactive OTA partition, validates it,
sets it as the next boot target, and reboots. Body is the raw `.bin` (same file
`idf.py` flashes as the app image). Requires `Content-Length`. Max size is the
OTA slot size (~1.81 MiB).

**Request:**
```
POST /api/ota
Content-Type: application/octet-stream

<raw pixel-dumpster.bin>
```

**Response:** `{"ok":true,"reboot":true}` then the device restarts.

```bash
curl -X POST --data-binary @build/pixel-dumpster.bin \
  "http://pixel-dumpster.local:8088/api/ota"
```

The first image that includes this endpoint must still be flashed over USB; after
that, subsequent firmware updates can use this path.

### GET /api/config

Returns the current transition/display/attract configuration.

**Response:**
```json
{
  "transition": {
    "mode": "baseline",
    "baseline": "fade",
    "duration_ms": 800
  },
  "display": {
    "hold_ms": 5000,
    "loop_sequences": true,
    "background": "#000000",
    "overlay": ""
  },
  "attract": {
    "enabled": false,
    "path": "images/",
    "shuffle": true,
    "idle_timeout_ms": 0
  }
}
```

> `mode` can be `baseline` (always use `baseline` transition), `random`, or
> `per-item` — but `per-item` currently falls back to the baseline transition in
> the playback code (not yet fully implemented). The `attract` block is
> configuration only; there is no attract-mode runtime/scheduler yet, and no
> `/api/attract/*` endpoints exist.

### POST /api/config

Partially updates transition/display/attract config (merges — omitted fields are
left unchanged). Accepts the same shape as the `GET /api/config` response, with
any subset of fields.

```bash
curl -X POST http://pixel-dumpster.local:8088/api/config \
  -d '{"transition":{"mode":"random","duration_ms":500}}'
```

**Response:** `{"ok":true}`

### GET /api/config/layout

Returns the current panel/matrix layout.

**Response:**
```json
{
  "panel_width": 64,
  "panel_height": 32,
  "panel_rows": 1,
  "panel_cols": 2,
  "chain_pattern": 0,
  "panel_rotation_deg": 0,
  "color_order": 0,
  "matrix_width": 128,
  "matrix_height": 32,
  "device_name": "pixel-dumpster",
  "wifi_ssid": "network-name",
  "hostname": "pixel-dumpster"
}
```

### POST /api/config/layout

Updates panel layout (`panel_width`, `panel_height`, `panel_rows`, `panel_cols`,
`chain_pattern`, `panel_rotation_deg`, `color_order` — any subset). Recomputes the
virtual matrix size, persists the config, then **reboots the device** (`esp_restart()`
after a short delay) to apply it.

**Response:** `{"ok":true,"reboot":true}` (sent before the reboot happens)

### POST /api/config/preview

Same fields as `/api/config/layout`, but applies the change live via
`pd_display_reinit()` **without saving or rebooting**. Changes are lost on the
next reboot (the last saved config is restored).

**Response:**
- `{"ok":true}` on success
- `{"ok":false,"error":"Layout preview requires reboot on this hardware"}` if the
  display driver doesn't support live reinit
- `{"ok":false,"error":"reinit failed"}` on other failures

### POST /api/test/start

Starts a display test pattern, useful for verifying panel wiring/orientation
during setup.

**Request:**
```json
{ "pattern": "numbered_panels", "brightness": 128 }
```

Valid `pattern` values (case-insensitive, see `pd-display-tests.cpp`):
`numbered_panels`, `checkerboard` / `checkerboard_scroll`, `arrow_chain`,
`bouncing_ball`, `rgb_sweep`, `color_test`, `panel_layout`. `brightness`
(0–255) is optional. The pattern runs indefinitely until stopped.

**Response:** `{"ok":true}`, or `400` for a missing/unknown pattern.

### POST /api/test/stop

Stops the running test pattern.

**Response:** `{"ok":true}`

### POST /api/test/panel_select

Highlights a single panel in the chain (used with the `panel_layout` test pattern
to visually confirm chain order during setup).

**Request:** `{ "panel_index": 2 }`

**Response:** `{"ok":true}`

---

## Content root

The Content API operates on `/pd/content/` on the device's LittleFS partition
(`content_base = "<storage-base>/content"`, set in `pd_content_init()`), **not**
the `/pd/system/`, `/pd/game/`, `/pd/assets/` layout described for the legacy API
below. See [docs/content-system.md](../docs/content-system.md) for the
`meta.json` sequence-metadata format.

---

## Legacy Artifact API (does not drive the display)

`pd-network.c` registers a second, older set of routes on the same server. They
read and write real files, but nothing in this path currently updates what's shown
on the LED matrix — `pd_http_reload_handler` and the UDP/poll paths below all load
`/pd/now.json` into a local variable and discard it. Treat this API as **legacy /
kept for compatibility**; new integrations should use the Content API above.

### POST /reload

Loads `/pd/now.json` (no effect on the display — see note above).

**Response:** `204 No Content`

### GET /state

Returns the last-loaded `/pd/now.json` contents.

```json
{
  "mode": "idle|system|game|custom",
  "system": "mame",
  "game": "pacman",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

### GET /list

Lists filenames directly under the storage base path (`/pd/`), one level deep,
excluding dotfiles. Returns bare names only — no `size`, `type`, or timestamp
fields.

```json
{ "files": [ { "path": "default.png" }, { "path": "now.json" } ] }
```

### POST /upload

Uploads a raw file body to `?path=` (or `assets/upload.bin` if omitted), relative
to `/pd/`. Unlike `/api/upload`, this does **not** parse `multipart/form-data`
despite older client examples suggesting otherwise — send the raw bytes directly.

**Response:** `200 OK`, body `File uploaded`

### GET /wizard

Returns the current device config as seen by the setup wizard (`setup_complete`,
`device_name`, `hostname`, `timezone`, `wifi_ssid`, static IP fields, matrix
dimensions, orientation). Does not include `wifi_password`.

### POST /wizard

An HTTP alternative to the USB-serial wizard protocol (see
[wizard-protocol.md](wizard-protocol.md)) for submitting the same fields. Marks
`setup_complete = true` once `wifi_ssid`, `device_name`, `hostname`, and
`timezone` are all non-empty, then saves config. Does **not** reboot the device
itself.

### `/pd/` artifact layout

This directory structure is created by `pd-storage` and used by the legacy API
above. It is separate from the Content API's `/pd/content/` root:

```
/pd/
├── now.json          # read by /state and /reload; not wired to the display
├── default.png
├── system/<system>.png
├── game/<system>/<game>.png
└── assets/<custom>.png
```

---

## UDP "doorbell" (port 9876)

`pd-network.c` binds a non-blocking UDP socket on port `9876`. Any packet
received logs the sender and reloads `/pd/now.json` into a local variable — as
with `/reload`, **this does not currently update the display**. There is no
message-format parsing; packet contents are ignored.

```bash
echo "refresh" | nc -u -w0 pixel-dumpster.local 9876
```

## `now.json` polling

Separately, the main loop polls `/pd/now.json`'s mtime and logs when it changes
(`pd_network_poll_now_json`), again without driving the display. If you need the
device to react to file changes, use `POST /api/play` directly instead of relying
on this path.

---

## Error handling

Errors use `httpd_resp_send_err()`, which sends a plain-text body with the
corresponding HTTP status code — **not** a JSON `{error, code, timestamp}` object.
Common codes:

- `400 Bad Request` — missing/invalid JSON body, missing required field, unknown
  test pattern, missing `?path=`
- `404 Not Found` — content path doesn't exist (`/api/play`)
- `500 Internal Server Error` — out of memory, storage/config not available, file
  I/O failure

There is no rate limiting, no CORS headers, and no authentication on any endpoint
in the current firmware.

---

## Related clients

- `pd-control/` (Tauri desktop app) calls the Content API exclusively — see
  `pd-control/src-tauri/src/device_api.rs` and [pd-control/README.md](../pd-control/README.md).
- `tools/dumpster-diver.c` (RetroPie daemon) pushes artwork via
  `POST /api/play` over WiFi, or via the serial JSON protocol (see
  `pd-serial-cmd` and [dumpster-diver.md](dumpster-diver.md)).
