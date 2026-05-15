#!/bin/bash
# Enhanced RetroPie Installation Script with ES Feature Detection
# Installs dumpster-diver daemon with automatic ES version detection

set -e

INSTALL_DIR="/home/pi/dumpster-diver"
FIFO_PATH="/tmp/dumpster-diver.fifo"
ES_SCRIPTS_DIR="/opt/retropie/configs/all/emulationstation/scripts"
RUNCOMMAND_DIR="/opt/retropie/configs/all"
AUTOSTART="/opt/retropie/configs/all/autostart.sh"

echo "=== Pixel-Dumpster Marquee Daemon Installer ==="
echo ""

# Step 1: Detect ES features
echo "[1/7] Detecting EmulationStation capabilities..."
if [ ! -f "./detect-es-features.sh" ]; then
    echo "ERROR: detect-es-features.sh not found in current directory"
    exit 1
fi

chmod +x ./detect-es-features.sh
FEATURES=$(./detect-es-features.sh)
ES_VERSION=$(echo "$FEATURES" | grep -o '"es_version": *"[^"]*"' | sed 's/.*: *"\(.*\)".*/\1/')
SCRIPTING=$(echo "$FEATURES" | grep -o '"scripting_support": *[^,}]*' | sed 's/.*: *//')
RUNCOMMAND=$(echo "$FEATURES" | grep -o '"runcommand_support": *[^,}]*' | sed 's/.*: *//')
LOG_FILE=$(echo "$FEATURES" | grep -o '"log_file": *"[^"]*"' | sed 's/.*: *"\(.*\)".*/\1/')

echo "  ✓ ES Version: $ES_VERSION"
if [ "$SCRIPTING" = "true" ]; then
    echo "  ✓ ES Scripting: Supported (browsing + launch events)"
else
    echo "  ✗ ES Scripting: Not supported (ES $ES_VERSION limitation)"
fi

if [ "$RUNCOMMAND" = "true" ]; then
    echo "  ✓ Runcommand: Supported (launch events)"
else
    echo "  ✗ Runcommand: Not available"
fi

if [ "$LOG_FILE" != "none" ]; then
    echo "  ✓ Log monitoring: Available ($LOG_FILE)"
fi
echo ""

# Step 2: Create installation directory
echo "[2/7] Creating installation directory..."
mkdir -p "$INSTALL_DIR"

# Step 3: Build daemon
echo "[3/7] Building daemon..."
if [ ! -f "dumpster-diver.c" ]; then
    echo "ERROR: dumpster-diver.c not found"
    exit 1
fi

make clean >/dev/null 2>&1 || true
if ! make dumpster-diver; then
    echo "ERROR: Failed to build daemon"
    exit 1
fi
echo "  ✓ Built successfully"

# Step 4: Install daemon binary
echo "[4/7] Installing daemon..."
cp dumpster-diver "$INSTALL_DIR/"
cp detect-es-features.sh "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/dumpster-diver"
chmod +x "$INSTALL_DIR/detect-es-features.sh"
echo "  ✓ Installed to $INSTALL_DIR"

# Step 5: Create configuration
echo "[5/7] Creating configuration..."
cat > "$INSTALL_DIR/config.json" <<EOF
{
  "device": {
    "host": "192.168.1.154",
    "port": 8088
  },
  "transport": "wifi",
  "marquee": {
    "device_prefix": "marquees",
    "auto_upload": true,
    "directories": [
      {
        "path": "/home/pi/marquee-custom",
        "priority": 1,
        "enabled": true
      },
      {
        "path": "/home/pi/PieMarquee2/marquee",
        "priority": 2,
        "enabled": true
      }
    ]
  },
  "fifo": "$FIFO_PATH",
  "es": {
    "roms_path": "/home/pi/RetroPie/roms",
    "gamelists_path": "/home/pi/.emulationstation/gamelists"
  },
  "events": {
    "game_select": $([ "$SCRIPTING" = "true" ] && echo "true" || echo "false"),
    "game_launch": true,
    "game_end": true,
    "system_select": $([ "$SCRIPTING" = "true" ] && echo "true" || echo "false")
  }
}
EOF
echo "  ✓ Config created"

# Step 6: Install event hooks
echo "[6/7] Installing event hooks..."

# Create custom marquee directory
mkdir -p /home/pi/marquee-custom/arcade
mkdir -p /home/pi/marquee-custom/system

