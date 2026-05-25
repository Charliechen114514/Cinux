---
title: 034-process-fork-exec-1 · Fork 与 Exec
---

# ELF64 类型定义与 PID 分配器

## 概览

本篇讲解 tag 034 中最底层的两个模块：ELF64 格式解析器和 PID 分配器。它们是整个 fork/execve 体系的地基——execve 需要从磁盘读取 ELF 文件并解析其结构，而 fork 和 getpid 需要一个可靠的进程标识来源。看完本篇，你会清楚 Cinux 是怎么定义 ELF 类型、验证 ELF 头、以及管理 PID 生命周期的。

关键设计决策：ELF 类型放在独立的 `cinux::proc::elf` 命名空间中，和进程管理逻辑隔离；PID 分配器是一个 RAII 风格的类而非全局函数包装器。

## 架构图

```
execve() ──→ elf::validate_elf_header() ──→ ElfValidateResult
    │                                           │
    │──→ elf::Elf64_Ehdr (64 bytes)             │ Ok/BadMagic/BadClass...
    │──→ elf::Elf64_Phdr (56 bytes)
    │
fork() ──→ PidAllocator::alloc() ──→ pid (1..256)
    │          PidAllocator::free(pid)
    │
sys_getpid() ──→ Task::pid
sys_getppid() ──→ Task::ppid
```

## 代码精讲

### ELF 常量定义

elf_types.hpp 开头定义了一组编译期常量，覆盖 ELF header 中所有需要验证的字段值。

```cpp
namespace cinux::proc::elf {

constexpr uint32_t ELF_MAGIC = 0x464C457F;    // 0x7F 'E' 'L' 'F' 小端表示
constexpr uint8_t  ELF_CLASS_64 = 2;          // 64-bit ELF
constexpr uint8_t  ELF_DATA_LSB = 1;          // Little-endian
constexpr uint16_t ET_EXEC = 2;               // Executable
constexpr uint16_t EM_X86_64 = 62;            // x86-64 architecture
constexpr uint32_t PT_LOAD = 1;               // Loadable segment
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;  // Segment flags
```

ELF_MAGIC 的值 0x464C457F 看起来有点反直觉——这是因为小端存储中 e_ident[0..3] 的字节序是 0x7F, 'E'(0x45), 'L'(0x4C), 'F'(0x46)，组合成 uint32_t 就是 0x464C457F。验证函数中用移位组装 magic 来比对，而不是用 memcmp，这样避免了字符串函数的依赖。

### Elf64_Ehdr — 文件头结构

```cpp
struct Elf64_Ehdr {
    uint8_t  e_ident[16];    // ELF identification bytes
    uint16_t e_type;         // Object file type (ET_EXEC = 2)
    uint16_t e_machine;      // Target architecture (EM_X86_64 = 62)
    uint32_t e_version;
    uint64_t e_entry;        // Virtual entry point address
    uint64_t e_phoff;        // Program header table file offset
    uint64_t e_shoff;        // Section header table file offset
    uint32_t e_flags;
    uint16_t e_ehsize;       // ELF header size (64 bytes)
    uint16_t e_phentsize;    // Program header entry size (56 bytes)
    uint16_t e_phnum;        // Number of program header entries
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

static_assert(sizeof(Elf64_Ehdr) == 64);
```

所有字段都是标准 ELF64 规范中的定义。e_phoff 告诉我们 program header table 从文件的什么位置开始，e_phentsize 是每个 entry 的大小（必须是 56 字节），e_phnum 是 entry 的数量。e_entry 是入口点虚拟地址——execve 成功后 task->ctx.rip 会被设为这个值。

### Elf64_Phdr — 程序头结构

```cpp
struct Elf64_Phdr {
    uint32_t p_type;     // Segment type (PT_LOAD = 1)
    uint32_t p_flags;    // PF_R | PF_W | PF_X
    uint64_t p_offset;   // Segment file offset
    uint64_t p_vaddr;    // Target virtual address
    uint64_t p_paddr;    // Physical address (unused on x86-64)
    uint64_t p_filesz;   // Bytes to copy from file
    uint64_t p_memsz;    // Total memory size (filesz + BSS zero-fill)
    uint64_t p_align;    // Alignment
} __attribute__((packed));

static_assert(sizeof(Elf64_Phdr) == 56);
```

p_filesz 和 p_memsz 的区别很重要：p_filesz 是文件中实际有的数据量，p_memsz 是内存中需要分配的空间。差值部分（通常是 .bss 段）需要清零。execve 在加载时先给整页清零，再填入文件数据，这样 .bss 自然就是零。

### ElfValidateResult — 验证结果枚举

```cpp
enum class ElfValidateResult : int {
    Ok = 0,
    BadMagic,      // 0x7F 'E' 'L' 'F' 不匹配
    BadClass,      // 不是 64-bit
    BadEndian,     // 不是 little-endian
    BadMachine,    // 不是 x86-64
    BadType,       // 不是 executable
    BadPhoff,      // Program header offset 越界
    BadPhdrSize,   // Program header entry size 不是 56
    NoPhdrs,       // 没有 program header
};
```

