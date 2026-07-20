# BLE Transport

Pixel Dumpster exposes a **Nordic UART Service (NUS)** GATT peripheral that
carries the same newline-terminated JSON protocol used over USB serial
([wizard-protocol.md](wizard-protocol.md), [dumpster-diver.md](dumpster-diver.md)).

## Priority

| Role | Transport |
|------|-----------|
| **Primary** | WiFi HTTP (`:8088`) when the device is on the LAN |
| **Fallback** | BLE NUS NDJSON — list / play / stop / status / upload / set_playback |
| **Wired** | USB serial NDJSON (wizard session + same content commands) |

In `pd-control`, an explicit **Control via** preference wins when that link is
connected; otherwise the order is **WiFi → BLE → USB**. SoftAP and a generic
HTTP/REST tunnel over BLE are deferred. Full-frame draw remains first-class on
WiFi; BLE is for control and content sync when the device is offline or mDNS
is unavailable.

## UUIDs

| Role | UUID |
|------|------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (host → device, write) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (device → host, notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Advertise name defaults to the device name / hostname / `pixel-dumpster`.

## Firmware

`components/pd-ble` starts NimBLE after wizard/serial-cmd init and mirrors all
outbound NDJSON (wizard + serial-cmd) onto TX notifies. Inbound RX writes feed
`pd_wizard_feed_bytes` directly. FS commands (`list`, `stop`, `upload_*`) run
on `pd_serial_heavy` (12 KiB internal stack — 6 KiB overflowed on `list`).
`play` uses `pd_content_play_async` (shared `pd_play` worker). NimBLE host
allocations use PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`).

Streaming content store lives in `pd_content_upload_*` and is driven by
`pd-serial-cmd` so USB and BLE share one path.

## NDJSON commands

One JSON object per line. Shared with USB via `pd-serial-cmd`.

| Command | Request (host → device) | Typical response |
|---------|-------------------------|------------------|
| List | `{"cmd":"list"}` | `{"type":"list","items":[...]}` |
| Play | `{"cmd":"play","path":"...","transition":"none","duration_ms":0}` | `{"type":"ack","cmd":"play","ok":true}` |
| Stop | `{"cmd":"stop"}` | `{"type":"ack","cmd":"stop","ok":true}` |
| Status | `{"cmd":"status"}` | `{"type":"status","playing":…,"path":…,"cache":…}` |
| Playback opts | `{"cmd":"set_playback"}` (probe) or `{"cmd":"set_playback","auto_quantize_palette":true,"save":true}` | `{"type":"ack","cmd":"set_playback","ok":true,"auto_quantize_palette":…}` |
| Upload begin | `{"cmd":"upload_begin","path":"images/foo.png","size":N}` | `{"type":"ack","cmd":"upload_begin","ok":true,…}` |
| Upload chunk | `{"cmd":"upload_chunk","data":"<base64>"}` | `{"type":"ack","cmd":"upload_chunk","ok":true,"received":N}` |
| Upload end | `{"cmd":"upload_end"}` | `{"type":"ack","cmd":"upload_end","ok":true,"size":N}` |
| Upload abort | `{"cmd":"upload_abort"}` | `{"type":"ack","cmd":"upload_abort","ok":true}` |

`set_playback` with no fields acks the current `auto_quantize_palette` without
changing it. With `"save":true`, the value is persisted to content config.
Auto-quantize applies to **sequences only**; see [api.md](api.md).

Keep raw upload chunk size ~150 bytes so each NDJSON line stays under the
wizard line buffer and typical ATT MTU. Max file size matches HTTP
`/api/upload` (2 MiB).

Wizard layout commands (`hello`, `nav`, `key`, …) use the same framing — see
[wizard-protocol.md](wizard-protocol.md).

## Hosts

### pd-control

**Settings** (sidebar) holds connection cards: USB, Bluetooth, WiFi, plus Device
Info, Layout, Playback, and Brightness (`#pd-card-*` anchors).

- **Bluetooth card** — Scan → Connect (Tauri: `ble_scan`, `ble_connect`,
  `ble_send`, `ble_play`, `ble_stop`, `ble_status`, upload helpers).
- **Transport dock** — icons jump to the matching Settings card; clicking a
  *connected* transport also selects it as **Control via**.
- **Control via picker** — on Content (and related views) chooses WiFi / BLE /
  USB when multiple links are up.
- **Content** — Upload PNG via native file dialog (≤ 2 MiB → `images/<filename>`
  over the active control transport). Play/list/status/stop follow Control via.
- **Launch Wizard** — Next/Back tour over Settings cards (not the firmware
  panel wizard).
- **Hardware panel wizard** — Panel Layout → “Configure panels over USB/BLE”
  opens `WizardPanel` (firmware `pd-wizard` over USB or BLE).

Content/control routing lives in `pd-control/src/lib/deviceControl.ts`. When
BLE or USB is up without mDNS, the app restores a remembered device session so
Content is usable offline.

BLE status polling is slower (~15s) and gated while play/list/upload are busy,
to reduce GATT contention.

### pd-ble-bridge + dumpster-diver

```bash
# On the Pi (or Mac):
python3 tools/pd-ble-bridge/pd_ble_bridge.py --name pixel-dumpster --port 9877

# dumpster-diver (HTTP still preferred when configured for wifi):
./dumpster-diver --ble-bridge 127.0.0.1:9877
```

Config:

```json
{
  "transport": "ble",
  "ble": { "bridge": "127.0.0.1:9877" }
}
```

With `transport: "ble"` (or `--serial` / `--ble-bridge`), auto-upload uses the
NDJSON upload commands instead of `POST /api/upload`.

Smoke test:

```bash
printf '%s\n' '{"cmd":"status"}' | nc 127.0.0.1 9877
```
