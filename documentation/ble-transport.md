# BLE Secondary Transport

Pixel Dumpster exposes a **Nordic UART Service (NUS)** GATT peripheral that
carries the same newline-terminated JSON protocol used over USB serial
([wizard-protocol.md](wizard-protocol.md), [dumpster-diver.md](dumpster-diver.md)).

## Priority

| Role | Transport |
|------|-----------|
| **Primary** | WiFi HTTP (`:8088`) when the device is on the LAN |
| **Secondary** | Content upload/sync over BLE NDJSON (`upload_begin` / `upload_chunk` / `upload_end`) |
| **Tertiary** | Generic HTTP/REST tunnel over BLE (not in this slice) |

SoftAP and automatic WiFi-failover are deferred. Full-frame draw remains first-class on WiFi.

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

## Content upload (secondary)

Host → device (one JSON object per line):

```json
{"cmd":"upload_begin","path":"marquees/arcade/pacman.png","size":12345}
{"cmd":"upload_chunk","data":"<base64>"}
{"cmd":"upload_end"}
```

On error or cancel:

```json
{"cmd":"upload_abort"}
```

Acks:

```json
{"type":"ack","cmd":"upload_begin","ok":true,"path":"...","size":12345}
{"type":"ack","cmd":"upload_chunk","ok":true,"received":N}
{"type":"ack","cmd":"upload_end","ok":true,"size":12345}
```

Keep raw chunk size ~150 bytes so each NDJSON line stays under the wizard line
buffer and typical ATT MTU. Max file size matches HTTP `/api/upload` (2 MiB).

## Hosts

### pd-control

Device Setup Wizard → **Bluetooth** → Scan → Connect. Uses the same wizard
commands as USB. Tauri commands: `ble_scan`, `ble_connect`, `ble_send`,
`ble_play`, `ble_stop`, `ble_status`.

Content UI (play / stop / status / list / upload) routes by live link state:
**WiFi HTTP → BLE NDJSON → USB wizard session**. Dock icons mean an active
session (BLE GATT connected, or USB wizard connected)—not merely that a serial
port is present. `upload_content_to_device` accepts an optional `via` of
`wifi` | `bluetooth` | `usb`.

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
