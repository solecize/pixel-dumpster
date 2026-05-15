#!/bin/bash
# ES event script: game-start
# Called by EmulationStation when a game is launched.
# Args: $1=rom_path $2=basename
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-start|$1|$2" > "$3"' _ "$1" "$2" "$FIFO" &
