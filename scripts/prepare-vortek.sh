#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_DIR="${RUNNER_TEMP:-/tmp}/winlator-vortek-upstream"
UPSTREAM_REPO="https://github.com/brunodev85/winlator-app.git"
UPSTREAM_REF="main"
CPP_DIR="$ROOT_DIR/app/src/main/cpp"
ASSET_DIR="$ROOT_DIR/app/src/main/assets/graphics_driver"

rm -rf "$UPSTREAM_DIR"
git clone --depth 1 --branch "$UPSTREAM_REF" "$UPSTREAM_REPO" "$UPSTREAM_DIR"

rm -rf "$CPP_DIR/vortekrenderer" "$CPP_DIR/winlator/include" "$CPP_DIR/vortek_support"
mkdir -p "$CPP_DIR/winlator" "$CPP_DIR/vortek_support" "$ASSET_DIR"

cp -a "$UPSTREAM_DIR/app/src/main/cpp/vortekrenderer" "$CPP_DIR/vortekrenderer"
cp -a "$UPSTREAM_DIR/app/src/main/cpp/winlator/include" "$CPP_DIR/winlator/include"
cp "$UPSTREAM_DIR/app/src/main/cpp/winlator/src/arrays.c" "$CPP_DIR/vortek_support/arrays.c"
cp "$UPSTREAM_DIR/app/src/main/cpp/winlator/src/ring_buffer.c" "$CPP_DIR/vortek_support/ring_buffer.c"
cp "$UPSTREAM_DIR/app/src/main/cpp/winlator/src/sysvshared_memory.c" "$CPP_DIR/vortek_support/sysvshared_memory.c"
cp "$UPSTREAM_DIR/app/src/main/assets/graphics_driver/vortek-2.1.tzst" "$ASSET_DIR/vortek-2.1.tzst"

# E7G/cmod uses a different Java package and a different adrenotools directory.
sed -i 's#Java_com_winlator_xenvironment_components_VortekRendererComponent_#Java_com_winlator_cmod_xenvironment_components_VortekRendererComponent_#g' \
  "$CPP_DIR/vortekrenderer/src/main.c"
sed -i 's#../libadrenotools/include#../adrenotools/include#g' \
  "$CPP_DIR/vortekrenderer/CMakeLists.txt"

python3 - "$CPP_DIR/vortekrenderer/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
needle = "            src/timeline_semaphore.c)"
replacement = "            src/timeline_semaphore.c\n            ../vortek_support/arrays.c\n            ../vortek_support/ring_buffer.c\n            ../vortek_support/sysvshared_memory.c)"
if needle not in s:
    raise SystemExit("Unexpected upstream Vortek CMake layout")
s = s.replace(needle, replacement)
# ASharedMemory_create is provided by libandroid.
p.write_text(s)
PY

# Add Vortek to E7G's existing native build without disturbing its renderer targets.
if ! grep -q 'add_subdirectory(vortekrenderer)' "$CPP_DIR/CMakeLists.txt"; then
  cat >> "$CPP_DIR/CMakeLists.txt" <<'EOF'

# Prepared by scripts/prepare-vortek.sh during CI/local builds.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vortekrenderer/CMakeLists.txt")
    add_subdirectory(vortekrenderer)
endif()
EOF
fi

echo "Prepared Vortek 2.1 native server + guest Vulkan runtime."
