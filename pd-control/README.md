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

## Getting Started

```bash
# Install frontend dependencies
npm install

# Run in development mode (hot-reload)
npm run tauri dev

# Build for production
npm run tauri build
```

## Architecture

### Backend (Rust — `src-tauri/`)

- **`discovery.rs`** — mDNS service browser for `_pdumpster._tcp` (ESP32 devices)
  and `_dumpster-diver._tcp` (daemons)
- **`device_api.rs`** — HTTP client for ESP32 REST API (`/api/play`, `/api/stop`,
  `/api/status`, `/api/content`, `/api/config`)
- **`daemon_api.rs`** — HTTP client for dumpster-diver control API (`/api/status`,
  `/api/config`, `/api/reload`, `/api/event`, `/api/log`)
- **`commands.rs`** — Tauri command handlers bridging frontend ↔ Rust

### Frontend (React + TypeScript + Tailwind — `src/`)

- **`App.tsx`** — Main layout with sidebar + content area
- **`components/Sidebar.tsx`** — Device list, scanning, manual add
- **`components/DevicePanel.tsx`** — ESP32 device management (playback, content
  browser, config)
- **`components/DaemonPanel.tsx`** — Daemon management (status, live log, test
  event injection, config)
- **`lib/api.ts`** — Typed Tauri invoke wrappers
- **`lib/types.ts`** — Shared TypeScript interfaces

## Network Discovery

Devices advertise themselves via mDNS:

| Service                   | Port | Description           |
|---------------------------|------|-----------------------|
| `_pdumpster._tcp`         | 8088 | ESP32 marquee device  |
| `_dumpster-diver._tcp`    | 7070 | RetroPie daemon       |

TXT records include `version`, `width`, `height`, and `name`.

## Daemon Control API

The `dumpster-diver` daemon exposes these endpoints on port 7070 (default):

| Method | Path          | Description                    |
|--------|---------------|--------------------------------|
| GET    | /api/status   | Daemon state and config summary|
| GET    | /api/config   | Full config JSON               |
| POST   | /api/reload   | Reload config + gamelists      |
| POST   | /api/event    | Inject test event into FIFO    |
| GET    | /api/log      | Last 200 log lines             |
