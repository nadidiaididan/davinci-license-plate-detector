#!/bin/sh
# Installs PlateMask.ofx.bundle into the system OpenFX folder (asks for your password) and clears
# the macOS quarantine flag that a downloaded, unsigned bundle carries. Restart DaVinci Resolve after.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BUNDLE="$HERE/PlateMask.ofx.bundle"
[ -d "$BUNDLE" ] || { echo "PlateMask.ofx.bundle not found next to this script"; exit 1; }
case "$(uname -s)" in
  Darwin) DEST="/Library/OFX/Plugins" ;;
  Linux)  DEST="/usr/OFX/Plugins" ;;
  *) echo "Unsupported OS"; exit 1 ;;
esac
echo "Installing to $DEST (administrator password required)..."
sudo mkdir -p "$DEST"
sudo rm -rf "$DEST/PlateMask.ofx.bundle"
sudo cp -R "$BUNDLE" "$DEST/"
if [ "$(uname -s)" = Darwin ]; then sudo xattr -dr com.apple.quarantine "$DEST/PlateMask.ofx.bundle" 2>/dev/null || true; fi
echo "Done. Restart DaVinci Resolve Studio; the effect is under OpenFX > Filters > PlateMask > License Plate Mask."
