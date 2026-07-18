#!/usr/bin/env bash
# Rebuild pd-control on macOS, optionally flash firmware, and print enable steps.
# Run on a Mac with this branch checked out (cloud Linux cannot produce a .app).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${IDF_PORT:-/dev/cu.usbmodem101}"
DO_FLASH="${DO_FLASH:-1}"
DO_BUILD="${DO_BUILD:-1}"

echo "==> Repo: $ROOT"
echo "==> Branch: $(git -C "$ROOT" branch --show-current)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: this script must run on macOS (found $(uname -s))" >&2
  exit 1
fi

if [[ "$DO_BUILD" == "1" ]]; then
  echo "==> Building Pixel Dumpster Control (Tauri)"
  cd "$ROOT/pd-control"
  npm install
  npm run tauri build
  echo
  echo "Install the new app from the Tauri output under pd-control/src-tauri/target/release/bundle/"
  echo "(replace /Applications/Pixel Dumpster Control.app if that is how you install)."
fi

if [[ "$DO_FLASH" == "1" ]]; then
  echo "==> Flashing firmware on $PORT (same branch as the app)"
  cd "$ROOT"
  if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py not on PATH — source ESP-IDF export.sh first" >&2
    exit 1
  fi
  idf.py -p "$PORT" flash
fi

cat <<'EOF'

==> Enable + verify
1. Open the newly installed app
2. Content (or Device Setup) → check "Auto-quantize animations to 64 colors"
   Or serial: {"cmd":"set_playback","auto_quantize_palette":true,"save":true}
3. Stop, then play a sequence again
4. Confirm serial log: palette cache ready: …

EOF
