#!/usr/bin/env python3
from pathlib import Path
import re
import html

BASE_DIR = Path('app/src/main/res/values')
ZH_DIR = Path('app/src/main/res/values-zh-rCN')

string_re = re.compile(r'<string\s+name="([^"]+)"[^>]*>(.*?)</string>', re.S)
cjk_re = re.compile(r'[\u3400-\u4dbf\u4e00-\u9fff]')
quoted_re = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')

def read_dir(root):
    out = {}
    for path in sorted(root.glob('*.xml')):
        text = path.read_text(encoding='utf-8', errors='ignore')
        out.update({m.group(1): m.group(2).strip() for m in string_re.finditer(text)})
    return out

base = read_dir(BASE_DIR)
zh = read_dir(ZH_DIR)
missing = [k for k in base if k not in zh]
extra = [k for k in zh if k not in base]

print(f'Base strings: {len(base)}')
print(f'Chinese strings: {len(zh)}')
print(f'Missing Chinese strings: {len(missing)}')
for key in missing:
    value = re.sub(r'\s+', ' ', html.unescape(base[key])).strip()
    print(f'MISSING\t{key}\t{value}')

print(f'Obsolete/extra Chinese strings: {len(extra)}')
for key in extra:
    print(f'EXTRA\t{key}')

# Product names, APIs and low-level graphics/runtime identifiers are intentionally kept unchanged.
allowed_literals = {
    'Winlator', 'Winlator CMOD', 'YouTube', 'FPS', 'GPU', 'CPU', 'RAM', 'HUD',
    'Wine', 'Proton', 'DXVK', 'VKD3D', 'Box64', 'Vulkan', 'Turnip', 'ReShade', 'DirectInput',
    'FEXCore', 'SteamGridDB', 'DisplayX', 'ALSA', 'PulseAudio',
    'Drive D:', 'Drive C', '/storage/emulated/0/Download', 'FolderName', 'default_version',
    '-force-gfx-direct', '-force-d3d11-singlethreaded', '-force-dx9', '-force-d3d9', '-force-d3d11',
    '--force-gfx-direct', '--force-d3d11-singlethreaded', '--force-dx9', '--force-d3d9', '--force-d3d11',
}

def decode_literal(val):
    try:
        return bytes(val, 'utf-8').decode('unicode_escape') if '\\' in val and all(ord(c) < 128 for c in val) else val
    except Exception:
        return val


def looks_unlocalized(val):
    val = decode_literal(val).strip()
    if not val or val in allowed_literals:
        return False
    if cjk_re.search(val):
        return False
    if val.startswith(('http://', 'https://', '/', '@', '${')):
        return False
    if re.fullmatch(r'[\d\s.,:+%xX_()/\\-]+', val):
        return False
    return bool(re.search(r'[A-Za-z]{3,}', val))

attrs = ('text', 'hint', 'title', 'summary', 'contentDescription')
attr_re = re.compile(r'android:(?:' + '|'.join(attrs) + r')="([^"@?][^"]*)"')
xml_hits = []
for root in [Path('app/src/main/res/layout'), Path('app/src/main/res/layout-land'), Path('app/src/main/res/menu'), Path('app/src/main/res/xml')]:
    if not root.exists():
        continue
    for p in root.rglob('*.xml'):
        txt = p.read_text(encoding='utf-8', errors='ignore')
        for i, line in enumerate(txt.splitlines(), 1):
            for m in attr_re.finditer(line):
                val = m.group(1).strip()
                if looks_unlocalized(val):
                    xml_hits.append((str(p), i, val))

print(f'Unlocalized XML English candidates: {len(xml_hits)}')
for p, line, val in xml_hits:
    print(f'HARDCODED_XML\t{p}:{line}\t{val}')

# Java UI-string audit. Include dialog buttons and Toasts in addition to setters.
java_hits = []
java_context = re.compile(
    r'(?:\.(?:setText|setHint|setTitle|setMessage|setContentDescription|setPositiveButton|setNegativeButton|setNeutralButton)\s*\('
    r'|Toast\.makeText\s*\()'
)
for p in Path('app/src/main/java').rglob('*.java'):
    lines = p.read_text(encoding='utf-8', errors='ignore').splitlines()
    for i, line in enumerate(lines, 1):
        if not java_context.search(line):
            continue
        for m in quoted_re.finditer(line):
            val = m.group(1).strip()
            if looks_unlocalized(val):
                java_hits.append((str(p), i, val))
print(f'Hard-coded Java UI English candidates: {len(java_hits)}')
for p, line, val in java_hits:
    print(f'HARDCODED_JAVA\t{p}:{line}\t{val}')

# Jetpack Compose / Kotlin audit. Previous checks missed the new 4.0 Compose UI entirely.
kotlin_hits = []
kotlin_ui_markers = (
    'Text(', 'SectionTitle(', 'NavigationRow(', 'ToggleRow(', 'EditableValueCard(',
    'ActionButton(', 'StorageValue(', 'PreferenceCard(', 'SettingRow(', 'OptionRow(',
    'Toast.makeText(', '.setTitle(', '.setMessage(', '.setPositiveButton(', '.setNegativeButton(',
    'contentDescription =', 'label =', 'placeholder =', 'supportingText =', 'headlineContent =',
)
for p in Path('app/src/main/java').rglob('*.kt'):
    lines = p.read_text(encoding='utf-8', errors='ignore').splitlines()
    for i, line in enumerate(lines, 1):
        if not any(marker in line for marker in kotlin_ui_markers):
            continue
        for m in quoted_re.finditer(line):
            val = m.group(1).strip()
            # Ignore obvious internal preference keys/IDs while still flagging visible one-word labels.
            if re.fullmatch(r'[a-z0-9_.-]+', val) and not any(marker in line for marker in ('Text(', 'SectionTitle(', 'contentDescription =')):
                continue
            if looks_unlocalized(val):
                kotlin_hits.append((str(p), i, val))

print(f'Hard-coded Kotlin/Compose UI English candidates: {len(kotlin_hits)}')
for p, line, val in kotlin_hits:
    print(f'HARDCODED_KOTLIN\t{p}:{line}\t{val}')

if missing or xml_hits or java_hits or kotlin_hits:
    raise SystemExit(1)