使用 scoped enum 而不是裸 int 或全局常量，避免和 errno 混淆。每个值对应一个具体的验证失败原因，调用者可以根据返回值判断问题所在。

### validate_elf_header() 实现

```cpp
ElfValidateResult validate_elf_header(const Elf64_Ehdr* ehdr, uint64_t total_size) {
    if (total_size < sizeof(Elf64_Ehdr))
        return ElfValidateResult::BadMagic;

    uint32_t magic = static_cast<uint32_t>(ehdr->e_ident[0])
                   | (static_cast<uint32_t>(ehdr->e_ident[1]) << 8)
                   | (static_cast<uint32_t>(ehdr->e_ident[2]) << 16)
                   | (static_cast<uint32_t>(ehdr->e_ident[3]) << 24);
    if (magic != ELF_MAGIC)
        return ElfValidateResult::BadMagic;

    if (ehdr->e_ident[4] != ELF_CLASS_64)
        return ElfValidateResult::BadClass;
    if (ehdr->e_ident[5] != ELF_DATA_LSB)
        return ElfValidateResult::BadEndian;
    if (ehdr->e_machine != EM_X86_64)
        return ElfValidateResult::BadMachine;
    if (ehdr->e_type != ET_EXEC)
        return ElfValidateResult::BadType;
    if (ehdr->e_phoff > total_size)
        return ElfValidateResult::BadPhoff;
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
        return ElfValidateResult::BadPhdrSize;
    if (ehdr->e_phnum == 0)
        return ElfValidateResult::NoPhdrs;

    return ElfValidateResult::Ok;
}
```

这个函数按顺序检查 8 个条件，任一不满足立即返回。注意 magic 的组装方式：从 e_ident[0] 开始逐字节左移——这是小端系统的自然字节序。

### PidAllocator 类

```cpp
class PidAllocator {
public:
    static constexpr int PID_NONE = 0;
    static constexpr int PID_MAX  = 256;

    PidAllocator();
    int  alloc();
    void free(int pid);
    bool is_allocated(int pid) const;
    int  count() const;

private:
    bool in_use_[PID_MAX + 1];  // 索引 0 到 256
    int  next_hint_;            // 下次分配的起始位置
};
```

PID_MAX 设为 256 对教学内核绰绰有余。Linux 的默认上限是 32768（PID_MAX_DEFAULT），可通过 /proc/sys/kernel/pid_max 调到更大会。xv6 用固定数组 NPROC=64 管理，更简单但上限更低。

alloc() 的核心逻辑：从 next_hint_ 开始线性扫描 PID_MAX 个位置，取模回绕到 1（跳过 0）。找到第一个 !in_use_ 的 PID 就标记已使用、推进 hint、返回。扫描一圈全满则返回 PID_NONE。

```cpp
PidAllocator::PidAllocator() : next_hint_(1) {
    for (int i = 0; i <= PID_MAX; ++i)
        in_use_[i] = false;
}

int PidAllocator::alloc() {
    for (int i = 0; i < PID_MAX; ++i) {
        int candidate = next_hint_ + i;
        if (candidate > PID_MAX) candidate -= PID_MAX;
        if (candidate == 0) candidate = 1;
        if (!in_use_[candidate]) {
            in_use_[candidate] = true;
            next_hint_ = (candidate >= PID_MAX) ? 1 : candidate + 1;
            return candidate;
        }
    }
    return PID_NONE;
}
```

free() 在标记 in_use_[pid] = false 之后，如果 pid < next_hint_，就把 hint 拉回来。这保证了低号 PID 优先被复用，和 Linux 的行为一致——Linux 的 pid_nr 递增直到到达 pid_max 后回绕到 RESERVED_PIDS(300)，效果类似。

## 设计决策

### 决策：ELF 验证放在哪里

**问题**: ELF 验证逻辑应该和 execve 混在一起，还是独立成模块？
**本项目的做法**: 独立命名空间 `cinux::proc::elf`，自己的 hpp/cpp 文件。
**备选方案**: 把验证逻辑内联在 execve 函数里。
**为什么不选备选**: execve 已经很长了（验证 + 清除 + 加载 + 映射），混合验证会进一步增加认知负担。独立的模块也方便单元测试——可以不依赖整个 execve 链路就测试验证逻辑。
**扩展方向**: 未来如果要支持共享库（.so），需要在 elf 命名空间里添加 PT_DYNAMIC 解析和重定位逻辑，独立模块使扩展更清晰。

## 扩展方向

- 支持 ET_DYN 类型（位置无关可执行文件 PIE）——修改 validate_elf_header 接受 BadType 但允许 ET_DYN
- 实现 section header 解析（e_shoff + Elf64_Shdr），用于符号表和调试信息
- PID 分配器添加位图优化——用 uint64_t[4] 替代 bool[257]，一次检查 64 位
- 添加 PID 回收延迟（类似 Linux 的 PID_REUSE 延迟），防止新进程立即复用刚退出的 PID

## 参考资料
- ELF-64 Object File Format: https://uclibc.org/docs/elf-64-gen.pdf
- OSDev Wiki ELF: https://wiki.osdev.org/ELF
- Linux PID 分配 (kernel/pid.c): https://github.com/torvalds/linux/blob/master/kernel/pid.c
