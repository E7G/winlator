#!/usr/bin/env python3
"""Second idempotent localization pass.

Handles UI literals that are concatenated with leading/trailing spaces and a few Compose strings
found by the expanded audit after the first pass. It imports the first-pass map so both passes stay
consistent while preserving whitespace around concatenated fragments.
"""
from pathlib import Path
import re

from apply_zh_hardcoded_ui import T, QUOTED, UI_MARKERS

ROOT = Path("app/src/main/java")
MARKERS = UI_MARKERS + ('showProgressDialog(',)

EXTRA = {
    'Create and manage Windows environments': '创建和管理 Windows 运行环境',
    'Components': '组件',
    'Wine, Proton, DXVK, VKD3D and runtimes': 'Wine、Proton、DXVK、VKD3D 与运行组件',
    'Selected Wine/Proton is not installed.': '所选 Wine/Proton 尚未安装。',
    'Moving...': '正在移动…',
    'Copying...': '正在复制…',
}
MAP = {**T, **EXTRA}


def replace_line(line: str) -> tuple[str, int]:
    if not any(marker in line for marker in MARKERS):
        return line, 0
    changed = 0

    def repl(match: re.Match) -> str:
        nonlocal changed
        raw = match.group(1)
        stripped = raw.strip()
        translated = MAP.get(stripped)
        if translated is None:
            return match.group(0)

        left_len = len(raw) - len(raw.lstrip())
        right_len = len(raw) - len(raw.rstrip())
        left = raw[:left_len]
        right = raw[len(raw) - right_len:] if right_len else ''
        value = left + translated + right
        value = value.replace('\\', '\\\\').replace('"', '\\"')
        changed += 1
        return f'"{value}"'

    return QUOTED.sub(repl, line), changed


def main() -> None:
    files_changed = 0
    replacements = 0
    for path in sorted(list(ROOT.rglob('*.java')) + list(ROOT.rglob('*.kt'))):
        text = path.read_text(encoding='utf-8', errors='ignore')
        output = []
        file_changes = 0
        for line in text.splitlines(keepends=True):
            line, count = replace_line(line)
            output.append(line)
            file_changes += count
        if file_changes:
            path.write_text(''.join(output), encoding='utf-8')
            files_changed += 1
            replacements += file_changes
            print(f'{path}: {file_changes}')
    print(f'Pass 2 localized {replacements} UI literals across {files_changed} files.')


if __name__ == '__main__':
    main()
