#!/usr/bin/env python3
from pathlib import Path
import re
import html

BASE = Path('app/src/main/res/values/strings.xml')
ZH = Path('app/src/main/res/values-zh-rCN/strings.xml')

string_re = re.compile(r'<string\s+name="([^"]+)"[^>]*>(.*?)</string>', re.S)

def read_map(path):
    text = path.read_text(encoding='utf-8')
    return {m.group(1): m.group(2).strip() for m in string_re.finditer(text)}

base = read_map(BASE)
zh = read_map(ZH)
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

# Heuristic scan for obvious hard-coded, user-facing English in Android XML.
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
                if re.search(r'[A-Za-z]{3,}', val):
                    xml_hits.append((str(p), i, val))
print(f'Hard-coded XML English candidates: {len(xml_hits)}')
for p, line, val in xml_hits:
    print(f'HARDCODED_XML\t{p}:{line}\t{val}')

if missing or xml_hits:
    raise SystemExit(1)
