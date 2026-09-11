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
p.write_text(s.replace(needle, replacement))
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

python3 - "$ROOT_DIR" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])

def replace_once(path: Path, old: str, new: str):
    s = path.read_text()
    if new in s:
        return
    if old not in s:
        raise SystemExit(f"Patch anchor not found in {path}: {old[:100]!r}")
    path.write_text(s.replace(old, new, 1))

# Expose Vortek in the graphics-driver picker.
arrays = root / "app/src/main/res/values/arrays.xml"
replace_once(
    arrays,
    "        <item>Wrapper</item>\n    </string-array>\n    <string-array name=\"wrapper_graphics_driver_version_entries\">",
    "        <item>Wrapper</item>\n        <item>Vortek</item>\n    </string-array>\n    <string-array name=\"wrapper_graphics_driver_version_entries\">",
)

activity = root / "app/src/main/java/com/winlator/cmod/XServerDisplayActivity.java"
replace_once(
    activity,
    "import com.winlator.cmod.xenvironment.components.SysVSharedMemoryComponent;\n",
    "import com.winlator.cmod.xenvironment.components.SysVSharedMemoryComponent;\nimport com.winlator.cmod.xenvironment.components.VortekRendererComponent;\n",
)

# Start the host Vulkan server before launching Wine when Vortek is selected.
anchor = '''        environment.addComponent(\n                new XServerComponent(\n                        xServer,\n                        UnixSocketConfig.createSocket(rootPath, UnixSocketConfig.XSERVER_PATH)));\n\n        if (audioDriver.equals("alsa")) {'''
replacement = '''        environment.addComponent(\n                new XServerComponent(\n                        xServer,\n                        UnixSocketConfig.createSocket(rootPath, UnixSocketConfig.XSERVER_PATH)));\n\n        if ("vortek".equalsIgnoreCase(graphicsDriver)) {\n            VortekRendererComponent.Options vortekOptions = new VortekRendererComponent.Options();\n            environment.addComponent(new VortekRendererComponent(\n                    this,\n                    xServer,\n                    UnixSocketConfig.createSocket(rootPath, UnixSocketConfig.VORTEK_SERVER_PATH),\n                    vortekOptions));\n        }\n\n        if (audioDriver.equals("alsa")) {'''
replace_once(activity, anchor, replacement)

# Vortek ships its own guest Vulkan ICD. Do not force cmod's wrapper ICD when selected.
anchor = '''        envVars.put("VK_ICD_FILENAMES", imageFs.getShareDir() + "/vulkan/icd.d/wrapper_icd.aarch64.json");\n\n        File graphicsRuntimeMarker = new File(rootDir,'''
replacement = '''        if ("vortek".equalsIgnoreCase(graphicsDriver)) {\n            Log.d("GraphicsDriverExtraction", "Installing Vortek 2.1 guest Vulkan ICD");\n            TarCompressorUtils.extract(TarCompressorUtils.Type.ZSTD, this,\n                    "graphics_driver/vortek-2.1.tzst", rootDir);\n            // Let the Vulkan loader discover Vortek's ICD JSON from /usr/share/vulkan/icd.d.\n            envVars.remove("VK_ICD_FILENAMES");\n            extractOpenGLDriver(rootDir);\n            if (!isMesaGlVersionOverrideManual()) envVars.put("MESA_GL_VERSION_OVERRIDE", "3.3");\n            if (!vkbasaltConfig.isEmpty()) {\n                envVars.put("ENABLE_VKBASALT", "1");\n                envVars.put("VKBASALT_CONFIG", vkbasaltConfig);\n            }\n            return;\n        }\n\n        envVars.put("VK_ICD_FILENAMES", imageFs.getShareDir() + "/vulkan/icd.d/wrapper_icd.aarch64.json");\n\n        File graphicsRuntimeMarker = new File(rootDir,'''
replace_once(activity, anchor, replacement)

# Vortek currently uses the device/system Vulkan driver in this cmod integration.
# Keep its configuration picker honest instead of offering Turnip packages that are ignored.
dialog = root / "app/src/main/java/com/winlator/cmod/contentdialog/GraphicsDriverConfigDialog.java"
old = '''        for (String version : wrapperDefaultVersions) {\n            if (GPUInformation.isDriverSupported(version, context))\n                wrapperVersions.add(version);\n        }\n        \n        // Add installed versions from AdrenotoolsManager\n        AdrenotoolsManager adrenotoolsManager = new AdrenotoolsManager(context);\n        wrapperVersions.addAll(adrenotoolsManager.enumarateInstalledDrivers());'''
new = '''        for (String version : wrapperDefaultVersions) {\n            if ("vortek".equalsIgnoreCase(graphicsDriver)) {\n                if ("System".equalsIgnoreCase(version)) wrapperVersions.add(version);\n            }\n            else if (GPUInformation.isDriverSupported(version, context)) {\n                wrapperVersions.add(version);\n            }\n        }\n\n        // Vortek is backed by the Android system Vulkan driver for the A5xx path.\n        if (!"vortek".equalsIgnoreCase(graphicsDriver)) {\n            AdrenotoolsManager adrenotoolsManager = new AdrenotoolsManager(context);\n            wrapperVersions.addAll(adrenotoolsManager.enumarateInstalledDrivers());\n        }'''
replace_once(dialog, old, new)
PY

echo "Prepared Vortek 2.1 native server, guest ICD and cmod runtime wiring."
