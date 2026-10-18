#!/bin/sh
# Build and install the fix layer for the current user. It is only loaded by programs that
# set VK_INSTANCE_LAYERS=VK_LAYER_SCFIX_hide_nvx (i.e. Star Citizen via its Lutris config).
set -e
cd "$(dirname "$0")"
DEST=${DEST:-${XDG_DATA_HOME:-$HOME/.local/share}/starcitizen-nobara-fix}

./build.sh
mkdir -p "$DEST"
install -m755 build/libVkLayer_scfix.so "$DEST/"
install -m644 build/VkLayer_SCFIX.json "$DEST/"

cat <<EOF

Installed to $DEST

Add these to the Star Citizen game in Lutris
(Configure -> System options -> Environment variables):

    VK_ADD_LAYER_PATH   $DEST
    VK_INSTANCE_LAYERS  VK_LAYER_SCFIX_hide_nvx
    SCFIX_HIDE          VK_NV_raw_access_chains

then fully close the RSI Launcher and start it again from Lutris.
EOF
