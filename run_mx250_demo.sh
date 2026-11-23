#!/bin/zsh
# Launch the interactive NVIDIA GeForce MX250 (NVK GP108) -> Intel Metal PRIME 3D Demo
# Press ESC or close the window to quit.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
PY_BIN="$HOME/miniconda3/bin/python3"
[ -x "$PY_BIN" ] || PY_BIN="python3"
"$PY_BIN" -c "
import ctypes
lib = ctypes.CDLL('$ROOT/build/libgp108_vkcube_macos.dylib')
lib.gp108_vkcube_run.argtypes = [ctypes.c_char_p, ctypes.c_double, ctypes.c_int, ctypes.c_char_p]
lib.gp108_vkcube_run.restype = ctypes.c_int
lib.gp108_vkcube_run(b'$ROOT/build/libvulkan_nvk.dylib', 0.0, 0, b'')
"