# Install ES scripts if supported
if [ "$SCRIPTING" = "true" ]; then
    for event in game-select system-select game-start game-end; do
        mkdir -p "$ES_SCRIPTS_DIR/$event"
        
        # Create appropriate script
        if [ "$event" = "game-select" ]; then
            cat > "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh" <<'SCRIPT'
#!/bin/bash
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-select|$1|$2|$3" > "$4"' _ "$1" "$2" "$3" "$FIFO" &
SCRIPT
        elif [ "$event" = "system-select" ]; then
            cat > "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh" <<'SCRIPT'
#!/bin/bash
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "system-select|$1" > "$2"' _ "$1" "$FIFO" &
SCRIPT
        elif [ "$event" = "game-start" ]; then
            cat > "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh" <<'SCRIPT'
#!/bin/bash
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-start|$1|$2|$3" > "$4"' _ "$1" "$2" "$3" "$FIFO" &
SCRIPT
        elif [ "$event" = "game-end" ]; then
            cat > "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh" <<'SCRIPT'
#!/bin/bash
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-end|$1|$2|$3" > "$4"' _ "$1" "$2" "$3" "$FIFO" &
SCRIPT
        fi
        
        chmod +x "$ES_SCRIPTS_DIR/$event/dumpster-diver.sh"
    done
    echo "  ✓ ES event scripts installed"
fi

# Install runcommand hooks (always available)
if [ "$RUNCOMMAND" = "true" ]; then
    # Append to existing runcommand-onstart.sh if it exists
    if [ -f "$RUNCOMMAND_DIR/runcommand-onstart.sh" ]; then
        if ! grep -q "dumpster-diver.fifo" "$RUNCOMMAND_DIR/runcommand-onstart.sh"; then
            cat >> "$RUNCOMMAND_DIR/runcommand-onstart.sh" <<'HOOK'

# dumpster-diver marquee daemon
# runcommand args: $1=system, $2=emulator, $3=rom_path, $4=command
if [ -p "/tmp/dumpster-diver.fifo" ]; then
    echo "game-start|$1|$3|" > /tmp/dumpster-diver.fifo &
fi
HOOK
        fi
    fi
    
    # Append to runcommand-onend.sh
    if [ -f "$RUNCOMMAND_DIR/runcommand-onend.sh" ]; then
        if ! grep -q "dumpster-diver.fifo" "$RUNCOMMAND_DIR/runcommand-onend.sh"; then
            cat >> "$RUNCOMMAND_DIR/runcommand-onend.sh" <<'HOOK'

# dumpster-diver marquee daemon
# runcommand args: $1=system, $2=emulator, $3=rom_path, $4=command
if [ -p "/tmp/dumpster-diver.fifo" ]; then
    echo "game-end|$1|$3|" > /tmp/dumpster-diver.fifo &
fi
HOOK
        fi
    fi
    echo "  ✓ Runcommand hooks installed"
fi

# Step 7: Configure autostart
echo "[7/7] Configuring autostart..."
if [ -f "$AUTOSTART" ]; then
    # Remove old entries
    sed -i '/dumpster-diver/d' "$AUTOSTART"
fi

# Add new autostart entry
cat >> "$AUTOSTART" <<EOF

# dumpster-diver marquee daemon
(
    cd "$INSTALL_DIR"
    ./dumpster-diver --config config.json --verbose > /tmp/dd.log 2>&1 &
)
EOF

echo "  ✓ Autostart configured"
echo ""

# Summary
echo "=== Installation Complete ==="
echo ""
echo "Detected Features:"
if [ "$SCRIPTING" = "true" ]; then
    echo "  ✓ Real-time browsing marquees (game/system selection)"
else
    echo "  ✗ Browsing marquees (ES $ES_VERSION doesn't support scripting)"
    echo "    → Upgrade to ES 2.11+ for full browsing support"
fi
echo "  ✓ Game launch/exit marquees"
echo ""
echo "The daemon will automatically:"
echo "  • Start on boot"
echo "  • Discover pixel-dumpster devices on your network"
echo "  • Update marquees based on available events"
echo ""
echo "Custom marquees: /home/pi/marquee-custom/"
echo "Daemon log: /tmp/dd.log"
echo ""
echo "Start the daemon now:"
echo "  cd $INSTALL_DIR && ./dumpster-diver --config config.json --verbose &"
echo ""
echo "Or reboot to start automatically"
