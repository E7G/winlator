#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Keep localized display entry "自定义" from leaking into the stored screenSize value.
path = ROOT / "app/src/main/java/com/winlator/cmod/ui/container/ContainerEditorV2.kt"
text = path.read_text(encoding="utf-8")
old = '''                val shownScreen = if (customScreenSelected) "Custom"
                else screenEntries.firstOrNull { normalizeResolution(it).equals(s.screen, true) } ?: "Custom"
                SettingChoice("Screen Size", shownScreen, screenEntries) {
                    if (it.equals("Custom", true)) {
                        customScreenSelected = true
                    } else {
                        customScreenSelected = false
                        s.screen = normalizeResolution(it)
                    }
                }
                if (shownScreen == "Custom") {
'''
new = '''                val customEntry = screenEntries.firstOrNull {
                    it.equals("Custom", true) || it == "自定义"
                } ?: "Custom"
                val shownScreen = if (customScreenSelected) customEntry
                else screenEntries.firstOrNull { normalizeResolution(it).equals(s.screen, true) } ?: customEntry
                SettingChoice("Screen Size", shownScreen, screenEntries) {
                    if (it == customEntry) {
                        customScreenSelected = true
                    } else {
                        customScreenSelected = false
                        s.screen = normalizeResolution(it)
                    }
                }
                if (shownScreen == customEntry) {
'''
if new not in text:
    if old not in text:
        raise RuntimeError("ContainerEditorV2 custom-resolution anchor not found")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print("Patched localized custom-resolution handling")
else:
    print("Custom-resolution handling already patched")

# Extend the localization checker with explicit string-array coverage. These are display-only
# arrays; protocol/value arrays intentionally remain in the base resources.
checker = ROOT / "tools/check_zh_strings.py"
text = checker.read_text(encoding="utf-8")
anchor = "# Product names, APIs, MIME types and internal identifiers that are intentionally not translated.\n"
block = r'''# Display string-array audit. Value/protocol arrays are intentionally excluded.
import xml.etree.ElementTree as ET

def read_arrays(path):
    if not path.exists():
        return {}
    root = ET.parse(path).getroot()
    return {
        node.attrib.get('name', ''): [''.join(item.itertext()).strip() for item in node.findall('item')]
        for node in root.findall('string-array')
    }

base_arrays = read_arrays(BASE_DIR / 'arrays.xml')
zh_arrays = read_arrays(ZH_DIR / 'arrays.xml')
required_display_arrays = {
    'screen_size_entries', 'binding_type_entries', 'dxvk_max_device_memory_entries',
    'device_memory_entries', 'wincomponent_entries', 'desktop_theme_entries',
    'desktop_background_type_entries', 'startup_selection_entries', 'button_options',
    'touchscreenInputModesEntries', 'transformCapturedPointerEntries',
}
array_issues = []
for name in sorted(required_display_arrays):
    base_items = base_arrays.get(name)
    zh_items = zh_arrays.get(name)
    if base_items is None:
        array_issues.append((name, 'missing base array'))
    elif zh_items is None:
        array_issues.append((name, 'missing zh-rCN array'))
    elif len(base_items) != len(zh_items):
        array_issues.append((name, f'item count {len(zh_items)} != {len(base_items)}'))

print(f'Chinese display-array issues: {len(array_issues)}')
for name, reason in array_issues:
    print(f'ARRAY\t{name}\t{reason}')

'''
if block not in text:
    if anchor not in text:
        raise RuntimeError("localization checker anchor not found")
    text = text.replace(anchor, block + anchor, 1)

text = text.replace(
    "if missing or xml_hits or java_hits or kotlin_hits:\n    raise SystemExit(1)\n",
    "if missing or array_issues or xml_hits or java_hits or kotlin_hits:\n    raise SystemExit(1)\n"
)
checker.write_text(text, encoding="utf-8")
print("Extended string-array localization audit")
