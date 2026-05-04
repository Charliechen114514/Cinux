# ELF 格式解析与进程标识

> 标签：ELF, PID, fork, execve, x86-64
> 前置：tag 033 GUI 桌面环境

## 前言

说实话，到 tag 033 为止，我们的内核虽然有了 GUI 桌面，但进程管理这一块还停留在"单任务"阶段——所有东西都在内核线程里跑，用户态的 shell 也只是一个嵌入内核的二进制 blob。说实话这套方案在真正的多任务操作系统面前实在站不住脚。如果你跟我一样，想看到点击 Shell 图标后弹出一个真正独立的用户进程，那接下来这两个 tag 就是质变的时刻——我们要给内核加上 fork、execve、waitpid 这些 UNIX 进程原语。

这一篇我们先搭地基：ELF 可执行文件格式的解析，以及进程 ID 的分配管理。别小看这两个东西——execve 从文件系统读取 ELF 并加载到内存，每一步的偏移计算都有坑；PID 分配看似简单，但回收复用的策略直接影响到系统能跑多少进程。

## 环境说明

我们依然在 QEMU 里跑 Cinux，工具链是 GCC/G++ + CMake。Shell 从 ext2 文件系统加载（/bin/sh），构建脚本自动把编译好的 shell ELF 复制到 ext2 镜像。

## ELF64 格式——execve 的入口券

先聊聊 ELF。如果你之前做过用户态 C 编程，应该见过 ELF——`file a.out` 输出的 "ELF 64-bit LSB executable" 就是它。ELF 文件开头 64 字节是文件头（Elf64_Ehdr），里面包含魔数、目标架构、文件类型、入口点地址、program header table 的位置等信息。紧接着 program header table 描述了需要加载到内存的段——每个 PT_LOAD 段告诉内核"从文件的这个偏移读这么多字节，放到内存的这个虚拟地址"。

Cinux 的 ELF 解析放在独立的命名空间 `cinux::proc::elf` 里，一个 hpp 一个 cpp，干净利落。文件头和程序头都是 packed struct，配合 static_assert 在编译期保证大小正确——Elf64_Ehdr 必须是 64 字节，Elf64_Phdr 必须是 56 字节。如果你在某个奇怪的平台编译，static_assert 会立即告诉你布局不对。

验证函数 `validate_elf_header()` 依次检查 8 个条件：总大小至少能容纳文件头、魔数是 0x7F 'E' 'L' 'F'、class 是 64-bit、data encoding 是 little-endian、machine 是 x86-64（EM_X86_64 = 62）、type 是 executable、program header offset 没越界、entry size 是 56 字节、至少有一个 program header。任一失败立即返回对应的错误码，调用者知道具体哪里不对。

这里有个细节值得展开——魔数验证用的是移位组装而不是 memcmp。e_ident[0..3] 在内存中是 0x7F, 0x45, 0x4C, 0x46，小端系统上组装成 uint32_t 就是 0x464C457F。这是 ELF 标准定义的固定值，用整数比较避免了字符串函数的依赖。

跟 xv6 对比一下：xv6 也有类似的 ELF 头验证（在 exec.c 中），但它直接嵌入在 exec 函数里，没有独立模块。xv6 的结构体定义在 param.h 和 elf.h 中，和 Cinux 的 elf_types.hpp 类似但更简陋——xv6 不检查 data encoding 和 phentsize，因为它只支持一种平台。Cinux 的独立命名空间方案为将来支持 PIE（position-independent executable）和动态链接预留了扩展空间。

Linux 的 ELF 加载路径就复杂多了——load_elf_binary() 处理 PT_INTERP（动态链接器）、PT_GNU_STACK（栈可执行标志）、PT_NOTE（ABI 信息）等十几种段类型，还要处理 setuid/setgid 权限变化。Cinux 只关心 PT_LOAD，这是教学内核的合理简化。

## PID 分配器——进程的身份证号

每个进程需要一个唯一的数字标识（PID），从 1 开始递增。Cinux 的 PidAllocator 管理范围是 1 到 256，用 bool 数组记录每个 PID 是否在使用中。

分配策略是 hint-based 线性扫描：记住上次分配的位置（next_hint_），下次从那里继续往后找，找到第一个空闲的 PID。这样做的好处是分配是 O(n) 最坏情况但通常很快——大部分时候 hint 附近就有空闲 PID。释放时如果 PID 比 hint 小，就把 hint 拉回来，保证低号 PID 优先复用。

这和 Linux 的 pidr_alloc 是类似的思路，但 Linux 用的是 IDR（radix tree + bitmap），能在 O(1) 时间内分配和查找。Linux 的 PID 上限默认是 32768，可通过 /proc/sys/kernel/pid_max 调到 2^22。xv6 更简单——它用固定数组 proc[NPROC]（64 个槽位），进程的索引就是 PID，没有独立的分配器。SerenityOS 用位图加顺序扫描，和 Cinux 最接近。

Cinux 选择 256 的上限对教学内核绰绰有余。如果你以后想跑更多进程，把 PID_MAX 改成 4096 就行——bool 数组只增加几 KB 内存。

## 把它们串起来

有了 ELF 解析和 PID 分配器，接下来就是 fork 和 execve 的事了。execve 的完整流程是：VFS 路径解析 -> inode 查找 -> 读取并验证 ELF header -> 读取 program headers -> 清除旧的用户地址空间 -> 逐页加载 PT_LOAD 段 -> 设置入口点。每一步都可能失败，每一步失败都要返回正确的错误码。

PID 分配器在 fork 时被调用：`int child_pid = pid_alloc.alloc()`，分配成功后赋给子进程的 pid 字段。子进程退出后 waitpid 调用 `pid_alloc.free(pid)` 回收。这个 alloc/free 配对必须严格遵守——忘记 free 会导致 PID 泄漏，最终耗尽池子。

到这里地基已经打好。下一篇我们直接啃最硬的骨头——fork 的 CoW 页表和"返回两次"的机制。

## 参考资料
- ELF-64 Object File Format Specification: https://uclibc.org/docs/elf-64-gen.pdf
- OSDev Wiki ELF: https://wiki.osdev.org/ELF
- Linux PID 分配 (kernel/pid.c): https://github.com/torvalds/linux/blob/master/kernel/pid.c
- xv6 exec.c: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/exec.c
- Intel SDM Vol.3A Section 4.7 "Page-Table Entries" — Available bits for OS use
