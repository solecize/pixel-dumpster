#!/bin/bash
# ES event script: game-end
# Called by EmulationStation when a game exits.
# Args: (none)
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-end" > "$1"' _ "$FIFO" &
