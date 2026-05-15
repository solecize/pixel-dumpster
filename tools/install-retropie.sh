#!/bin/bash
# install-retropie.sh — Install dumpster-diver daemon on RetroPie
#
# Run on the RetroPie machine after building dumpster-diver:
#   ./install-retropie.sh [--serial /dev/ttyACM0] [--host 192.168.1.154]
#   ./install-retropie.sh --uninstall
#
# What this script does:
#   1. Copies dumpster-diver binary to /opt/retropie/configs/all/
#   2. Creates default config at ~/.config/dumpster-diver/config.json
#   3. Installs ES event scripts (game-select, system-select, game-start, game-end)
#   4. Creates FIFO at /tmp/dumpster-diver.fifo
#   5. Adds dumpster-diver to autostart.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/dumpster-diver"
ES_SCRIPTS_SRC="$SCRIPT_DIR/es-scripts"
INSTALL_DIR="/opt/retropie/configs/all"
CONFIG_DIR="$HOME/.config/dumpster-diver"
ES_SCRIPTS_DIR="$HOME/.emulationstation/scripts"
AUTOSTART="$INSTALL_DIR/autostart.sh"
FIFO_PATH="/tmp/dumpster-diver.fifo"

DEVICE_HOST="192.168.1.154"
DEVICE_PORT="8088"
SERIAL_DEVICE=""
TRANSPORT="wifi"
DO_UNINSTALL=false

# ---------- parse args ----------
while [[ $# -gt 0 ]]; do
    case $1 in
        --host)   DEVICE_HOST="$2"; shift 2 ;;
        --port)   DEVICE_PORT="$2"; shift 2 ;;
        --serial) SERIAL_DEVICE="$2"; TRANSPORT="serial"; shift 2 ;;
        --uninstall) DO_UNINSTALL=true; shift ;;
        --help)
            echo "Usage: $0 [options]"
            echo "  --host IP        Device IP (default: 192.168.1.154)"
            echo "  --port PORT      Device port (default: 8088)"
            echo "  --serial DEVICE  Use serial transport (e.g. /dev/ttyACM0)"
            echo "  --uninstall      Remove dumpster-diver (keeps config)"
            echo "  --help           Show this help"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------- uninstall ----------
if $DO_UNINSTALL; then
    echo "=== Dumpster-Diver Uninstaller ==="
    echo ""

    echo "[1/4] Removing binary..."
    sudo rm -f "$INSTALL_DIR/dumpster-diver"
    echo "  -> Removed $INSTALL_DIR/dumpster-diver"

    echo "[2/4] Removing ES event scripts..."
    ES_EVENTS="game-select system-select game-start game-end"
    for event in $ES_EVENTS; do
        rm -f "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh"
        # remove dir if empty
        rmdir "$ES_SCRIPTS_DIR/$event" 2>/dev/null || true
    done
    echo "  -> Removed ES event scripts"

    echo "[3/4] Removing FIFO..."
    rm -f "$FIFO_PATH"
    echo "  -> Removed $FIFO_PATH"

    echo "[4/4] Removing from autostart..."
    if [ -f "$AUTOSTART" ]; then
        sed -i '/dumpster-diver/d' "$AUTOSTART"
        echo "  -> Cleaned $AUTOSTART"
    fi

    echo ""
    echo "=== Uninstall complete ==="
    echo "Config preserved at: $CONFIG_DIR/config.json"
    echo "To remove config: rm -rf $CONFIG_DIR"
    exit 0
fi

echo "=== Dumpster-Diver RetroPie Installer ==="
echo ""

# ---------- check binary ----------
if [ ! -f "$BINARY" ]; then
    echo "ERROR: dumpster-diver binary not found at $BINARY"
    echo "Build it first: cc -o tools/dumpster-diver tools/dumpster-diver.c tools/cJSON.c -lcurl"
    exit 1
fi

