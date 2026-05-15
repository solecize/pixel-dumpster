# dumpster-diver

EmulationStation bridge daemon for pixel-dumpster. Uses native ES scripting hooks to receive events via a named pipe (FIFO), parses `gamelist.xml` for marquee metadata, and sends content commands to the device via HTTP API or USB serial.

## Features

- **ES scripting hooks** — Native event scripts for `game-select`, `system-select`, `game-start`, `game-end`
- **FIFO event pipe** — Zero-latency, no disk I/O event delivery from ES to daemon
- **gamelist.xml parsing** — Reads `<marquee>`, `<image>`, `<name>`, `<path>` tags to resolve display names to ROM filenames
- **6-level content lookup** — Config match → ROM name match → marquee convention → system game path → system art → fallback
- **Dual transport** — WiFi (HTTP POST) or USB serial (JSON over UART/JTAG)
- **Multiple event types** — System select, game select, game launch, game end
- **Configurable** — JSON config for device, events, systems, game mappings, ES paths, and marquee settings
- **Log watcher fallback** — kqueue (macOS) / inotify (Linux) for dev/testing without ES
- **Cross-platform** — Builds on macOS and Linux (ARM for RetroPie)

## Building

```bash
# Desktop / development
cd tools
cc -o dumpster-diver dumpster-diver.c cJSON.c -lcurl -Wall -O2

# Cross-compile for RetroPie (ARM)
arm-linux-gnueabihf-gcc -o dumpster-diver dumpster-diver.c cJSON.c -lcurl -Wall -O2
```

## Usage

```bash
./dumpster-diver [options]

Options:
  --config FILE    Config file (default: ~/.config/dumpster-diver/config.json)
  --fifo PATH      FIFO for ES events (default: /tmp/dumpster-diver.fifo)
  --log FILE       Fallback: watch ES log file instead of FIFO
  --host IP        Device IP address (overrides config)
  --port PORT      Device port (default: 8088)
  --serial DEVICE  Use serial transport (e.g. /dev/ttyACM0)
  --baud RATE      Serial baud rate (default: 115200)
  --roms PATH      ROMs directory (default: ~/RetroPie/roms)
  --gamelists PATH Gamelists directory (default: ~/.emulationstation/gamelists)
  --verbose        Enable verbose logging
  --dry-run        Parse events without sending commands
  --help           Show help
```

## Configuration

Create `~/.config/dumpster-diver/config.json`:

```json
{
  "device": {
    "host": "192.168.1.154",
    "port": 8088
  },
  "transport": "wifi",
  "serial": {
    "device": "/dev/ttyACM0",
    "baud": 115200
  },
  "fifo": "/tmp/dumpster-diver.fifo",
  "es": {
    "gamelists_path": "~/.emulationstation/gamelists",
    "roms_path": "~/RetroPie/roms"
  },
  "marquee": {
    "device_prefix": "marquees"
  },
  "defaults": {
    "system_fallback": "marquees/systems/default",
    "game_fallback": "marquees/default",
    "transition": "fade",
    "duration_ms": 500
  },
  "events": {
    "game_select": true,
    "game_launch": true,
    "game_end": true,
    "system_select": true
  },
  "systems": {
    "arcade": {
      "art": "marquees/systems/arcade",
      "game_path": "marquees/arcade/"
    }
  },
  "games": {
    "Pac-Man": "images/pac-ghost",
    "Super Mario Bros": "images/sprite-test"
  }
}
```

### Config Fields

| Field | Description |
|-------|-------------|
| `transport` | `"wifi"` (HTTP) or `"serial"` (USB) |
| `serial.device` | Serial port path (e.g. `/dev/ttyACM0`) |
| `serial.baud` | Baud rate (default `115200`) |
| `fifo` | FIFO path for ES events (default `/tmp/dumpster-diver.fifo`) |
| `es.gamelists_path` | Where ES stores `gamelist.xml` per system |
| `es.roms_path` | Where ROMs are stored per system |
| `marquee.device_prefix` | Base path for marquee content on the device |

## Content Lookup Chain

When a game is selected, dumpster-diver resolves content in this order:

1. **Config display name match** — `games["Pac-Man"]`
2. **Config ROM name match** — `games["pacman"]` (resolved via gamelist.xml)
3. **Marquee convention** — `{device_prefix}/{system}/{romname}` (e.g. `marquees/arcade/pacman`)
4. **System game path** — `{system.game_path}{romname}` (e.g. `marquees/arcade/pacman`)
5. **System art** — `systems.{system}.art`
6. **Global fallback** — `defaults.game_fallback`

The gamelist.xml parser resolves display names (e.g. "Pac-Man") to ROM filenames (e.g. "pacman") by reading the `<path>` and `<name>` tags. It also reads `<marquee>` and `<image>` tags for future use.

## Transport Modes

### WiFi (HTTP)

Default mode. Sends `POST /api/play` requests to the device:
```
POST http://192.168.1.154:8088/api/play
{"path":"marquees/arcade/pacman","transition":"fade","duration_ms":500}
```

### USB Serial

For wired setups. Sends newline-terminated JSON over serial:
```
{"cmd":"play","path":"marquees/arcade/pacman","transition":"fade","duration_ms":500}\n
```

