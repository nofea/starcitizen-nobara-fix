# Star Citizen on Nobara: fix for "Failed to create Vulkan logical device"

A small Vulkan layer that gets Star Citizen past a startup crash on NVIDIA
under Proton/umu (Lutris), plus the diagnostic tools used to find the cause.

## Symptom

The RSI Launcher shows **GAME IS RUNNING**, but no game window ever appears.
`StarCitizen/LIVE/Game.log` ends with:

```
[VK_ERROR] terminator_CreateDevice: Failed in ICD .../libGLX_nvidia.so.0 vkCreateDevice call
Fatal Error Thrown: [...evice/Vulkan/GPUDeviceVulkan.cpp:825 createDevice] (0xFFFFFFFD)
  ... Failed to create Vulkan logical device.
Cloud Imperium Games public crash handler taking over...
```

The process stays alive in the crash handler, so the launcher never notices it died.

## Tested on

| | |
|---|---|
| Distro | Nobara (Fedora 44), KDE Plasma, Wayland |
| GPU | NVIDIA RTX 3070 Ti (+ Intel iGPU) |
| Driver | NVIDIA 595.104.02 (open kernel module) |
| Launcher | Lutris → umu-run → UMU-Proton-10.0-4 (GE-Proton11-9 fails the same way) |
| Game | Star Citizen LIVE 4.10.2 |

## Root cause

1. Star Citizen enables `VK_NVX_binary_import` and `VK_NVX_image_view_handle`
   (used for DLSS) when creating its Vulkan device.
2. With those extensions, the NVIDIA driver initialises **CUDA** (`libcuda`) for
   interop. libcuda reserves ~4 GiB of address space within a window that starts at
   `0x200000000` (8 GiB) and ends at ~1 TiB.
3. Inside the Wine process, that whole window is already reserved (~1016 GiB taken,
   not a single free byte). libcuda reads `/proc/self/maps`, finds no room, tears its UVM
   setup down, and `vkCreateDevice` returns `VK_ERROR_INITIALIZATION_FAILED`.
4. DXVK/vkd3d-proton retry without the NVX extensions, but Star Citizen's own
   renderer does not, so the game dies.

The identical request succeeds when replayed from a normal Linux process, natively or
inside the Steam Runtime container, which is what isolated the cause to the Wine
process's address space.

### Proton's existing workaround doesn't help (and corrupts libcuda)

protonfixes ships a Star Citizen fix (`gamefixes-umu/umu-starcitizen.py` →
`util.patch_libcuda()`) that copies libcuda to `/tmp`, patches it, and preloads it.
The patch converts the whole library to a **hex string** and runs
`replace('000000f8ff000000', '000000f8ffff0000')`, meant to raise the limit
`0xfff8000000` → `0xfffff8000000`. With libcuda 595.104.02:

- The intended constant (`movabs rax, 0xfff8000000` at `0x300e93`) is changed, but device
  creation still fails, so on this driver the limit is evidently enforced elsewhere too.
- Because the text search isn't aligned to whole bytes, it also matches half a byte off and
  rewrites four `jg rel32` instructions (`0f 8f f0 00 00 00` → `0f 8f ff f0 00 00`) at
  `0xc0f772`, `0xe0384a`, `0xe1156a`, `0xe337ea`, sending those jumps ~61 KiB into
  unrelated code.

## The fix

`layer/scfix_layer.c` builds `VK_LAYER_SCFIX_hide_nvx`, a Vulkan layer that removes
`VK_NVX_binary_import` and `VK_NVX_image_view_handle` from the extension list the game
sees, and strips them from `vkCreateDevice`. The driver then never starts CUDA, and the
device is created normally. It is only loaded by processes that request it in
`VK_INSTANCE_LAYERS`.

**Trade-off:** DLSS is unavailable (`Game.log`: `GPU: DLSS Support = Unavailable`).
Use FSR instead.

Extra extensions can be hidden with `SCFIX_HIDE` (comma-separated). The working setup
also hides `VK_NV_raw_access_chains`: without it, one run reached the window and then hit
`GPU Crash Vulkan (async): Nvidia - Device Timeout` after a few frames. A Proton deadlock
has been [traced to that extension](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/1409),
and DXVK devices inside the game process enable it. With it hidden, the game reached the
main menu. One run each way, so treat that part as likely rather than proven.

## Install

Requirements: `gcc`, `python3`, and either the Vulkan headers + registry
(`sudo dnf install vulkan-headers`) or [devenv](https://devenv.sh), which `build.sh`
uses automatically when the headers aren't installed.

```sh
git clone https://github.com/nofea/starcitizen-nobara-fix.git
cd starcitizen-nobara-fix
./install.sh
```

This builds into `build/` and installs the layer to
`~/.local/share/starcitizen-nobara-fix/`. Then, in Lutris, open Star Citizen →
**Configure → System options → Environment variables** and add:

| Key | Value |
|---|---|
| `VK_ADD_LAYER_PATH` | `/home/<you>/.local/share/starcitizen-nobara-fix` |
| `VK_INSTANCE_LAYERS` | `VK_LAYER_SCFIX_hide_nvx` |
| `SCFIX_HIDE` | `VK_NV_raw_access_chains` |

Fully close the RSI Launcher, then start it again from Lutris. The layers are compiled
with the host gcc and only need glibc ≥ 2.2.5, so they load inside the Steam Runtime
container.

To remove it: run `./uninstall.sh` and delete those three variables.

## Diagnostic tools (`diag/`)

Built by `./build.sh` into `build/`. They're useful if your crash looks similar but the
fix doesn't help.

- **`VK_LAYER_SCDIAG_diagnose`** (`scdiag_layer.c`) records every `vkCreateDevice` request
  (queues, extensions, raw bytes of each feature struct) plus the driver's result to
  `$SCDIAG_LOG`. With `SCDIAG_PAUSE=<dir>` it pauses before each NVIDIA device creation
  until `<dir>/go.<pid>` exists, so a tracer can attach.
- **`scdiag_replay LOG [--block N] [--bisect]`** rebuilds a recorded request in a normal
  process. `--bisect` retries a failing request with one extension, struct, or feature bit
  removed at a time.
- **`attach.sh <dir>/game.log`** watches for paused processes, attaches `strace` (system or
  devenv), and releases them.
- **`gen_structs.py`** generates struct sizes and `VkBool32` offsets from `vk.xml` for the two
  above.
- **`vkdevtest`** creates a device on the NVIDIA GPU with chosen extensions and features.

Example: record a launch and bisect it:

```sh
# Lutris env for the game:
#   VK_ADD_LAYER_PATH=<repo>/build  VK_INSTANCE_LAYERS=VK_LAYER_SCDIAG_diagnose
#   SCDIAG_LOG=/tmp/scdiag/game.log  (optional) SCDIAG_PAUSE=/tmp/scdiag
diag/attach.sh /tmp/scdiag/game.log      # only if SCDIAG_PAUSE is set
build/scdiag_replay /tmp/scdiag/game.log --bisect
```

Logs and traces contain local paths and process details. Review them before sharing.

## License

MIT, see [LICENSE](LICENSE).