# ---------- install binary ----------
echo "[1/5] Installing binary..."
sudo cp "$BINARY" "$INSTALL_DIR/dumpster-diver"
sudo chmod +x "$INSTALL_DIR/dumpster-diver"
echo "  -> $INSTALL_DIR/dumpster-diver"

# ---------- create config ----------
echo "[2/5] Creating config..."
mkdir -p "$CONFIG_DIR"

if [ -f "$CONFIG_DIR/config.json" ]; then
    echo "  -> Config already exists, skipping (backup at config.json.bak)"
    cp "$CONFIG_DIR/config.json" "$CONFIG_DIR/config.json.bak"
else
    cat > "$CONFIG_DIR/config.json" << CONFIGEOF
{
  "device": {
    "host": "$DEVICE_HOST",
    "port": $DEVICE_PORT
  },
  "transport": "$TRANSPORT",
  "serial": {
    "device": "${SERIAL_DEVICE:-/dev/ttyACM0}",
    "baud": 115200
  },
  "fifo": "$FIFO_PATH",
  "es": {
    "gamelists_path": "$HOME/.emulationstation/gamelists",
    "roms_path": "$HOME/RetroPie/roms"
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
  "systems": {},
  "games": {}
}
CONFIGEOF
    echo "  -> $CONFIG_DIR/config.json"
fi

# ---------- install ES event scripts ----------
echo "[3/5] Installing ES event scripts..."

ES_EVENTS="game-select system-select game-start game-end"
for event in $ES_EVENTS; do
    target_dir="$ES_SCRIPTS_DIR/$event"
    mkdir -p "$target_dir"
    src="$ES_SCRIPTS_SRC/$event/dumpster-diver.sh"
    if [ -f "$src" ]; then
        sed "s|FIFO=\"/tmp/dumpster-diver.fifo\"|FIFO=\"$FIFO_PATH\"|" "$src" > "$target_dir/dumpster-diver.sh"
        chmod +x "$target_dir/dumpster-diver.sh"
        echo "  -> $target_dir/dumpster-diver.sh"
    else
        echo "  WARNING: source script not found: $src"
    fi
done

# ---------- create FIFO ----------
echo "[4/5] Creating FIFO..."
if [ -p "$FIFO_PATH" ]; then
    echo "  -> FIFO already exists: $FIFO_PATH"
elif [ -e "$FIFO_PATH" ]; then
    echo "  WARNING: $FIFO_PATH exists but is not a FIFO, removing"
    rm -f "$FIFO_PATH"
    mkfifo "$FIFO_PATH"
    echo "  -> Created $FIFO_PATH"
else
    mkfifo "$FIFO_PATH"
    echo "  -> Created $FIFO_PATH"
fi

# ---------- autostart ----------
echo "[5/5] Adding to autostart..."
if [ -f "$AUTOSTART" ] && grep -q "dumpster-diver" "$AUTOSTART"; then
    echo "  -> Already in autostart.sh, skipping"
else
    # Insert dumpster-diver before emulationstation auto line
    if [ -f "$AUTOSTART" ]; then
        sed -i '/emulationstation/i \/opt/retropie/configs/all/dumpster-diver --verbose >> /tmp/dumpster-diver.log 2>&1 &' "$AUTOSTART"
    else
        echo '#!/bin/bash' > "$AUTOSTART"
        echo '/opt/retropie/configs/all/dumpster-diver --verbose >> /tmp/dumpster-diver.log 2>&1 &' >> "$AUTOSTART"
        echo 'emulationstation #auto' >> "$AUTOSTART"
    fi
    echo "  -> Added to $AUTOSTART"
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "Next steps:"
echo "  1. Pre-load marquee images on the pixel-dumpster device"
echo "     Convention: marquees/{system}/{romname} (e.g. marquees/arcade/pacman)"
echo "  2. Edit config: $CONFIG_DIR/config.json"
echo "  3. Reboot or run: $INSTALL_DIR/dumpster-diver --verbose"
echo "  4. Check logs: tail -f /tmp/dumpster-diver.log"
