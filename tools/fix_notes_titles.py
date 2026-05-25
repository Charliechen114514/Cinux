#!/usr/bin/env python3
"""Clean up notes sidebar titles — strip redundant prefixes."""

import os, re

DOCS = os.path.join(os.path.dirname(__file__), "..", "document", "notes")

NOTES_TITLES = {
    "001/mbr-boot.md": "MBR 引导扇区",
    "001/vesa-framebuffer.md": "VESA Framebuffer 调试",
    "002/1.md": "E820 内存探测",
    "002/2.md": "INT 13h 读取排查",
    "003/1.md": "Long Mode 启动",
    "004-A/1.md": "VESA GDB 验证",
    "004-A/2.md": "核心踩坑总结",
    "004-B/kernel_load_stack_collision.md": "内核加载踩栈",
    "004-B/verify-loading.md": "验证内核加载",
    "004-C/boot_info_param_corruption.md": "BootInfo 参数破坏",
    "004-C/bss_data_symbol_conflict.md": ".bss 符号冲突",
    "005/005-01-test-framework.md": "自研测试框架",
    "005/005-02-host-tests.md": "Host 端单元测试",
    "005/005-03-kernel-tests.md": "内核测试基础设施",
    "005/005-04-test-automation.md": "测试自动化",
    "005/005-05-test-summary.md": "测试基建总结",
    "005/005-06-kprintf-format.md": "kprintf 格式化",
    "005/005-07-format-algorithms.md": "格式化算法",
    "005/005-08-vscode-debug-setup.md": "VSCode 调试配置",
    "005/005-09-qemu-gdb-debug.md": "QEMU GDB 调试",
    "005/005-10-serial-debug.md": "串口调试",
    "005/005-11-debug-workflows.md": "调试工作流",
    "005/005-12-mistake-check.md": "页表映射调试",
    "006/006-01-linker-symbol-access.md": "链接器符号访问",
    "006/006-02-object-library-global-ctors-not-called.md": "全局构造函数未调用",
    "009/009-01-elf-loader-header-corruption.md": "ELF 头自毁",
    "012/012-01-sse-init-crash-o2.md": "SSE 未初始化崩溃",
    "019_proc_context/001_higher_half_fix.md": "Higher-Half 修复",
    "019_proc_context/002_thread_exit_crash.md": "线程退出崩溃",
    "020/001_time_slice_too_long.md": "时间片过长",
    "020/002_if_flag_lost_in_context_switch.md": "IF 标志丢失",
    "021/001_big_kernel_magic_check.md": "大内核魔数检查",
    "022/001_usermode_three_bugs.md": "Ring 3 转换排查",
    "022/002_sfmask_qemu_msr.md": "IA32_FMASK MSR 问题",
    "023/design_notes.md": "Syscall 设计笔记",
    "023/fpu_sse_debug_notes.md": "FPU/SSE 排查",
    "024/024-01-sysretq-ss-rpl.md": "SYSRETQ SS.RPL",
    "024/024-02-syscall-rbx-clobber.md": "SYSCALL RBX 覆写",
    "028b/syscall_gp_fault.md": "Syscall GP Fault",
    "028e/001_init_thread_refactor_mmio_collision.md": "Init 线程重构",
    "029/1.md": "GUI Canvas 排查",
    "030/gp_fault_stack_alignment.md": "#GP 栈对齐",
    "030/mouse_cursor_offset.md": "鼠标光标偏移",
    "035/execve_page_offset_overflow.md": "execve 页内偏移越界",
    "035/fork_cow_huge_page_filter.md": "Fork CoW Huge Page",
    "035/fork_frame_pointer_bug.md": "Fork 帧指针 Bug",
    "035/stack_guard_page_debug.md": "Stack Guard Page",
    "035/syscall_gs_msr_bug.md": "Syscall GS MSR 丢失",
    "2026-05-22_phase0a_refactor.md": "Phase 0-A 重构日志",
}


def set_frontmatter_title(filepath: str, title: str) -> bool:
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    has_fm = content.startswith("---\n")
    if has_fm:
        existing = re.search(r"^title:\s*['\"]?(.+?)['\"]?\s*$", content, re.MULTILINE)
        if existing and existing.group(1) == title:
            return False
        if existing:
            new = re.sub(r"^title:\s*['\"]?.*?['\"]?\s*$", f"title: {title}",
                         content, count=1, flags=re.MULTILINE)
        else:
            new = content.replace("---\n", f"---\ntitle: {title}\n", 1)
    else:
        new = f"---\ntitle: {title}\n---\n\n{content}"

    with open(filepath, "w", encoding="utf-8") as f:
        f.write(new)
    return True


changed = 0
for rel, title in NOTES_TITLES.items():
    fpath = os.path.join(DOCS, rel)
    if not os.path.exists(fpath):
        print(f"  MISS {rel}")
        continue
    if set_frontmatter_title(fpath, title):
        changed += 1
        print(f"  OK   {rel} → {title}")
    else:
        print(f"  SAME {rel}")

print(f"\n{changed}/{len(NOTES_TITLES)} updated")
