#!/bin/bash
# test-fifo.sh — Send sample events to dumpster-diver FIFO for manual testing.
#
# Usage:
#   1. Start the daemon:  ./dumpster-diver --verbose --dry-run
#   2. Run this script:   ./test-fifo.sh
#
# The daemon should log each event and the resulting lookup chain.

FIFO="${1:-/tmp/dumpster-diver.fifo}"

if [ ! -p "$FIFO" ]; then
    echo "FIFO not found: $FIFO"
    echo "Start dumpster-diver first, or create it: mkfifo $FIFO"
    exit 1
fi

send() {
    echo "  -> $1"
    echo "$1" > "$FIFO"
    sleep 0.3
}

echo "Sending test events to $FIFO"
echo ""

send "system-select|arcade"
send "game-select|arcade|/home/pi/RetroPie/roms/arcade/pacman.zip|Pac-Man"
send "game-select|arcade|/home/pi/RetroPie/roms/arcade/galaga.zip|Galaga"
send "game-start|/home/pi/RetroPie/roms/arcade/galaga.zip|galaga"
send "game-end"
send "system-select|snes"
send "game-select|snes|/home/pi/RetroPie/roms/snes/smw.sfc|Super Mario World"

echo ""
echo "Done. Check daemon output for event processing."
