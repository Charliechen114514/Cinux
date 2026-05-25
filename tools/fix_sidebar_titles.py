#!/usr/bin/env python3
"""Batch-add concise frontmatter sidebar titles to all docs."""

import os, re, sys

# Module name → short Chinese title
MODULE_TITLES = {
    "000-env-toolchain": "环境搭建",
    "001-boot-real-mode": "实模式引导",
    "002-boot-gdt-protected": "GDT 与保护模式",
    "003-boot-long-mode": "Long Mode",
    "004-boot-load-mini-kernel-a": "内核加载 (A)",
    "004-boot-load-mini-kernel-b": "内核加载 (B)",
    "004-boot-load-mini-kernel-c": "内核加载 (C)",
    "005-mini-kernel-entry": "内核入口",
    "006-mini-kernel-pmm": "物理内存管理",
    "007-mini-kernel-intr": "中断处理",
    "008-load-large-kernel": "大内核加载",
    "009-large-kernel-entry": "大内核入口",
    "010-big-kernel-gdt-idt": "GDT/IDT 重构",
    "011-big-kernel-pic-irq": "PIC 与 IRQ",
    "012-driver-serial": "串口驱动",
    "013-driver-vga-fb": "VGA 帧缓冲",
    "014-driver-keyboard": "键盘驱动",
    "015-mm-pmm": "物理内存管理",
    "016-mm-vmm": "虚拟内存管理",
    "017-mm-heap": "堆管理",
    "018-mm-address-space": "地址空间",
    "019-proc-context": "进程上下文",
    "020-proc-scheduler": "进程调度",
    "021-proc-sync": "同步原语",
    "022-ring3-usermode": "用户态 (Ring 3)",
    "023-syscall": "系统调用",
    "024-shell": "Shell",
    "025-driver-ahci": "AHCI 驱动",
    "026-fs-ramdisk": "Ramdisk 文件系统",
    "027-fs-vfs": "VFS 虚拟文件系统",
    "028b-fs-ext2-write": "Ext2 写入",
    "028c-fs-cwd-stat": "CWD 与 Stat",
    "028d-sync-safety": "同步安全",
    "028e-activate-init-thread": "Init 线程",
    "028-fs-ext2": "Ext2 文件系统",
    "029-gui-canvas": "GUI Canvas",
    "030-gui-wm-basic": "窗口管理器",
    "031-gui-native-app": "原生应用",
    "032-gui-bitmap-icon": "位图与图标",
    "033-gui-desktop": "桌面环境",
    "034-process-fork-exec": "Fork 与 Exec",
    "035-multi-terminal": "多终端",
}

# Volume-specific suffixes for differentiation
VOLUME_LABELS = {
    "hands-on": "",
    "read-through": "",
    "tutorial": "",
}

def make_sidebar_title(filename: str, volume: str) -> str:
    """Generate a clean sidebar title from filename."""
    name = filename.removesuffix(".md")
    # Extract module and part number
    m = re.match(r"^(.+)-(\d+)$", name)
    if not m:
        return None
    module, part = m.group(1), m.group(2)
    title = MODULE_TITLES.get(module)
    if not title:
        return None
    return f"{module}-{part} · {title}"

def has_frontmatter(content: str) -> bool:
    return content.startswith("---\n") or content.startswith("---\r\n")

def get_frontmatter_title(content: str) -> str | None:
    if not has_frontmatter(content):
        return None
    m = re.search(r"^title:\s*['\"]?(.+?)['\"]?\s*$", content, re.MULTILINE)
    return m.group(1) if m else None

def set_frontmatter_title(filepath: str, title: str) -> bool:
    """Add or update frontmatter title. Returns True if changed."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    existing = get_frontmatter_title(content)

    if has_frontmatter(content):
        if existing:
            # Update existing title
            new_content = re.sub(
                r"^title:\s*['\"]?.*?['\"]?\s*$",
                f"title: {title}",
                content,
                count=1,
                flags=re.MULTILINE,
            )
        else:
            # Insert title after opening ---
            new_content = content.replace("---\n", f"---\ntitle: {title}\n", 1)
    else:
        # Add new frontmatter
        new_content = f"---\ntitle: {title}\n---\n\n{content}"

    if new_content != content:
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(new_content)
        return True
    return False

def main():
    docs_root = os.path.join(os.path.dirname(__file__), "..", "document")
    docs_root = os.path.abspath(docs_root)

    changed = 0
    total = 0

    for volume in ["hands-on", "read-through", "tutorial"]:
        vol_dir = os.path.join(docs_root, volume)
        if not os.path.isdir(vol_dir):
            continue
        for fname in sorted(os.listdir(vol_dir)):
            if not fname.endswith(".md") or fname == "index.md":
                continue
            total += 1
            title = make_sidebar_title(fname, volume)
            if not title:
                print(f"  SKIP {volume}/{fname} (no mapping)")
                continue
            fpath = os.path.join(vol_dir, fname)
            if set_frontmatter_title(fpath, title):
                changed += 1
                print(f"  OK   {volume}/{fname} → {title}")
            else:
                print(f"  SAME {volume}/{fname}")

    print(f"\n{changed}/{total} files updated")

if __name__ == "__main__":
    main()
