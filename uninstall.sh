#!/bin/sh
# Remove the installed fix layer. Also remove the VK_ADD_LAYER_PATH / VK_INSTANCE_LAYERS /
# SCFIX_HIDE environment variables from the game's Lutris config.
set -e
DEST=${DEST:-${XDG_DATA_HOME:-$HOME/.local/share}/starcitizen-nobara-fix}
rm -f "$DEST/libVkLayer_scfix.so" "$DEST/VkLayer_SCFIX.json"
rmdir "$DEST" 2>/dev/null || true
echo "Removed $DEST. Remember to remove the layer variables from the Lutris game config."
