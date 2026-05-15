#!/bin/bash
# ES event script: game-select
# Called by EmulationStation when cursor changes in gamelist view.
# Args: $1=system $2=rom_path $3=game_name $4=context
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "game-select|$1|$2|$3" > "$4"' _ "$1" "$2" "$3" "$FIFO" &
