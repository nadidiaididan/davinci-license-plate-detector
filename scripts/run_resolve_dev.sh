#!/bin/sh
# Launch DaVinci Resolve with the freshly built plugin bundle (no sudo install needed).
# The plugin binary is only loaded at startup, so a running Resolve is asked to quit first.
cd "$(dirname "$0")/.." || exit 1
if pgrep -x Resolve >/dev/null; then
  echo "Resolve is running - asking it to quit (answer the save prompt if one appears)..."
  osascript -e 'tell application "DaVinci Resolve" to quit' >/dev/null 2>&1 &
  i=0
  while pgrep -x Resolve >/dev/null; do
    i=$((i+1)); [ $i -gt 150 ] && { echo "Resolve did not quit; quit it manually and run this again."; exit 1; }
    sleep 2
  done
fi
open --env OFX_PLUGIN_PATH="$PWD/build" -a "DaVinci Resolve" && echo "Resolve launched with $PWD/build on the OFX path"
