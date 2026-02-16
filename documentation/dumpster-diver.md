# dumpster-diver

EmulationStation bridge daemon for pixel-dumpster. Watches ES log files for events and sends content commands to the device via HTTP API.

## Features

- **Real-time log watching** - Uses kqueue (macOS) or inotify (Linux) for efficient file monitoring
- **Multiple event types** - System select, game select, game launch, game end
- **Flexible content lookup** - Exact game match → system game path → system art → fallback
- **Configurable** - JSON config for device, events, systems, and game mappings
- **Cross-platform** - Builds on macOS and Linux

## Building

```bash
cd tools
cc -o dumpster-diver dumpster-diver.c cJSON.c -lcurl -Wall -O2
```

## Usage

```bash
./dumpster-diver [options]

Options:
  --config FILE    Path to config file (default: ~/.config/dumpster-diver/config.json)
  --log FILE       Path to ES log file (default: ~/.emulationstation/es_log.txt)
  --host IP        Device IP address (overrides config)
  --port PORT      Device port (default: 8088)
  --verbose        Enable verbose logging
  --dry-run        Parse events but don't send HTTP requests
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
  "defaults": {
    "system_fallback": "images/systems/default.png",
    "game_fallback": "images/games/default.png",
    "transition": "fade",
    "duration_ms": 800
  },
  "events": {
    "game_select": true,
    "game_launch": true,
    "game_end": true,
    "system_select": true
  },
  "systems": {
    "nes": {
      "art": "images/systems/nes.png",
      "game_path": "images/games/nes/"
    },
    "snes": {
      "art": "images/systems/snes.png",
      "game_path": "images/games/snes/"
    },
    "arcade": {
      "art": "images/systems/arcade.png",
      "game_path": "images/games/arcade/"
    }
  },
  "games": {
    "Pac-Man": "images/pac-ghost",
    "Super Mario Bros": "images/sprite-test"
  }
}
```

## Content Lookup Chain

When a game is selected, dumpster-diver looks up content in this order:

1. **Exact game name match** - Checks `games` object for exact key match
2. **System game path** - Looks for `{system.game_path}{game_name}.png`
3. **System art** - Falls back to `systems.{system}.art`
4. **Global fallback** - Uses `defaults.game_fallback`

## Event Types

| Event | ES Log Pattern | Action |
|-------|----------------|--------|
| System Select | `SystemView::onCursorChanged(): cursor changed to <system>` | Show system art |
| Game Select | `GamelistView::onCursorChanged(): cursor changed to <game>` | Show game art |
| Game Launch | `Running game: <rom_path>` | Show game art |
| Game End | `Game ended` | Return to system art |

## Running as a Service

### macOS (launchd)

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

1. Start the daemon with `--verbose --dry-run`:
   ```bash
   ./dumpster-diver --config config.json --log test.log --verbose --dry-run
   ```

2. In another terminal, append test events:
   ```bash
   echo 'SystemView::onCursorChanged(): cursor changed to arcade' >> test.log
   echo 'GamelistView::onCursorChanged(): cursor changed to Pac-Man' >> test.log
   ```

3. Watch the daemon output for parsed events and (dry-run) HTTP requests.

## Troubleshooting

- **"log file not accessible"** - Ensure the ES log file exists and is readable
- **"HTTP POST failed"** - Check device IP/port and network connectivity
- **No events detected** - Enable `--verbose` to see raw log lines; check ES log format matches expected patterns
