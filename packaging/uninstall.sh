#!/bin/sh
case "$(uname -s)" in Darwin) DEST="/Library/OFX/Plugins" ;; Linux) DEST="/usr/OFX/Plugins" ;; *) exit 1 ;; esac
sudo rm -rf "$DEST/PlateMask.ofx.bundle" && echo "Removed $DEST/PlateMask.ofx.bundle (track data in ~/Library/Application Support/PlateMask is kept)"
