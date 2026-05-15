#!/bin/bash
# ES event script: system-select
# Called by EmulationStation when cursor changes in system view.
# Args: $1=system_name $2=context
FIFO="/tmp/dumpster-diver.fifo"
[ -p "$FIFO" ] && timeout 1 bash -c 'echo "system-select|$1" > "$2"' _ "$1" "$FIFO" &
