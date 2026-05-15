#!/bin/bash
# ES Feature Detection Script
# Detects EmulationStation version and available event methods

ES_BIN="/opt/retropie/supplementary/emulationstation/emulationstation"
ES_VERSION="unknown"
SCRIPTING_SUPPORT=false
RUNCOMMAND_SUPPORT=false
LOG_FILE="none"

# Detect ES version from binary strings (don't run ES, it tries to init graphics)
if [ -f "$ES_BIN" ]; then
    # Extract version from binary strings
    ES_VERSION=$(strings "$ES_BIN" 2>/dev/null | grep -E '^[0-9]+\.[0-9]+\.[0-9]+' | grep -v '255.255.255' | head -1)
    
    # If that didn't work, try to find version near "PROGRAM_VERSION_STRING"
    if [ -z "$ES_VERSION" ]; then
        ES_VERSION=$(strings "$ES_BIN" 2>/dev/null | grep -A1 "PROGRAM_VERSION_STRING" | tail -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+[a-z]*')
    fi
    
    # Check strings for common RetroPie version pattern
    if [ -z "$ES_VERSION" ]; then
        ES_VERSION=$(strings "$ES_BIN" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+rp' | head -1)
    fi
fi

# If still unknown, default to RetroPie standard
if [ -z "$ES_VERSION" ]; then
    ES_VERSION="2.9.3rp"
fi

# Test scripting support based on version
# ES 2.11+ has full scripting support
MAJOR=$(echo "$ES_VERSION" | grep -oP '^\d+' || echo "2")
MINOR=$(echo "$ES_VERSION" | grep -oP '^\d+\.\K\d+' || echo "9")

if [ "$MAJOR" -ge 3 ]; then
    SCRIPTING_SUPPORT=true
elif [ "$MAJOR" -eq 2 ] && [ "$MINOR" -ge 11 ]; then
    SCRIPTING_SUPPORT=true
else
    SCRIPTING_SUPPORT=false
fi

# Check runcommand support (exists in all RetroPie installations)
if [ -d "/opt/retropie/configs/all" ]; then
    RUNCOMMAND_SUPPORT=true
fi

# Find log file
for LOG_PATH in "/dev/shm/runcommand.log" "/tmp/emulationstation.log" "$HOME/.emulationstation/es_log.txt"; do
    if [ -f "$LOG_PATH" ]; then
        LOG_FILE="$LOG_PATH"
        break
    fi
done

# Build recommended methods array
RECOMMENDED_METHODS='["runcommand"]'
if [ "$SCRIPTING_SUPPORT" = true ]; then
    RECOMMENDED_METHODS='["scripting", "runcommand"]'
elif [ "$LOG_FILE" != "none" ]; then
    RECOMMENDED_METHODS='["runcommand", "log"]'
fi

# Output JSON
cat <<EOF
{
  "es_version": "$ES_VERSION",
  "scripting_support": $SCRIPTING_SUPPORT,
  "runcommand_support": $RUNCOMMAND_SUPPORT,
  "log_file": "$LOG_FILE",
  "recommended_methods": $RECOMMENDED_METHODS
}
EOF
