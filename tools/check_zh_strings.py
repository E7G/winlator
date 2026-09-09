#!/usr/bin/env python3
from pathlib import Path
import re
import html

BASE_DIR = Path('app/src/main/res/values')
ZH_DIR = Path('app/src/main/res/values-zh-rCN')

string_re = re.compile(r'<string\s+name="([^"]+)"[^>]*>(.*?)</string>', re.S)

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

# Technical/product literals that should not be translated.
allowed_literals = {
    'Winlator', 'Winlator CMOD', 'YouTube', 'FPS', 'GPU', 'CPU', 'ReShade', 'DirectInput',
    'Drive D:', '/storage/emulated/0/Download', 'FolderName', 'default_version',
    '-force-gfx-direct', '-force-d3d11-singlethreaded', '-force-dx9', '-force-d3d9', '-force-d3d11',
    '--force-gfx-direct', '--force-d3d11-singlethreaded', '--force-dx9', '--force-d3d9', '--force-d3d11',
}
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
                if val in allowed_literals:
                    continue
                if re.search(r'[A-Za-z]{3,}', val):
                    xml_hits.append((str(p), i, val))

print(f'Unlocalized XML English candidates: {len(xml_hits)}')
for p, line, val in xml_hits:
    print(f'HARDCODED_XML\t{p}:{line}\t{val}')

# Java UI-string audit. Limit to APIs that normally surface text to users.
java_hits = []
ui_call = re.compile(r'\.(?:setText|setHint|setTitle|setMessage|setContentDescription)\(\s*"([^"\\]*(?:\\.[^"\\]*)*)"\s*\)')
for p in Path('app/src/main/java').rglob('*.java'):
    txt = p.read_text(encoding='utf-8', errors='ignore')
    for i, line in enumerate(txt.splitlines(), 1):
        for m in ui_call.finditer(line):
            val = m.group(1).strip()
            if val and val not in allowed_literals and re.search(r'[A-Za-z]{3,}', val):
                java_hits.append((str(p), i, val))
print(f'Hard-coded Java UI English candidates: {len(java_hits)}')
for p, line, val in java_hits:
    print(f'HARDCODED_JAVA\t{p}:{line}\t{val}')

if missing or xml_hits or java_hits:
    raise SystemExit(1)
