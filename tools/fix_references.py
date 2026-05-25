#!/usr/bin/env python3
"""Fix consecutive '> 参考：' blockquote lines that render as one paragraph.

Converts:
  > 参考：line1
  > 参考：line2

To:
  参考：
  - line1
  - line2
"""

import os, re

DOCS = os.path.join(os.path.dirname(__file__), "..", "document")

def fix_file(path: str) -> bool:
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    # Match 2+ consecutive '> 参考：...' lines
    pattern = re.compile(
        r"((?:^> 参考：.*\n){2,})",
        re.MULTILINE,
    )

    def replace_block(m):
        block = m.group(1)
        lines = block.strip().split("\n")
        items = []
        for line in lines:
            text = line.lstrip("> ").removeprefix("参考：").strip()
            items.append(f"- {text}")
        return "参考：\n\n" + "\n".join(items) + "\n"

    new = pattern.sub(replace_block, content)
    if new != content:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
        return True
    return False


changed = 0
total = 0
for root, dirs, files in os.walk(DOCS):
    for fname in files:
        if not fname.endswith(".md") or fname == "index.md":
            continue
        fpath = os.path.join(root, fname)
        total += 1
        if fix_file(fpath):
            changed += 1
            rel = os.path.relpath(fpath, DOCS)
            print(f"  FIX  {rel}")

print(f"\n{changed}/{total} files fixed")