The ESP32 responds with ack:
```
{"type":"ack","cmd":"play","ok":true}\n
```

**Note:** USB serial and USB thumb drive cannot be used simultaneously (same physical port, different USB modes).

### Serial Command Protocol

Commands sent from host to ESP32:

| Command | JSON | Response |
|---------|------|----------|
| Play content | `{"cmd":"play","path":"...","transition":"fade","duration_ms":500}` | `{"type":"ack","cmd":"play","ok":true}` |
| Stop playback | `{"cmd":"stop"}` | `{"type":"ack","cmd":"stop","ok":true}` |
| Get status | `{"cmd":"status"}` | `{"type":"status","playing":true,"path":"..."}` |
| List content | `{"cmd":"list"}` | `{"type":"list","items":[...]}` |

## Event Sources

### Primary: ES Scripting Hooks + FIFO (recommended)

Stock RetroPie EmulationStation fires scripting events for cursor changes. The install script places event scripts in `~/.emulationstation/scripts/{event}/` that write structured lines to a named pipe (FIFO).

**ES events used:**

| Event | ES Script Args | FIFO Line |
|-------|---------------|-----------|
| `game-select` | `$1=system $2=rom_path $3=game_name` | `game-select\|arcade\|/path/to/rom.zip\|Pac-Man` |
| `system-select` | `$1=system_name` | `system-select\|arcade` |
| `game-start` | `$1=rom_path $2=basename` | `game-start\|/path/to/rom.zip\|pacman` |
| `game-end` | (none) | `game-end` |

No ES debug logging or custom ES build required. All events fire natively in stock RetroPie ES.

### Fallback: ES Log File Watching

For macOS development or non-RetroPie setups, use `--log` to watch `es_log.txt`:

```bash
./dumpster-diver --log ~/.emulationstation/es_log.txt --verbose --dry-run
```

**Note:** Log-based cursor events require `emulationstation --debug`. Without it, only game launch/end are detected.

## RetroPie Installation

Use the included install script:

```bash
# WiFi transport
./tools/install-retropie.sh --host 192.168.1.154

# Serial transport
./tools/install-retropie.sh --serial /dev/ttyACM0
```

The script:
1. Copies the binary to `/opt/retropie/configs/all/`
2. Creates config at `~/.config/dumpster-diver/config.json`
3. Installs ES event scripts into `~/.emulationstation/scripts/{event}/`
4. Creates FIFO at `/tmp/dumpster-diver.fifo`
5. Adds dumpster-diver to `autostart.sh`

## Running as a Service

### macOS (launchd) — log fallback mode

Create `~/Library/LaunchAgents/com.pixeldumpster.dumpster-diver.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.pixeldumpster.dumpster-diver</string>
    <key>ProgramArguments</key>
    <array>
        <string>/path/to/dumpster-diver</string>
        <string>--log</string>
        <string>/path/to/es_log.txt</string>
        <string>--config</string>
        <string>/path/to/config.json</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>
```

Load with: `launchctl load ~/Library/LaunchAgents/com.pixeldumpster.dumpster-diver.plist`

### Linux (systemd)

Create `~/.config/systemd/user/dumpster-diver.service`:

```ini
[Unit]
Description=Dumpster Diver - EmulationStation Bridge
After=network.target

[Service]
ExecStart=/path/to/dumpster-diver --config /path/to/config.json
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

Enable with: `systemctl --user enable --now dumpster-diver`

## Testing

### FIFO mode (simulating ES events)

1. Start the daemon:
   ```bash
   ./dumpster-diver --config tools/dumpster-diver-config.json --verbose --dry-run
   ```

2. In another terminal, send events to the FIFO:
   ```bash
   echo 'system-select|arcade' > /tmp/dumpster-diver.fifo
   echo 'game-select|arcade|/home/pi/RetroPie/roms/arcade/pacman.zip|Pac-Man' > /tmp/dumpster-diver.fifo
   echo 'game-start|/home/pi/RetroPie/roms/arcade/pacman.zip|pacman' > /tmp/dumpster-diver.fifo
   echo 'game-end' > /tmp/dumpster-diver.fifo
   ```

### Log fallback mode

1. Start with `--log`:
   ```bash
   ./dumpster-diver --config tools/dumpster-diver-config.json --log /tmp/test-es.log --verbose --dry-run
   ```

2. Append ES-format lines:
   ```bash
   echo 'SystemView::onCursorChanged(): cursor changed to arcade' >> /tmp/test-es.log
   echo 'GamelistView::onCursorChanged(): cursor changed to Pac-Man' >> /tmp/test-es.log
   ```

## Troubleshooting

- **"cannot create FIFO"** — Check permissions on `/tmp/`; try `mkfifo /tmp/dumpster-diver.fifo` manually
- **No events from FIFO** — Verify ES scripts are installed: `ls ~/.emulationstation/scripts/game-select/`
- **"HTTP POST failed"** — Check device IP/port and network connectivity
- **"serial: cannot open"** — Check serial device path and permissions (`sudo usermod -a -G dialout $USER`)
- **Log fallback: no cursor events** — ES debug logging must be enabled (`emulationstation --debug`)
- **gamelist: 0 entries** — Check `es.gamelists_path` and `es.roms_path` in config
