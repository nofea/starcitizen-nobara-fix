#!/bin/sh
# Build the fix layer and the diagnostic tools into ./build.
#
# Vulkan headers and the registry (vk.xml) come from the system if installed
# (/usr/include/vulkan, /usr/share/vulkan/registry), otherwise from devenv (devenv.nix).
# Everything is compiled with the host gcc (not Nix's) so the layers load inside the
# Steam Runtime container that Proton/umu runs the game in.
set -e
cd "$(dirname "$0")"

if [ -f /usr/include/vulkan/vk_layer.h ] && [ -f /usr/share/vulkan/registry/vk.xml ]; then
    VKINC=/usr/include
    VKXML=/usr/share/vulkan/registry/vk.xml
else
    command -v devenv >/dev/null || { echo "need Vulkan headers (vulkan-headers) or devenv" >&2; exit 1; }
    VKINC=$(devenv shell -- bash -c 'echo $NIX_CFLAGS_COMPILE' 2>/dev/null | tr ' ' '\n' | grep -m1 'vulkan-headers.*/include$')
    VKXML="$VKINC/../share/vulkan/registry/vk.xml"
fi
CC=${CC:-/usr/bin/gcc}
CFLAGS="-O2 -g -Wall -I$VKINC"
LAYER_FLAGS="-shared -fPIC -fvisibility=hidden"

mkdir -p build
# fix layer
$CC $CFLAGS $LAYER_FLAGS -o build/libVkLayer_scfix.so layer/scfix_layer.c
cp layer/VkLayer_SCFIX.json build/
# diagnostics
python3 diag/gen_structs.py "$VKXML" "$VKINC/vulkan/vulkan_core.h" build/scdiag_structs.h
$CC $CFLAGS -Ibuild $LAYER_FLAGS -o build/libVkLayer_scdiag.so diag/scdiag_layer.c
cp diag/VkLayer_SCDIAG.json build/
$CC $CFLAGS -Ibuild -o build/scdiag_replay diag/scdiag_replay.c -l:libvulkan.so.1
$CC $CFLAGS -o build/vkdevtest diag/vkdevtest.c -l:libvulkan.so.1
echo "built into $(pwd)/build"
