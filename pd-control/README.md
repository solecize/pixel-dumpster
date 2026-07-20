# Pixel Dumpster Control Center

A cross-platform Tauri desktop application for managing Pixel Dumpster ESP32 devices
and the `dumpster-diver` daemon remotely.

## Prerequisites

- **Rust** (1.70+): https://rustup.rs
- **Node.js** (18+): https://nodejs.org
- **Tauri CLI**: Installed automatically via npm devDependencies

### macOS

```bash
xcode-select --install   # if not already installed
```

Bluetooth control needs macOS Bluetooth permission for the app (usage strings are
in `src-tauri` `Info.plist`). Grant access when prompted on first scan/connect.

## Getting Started

```bash
# Install frontend dependencies
npm install

# Run in development mode (hot-reload)
npm run tauri dev

# Build for production
npm run tauri build
```

On macOS, prefer installing the `.app` bundle directly into `/Applications`
(e.g. `tauri build --bundles app`) rather than opening a DMG that may auto-eject.

## Architecture

### Backend (Rust — `src-tauri/`)

- **`discovery.rs`** — mDNS browser for `_pdumpster._tcp` (ESP32) and
  `_dumpster-diver._tcp` (daemons)
- **`device_api.rs`** — HTTP client for ESP32 Content API (`/api/play`,
  `/api/stop`, `/api/status`, `/api/content`, `/api/config`, `/api/upload`, …)
- **`ble_link.rs`** — BLE NUS (btleplug): scan, connect, NDJSON send/poll,
  play/stop/status/list/upload
- **`daemon_api.rs`** — HTTP client for dumpster-diver control API
- **`commands.rs`** — Tauri command handlers (HTTP, BLE, USB wizard, flash, SSH)

### Frontend (React + TypeScript + Tailwind — `src/`)

| Area | Role |
|------|------|
| **Content** | Browse device content, play/stop, Upload PNG, Control via picker |
| **Settings** | USB / Bluetooth / WiFi cards, Device Info, Layout, Playback, Brightness |
| **Flash** | ESP32 firmware flasher |
| **Pi** | dumpster-diver install / daemon panel |

Key modules:

- **`DeviceSetupPanel.tsx`** — Settings cards + Launch Wizard (card tour)
- **`WizardPanel.tsx`** — firmware panel-layout wizard over USB/BLE (from Layout)
- **`ContentPanel.tsx`** — content list, play, Upload PNG dialog
- **`ControlViaPicker.tsx`** + **`lib/transportPrefs.ts`** — preferred control transport
- **`lib/deviceControl.ts`** — WiFi / BLE / USB routing for play, list, status, upload, auto-quantize
- **`lib/deviceSession.ts`** — remember last device for offline BLE/USB Content
- **`lib/settingsCards.ts`** — card order and `#pd-card-*` anchors
- **`TransportDock`** — connection indicators; focus Settings cards / set Control via

### Control path

1. If the user picked **Control via** and that link is connected → use it.
2. Else **WiFi → Bluetooth → USB**.

Dock: clicking a *connected* transport selects it for control and scrolls to its
Settings card. See [ble-transport.md](../documentation/ble-transport.md).

### Settings vs wizards

- **Launch Wizard** — Next/Back tour through Settings cards (USB → … → Brightness).
- **Hardware wizard** — Panel Layout → “Configure panels over USB/BLE” →
  `WizardPanel` talking to firmware `pd-wizard`.

### Content upload

**Upload PNG** opens a native file dialog (`@tauri-apps/plugin-dialog`), then
uploads via `upload_local_file_to_device` to `images/<filename>` (max 2 MiB)
over the active control transport (HTTP, BLE NDJSON, or USB).

Playback settings include **auto-quantize palette** (sequences only), read/set
over WiFi `/api/config` or BLE/USB `set_playback`.

## Network Discovery

Devices advertise themselves via mDNS:

| Service                   | Port | Description           |
|---------------------------|------|-----------------------|
| `_pdumpster._tcp`         | 8088 | ESP32 marquee device  |
| `_dumpster-diver._tcp`    | 7070 | RetroPie daemon       |

TXT records include `version`, `width`, `height`, and `name`.

Without mDNS, connect Bluetooth or USB and use Content with the remembered
device session.

## Daemon Control API

The `dumpster-diver` daemon exposes these endpoints on port 7070 (default):

| Method | Path          | Description                    |
|--------|---------------|--------------------------------|
| GET    | /api/status   | Daemon state and config summary|
| GET    | /api/config   | Full config JSON               |
| POST   | /api/reload   | Reload config + gamelists      |
| POST   | /api/event    | Inject test event into FIFO    |
| GET    | /api/log      | Last 200 log lines             |
