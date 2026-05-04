# 028c-1 Read-through: 路径解析与进程工作目录

## 导语

我们现在来逐步阅读 028c tag 的代码变更——从最底层的路径解析基础设施开始，一路看到用户空间的 Shell 命令实现。这一章聚焦在三个核心模块：Task 结构体中的 CWD 字段、path.cpp 中的规范化算法、以及 path_util.cpp 中为 syscall 层提供的统一入口。

## Task 结构体中的 CWD 字段

一切从 `kernel/proc/process.hpp` 开始。我们在 `Task` 结构体的末尾新增了一个字段：

```cpp
struct Task {
    // ... 所有已有字段 ...

    /** FPU/SSE state (512 bytes, 16-byte aligned for fxsave/fxrstor). */
    alignas(16) uint8_t fpu_state[512];

    /** Per-process current working directory (absolute path, NUL-terminated). */
    char cwd[256];
};
```

`cwd[256]` 是一个固定大小的字符数组，存储的是绝对路径字符串，NUL 结尾。256 字节对于教学内核来说完全够用——即使路径深达 20 层（每层平均 8 个字符），也不过 160+20=180 字节左右。放在 `fpu_state` 之后是因为 FPU 状态是结构体中最后一个对齐敏感的字段，新增的字符数组不需要特殊对齐。

CWD 的初始化发生在两个地方。第一处是 `kernel/proc/process.cpp` 的 `TaskBuilder::build()` 中：

```cpp
// Step 7.5: Initialise cwd to "/"
task->cwd[0] = '/';
task->cwd[1] = '\0';
```

这意味着所有通过 TaskBuilder 创建的 Task，CWD 都从根目录开始。第二处是在 `kernel/arch/x86_64/usermode.cpp` 的 `launch_first_user()` 函数中，专门为 Shell 进程准备了一个静态 Task：

```cpp
// Create a minimal Task so chdir/getcwd can read/write a per-process cwd
static cinux::proc::Task shell_task{};
shell_task.cwd[0] = '/';
shell_task.cwd[1] = '\0';
shell_task.state = cinux::proc::TaskState::Running;
cinux::proc::Scheduler::set_current(&shell_task);
```

这里用 `static` 局部变量是因为 `launch_first_user()` 最后会调用 `jump_to_usermode` 跳转到 Ring 3，这个函数不会返回。如果 Task 是栈上变量，理论上没问题（栈帧不会被销毁），但 static 更安全、意图更明确。

为了支持 `set_current`，`kernel/proc/scheduler.hpp` 中新增了声明：

```cpp
static void set_current(Task* task);
```

实现在 `scheduler.cpp` 中：

```cpp
void Scheduler::set_current(Task* task) {
    current_ = task;
    g_per_cpu.current = task;
}
```

同时更新了静态成员 `current_` 和 Per-CPU 数据页中的 `g_per_cpu.current`。之所以两边都要设，是因为 syscall 入口通过 `gs:0` 读取内核栈指针，而 `current()` 方法直接访问 `current_`。两边不同步会导致 chdir/getcwd 在某些路径下拿到 nullptr。

## path.hpp —— 公共接口定义

路径解析的公共接口定义在 `kernel/fs/path.hpp` 中，整个文件只有 46 行：

```cpp
namespace cinux::fs {

/// Maximum absolute path length (including NUL terminator)
static constexpr uint32_t PATH_MAX = 4096;

void path_canonicalize(char* buf);

bool path_resolve(const char* cwd, const char* path, char* out);

}  // namespace cinux::fs
```

`PATH_MAX = 4096` 是 Linux 的标准值。定义为 `static constexpr` 意味着每个包含此头文件的翻译单元都有一份自己的副本，但因为是编译期常量，不会产生链接冲突。这个常量替代了之前分散在 sys_creat.cpp、sys_mkdir.cpp、sys_unlink.cpp、sys_rmdir.cpp 中各自的 `constexpr uint32_t PATH_MAX = 4096`。

`path_canonicalize` 的接口设计有一个值得注意的地方——它是"原地修改"的。传入的 `buf` 既是输入也是输出。内部实现使用一个临时缓冲区 `out[PATH_MAX]` 做处理，最后 memcpy 回 buf。这个设计避免了堆分配，但要求 buf 的大小至少是 PATH_MAX。

## path.cpp —— 规范化算法的实现

`kernel/fs/path.cpp` 是路径解析的核心，共 133 行，包含两个函数。先看 `path_canonicalize`：

```cpp
void path_canonicalize(char* buf) {
    if (buf == nullptr || buf[0] == '\0') {
        return;
    }

    uint32_t len = static_cast<uint32_t>(strlen(buf));
    char out[PATH_MAX];
    uint32_t out_pos = 0;

    // Always produce a leading '/' — result is an absolute path
    out[out_pos++] = '/';
```

函数开头做 null 检查和空字符串检查——直接返回，不做任何修改。然后分配一个 PATH_MAX 大小的栈上缓冲区作为输出区域，写入前导 `/`。这意味着无论输入是什么，输出一定以 `/` 开头。

接下来跳过输入中所有前导 `/`：

```cpp
    uint32_t i = 0;
    // Skip leading slashes
    while (i < len && buf[i] == '/') {
        ++i;
    }
```

然后进入主循环，逐个提取路径组件：

```cpp
    while (i < len) {
        // Extract the next component
        uint32_t comp_start = i;
        while (i < len && buf[i] != '/') {
            ++i;
        }
        uint32_t comp_len = i - comp_start;

        // Skip trailing/duplicate slashes
        while (i < len && buf[i] == '/') {
            ++i;
        }
```

每个"组件"是两个 `/` 之间的字符串。提取完组件后，跳过所有连续的 `/`。

然后根据组件内容做不同处理。首先是空组件（连续 `/` 的情况）和 `.`：

```cpp
        if (comp_len == 0) {
            continue;
        }

        // Handle "." — skip
        if (comp_len == 1 && buf[comp_start] == '.') {
            continue;
        }
```

`..` 的处理是算法的核心——需要"弹出"输出缓冲区中的最后一个组件：

```cpp
        // Handle ".." — pop one component
        if (comp_len == 2 && buf[comp_start] == '.' && buf[comp_start + 1] == '.') {
            if (out_pos > 1) {
                --out_pos;
                while (out_pos > 0 && out[out_pos - 1] != '/') {
                    --out_pos;
                }
                // Remove the '/' separator unless it's the root '/'
                if (out_pos > 1) {
                    --out_pos;
                }
            }
            continue;
        }
```

弹出逻辑是：先退一格（跳过最后一个字符），然后往前搜索直到遇到 `/`，再退掉那个 `/`。`out_pos > 1` 的条件保护了根目录——如果输出只有 `/`（out_pos == 1），`..` 什么也不做。

普通组件的追加：

```cpp
        // Normal component: append '/'
        if (out_pos > 0 && out[out_pos - 1] != '/') {
            out[out_pos++] = '/';
        }

        // Copy component
        for (uint32_t j = 0; j < comp_len && out_pos < PATH_MAX - 1; ++j) {
            out[out_pos++] = buf[comp_start + j];
        }
```

最后确保至少有 `/`，然后拷贝回 buf：

```cpp
    if (out_pos == 0) {
        out[out_pos++] = '/';
    }

    out[out_pos] = '\0';
    memcpy(buf, out, out_pos + 1);
}
```

接下来是 `path_resolve`，它负责把 CWD 和用户路径合体：

```cpp
bool path_resolve(const char* cwd, const char* path, char* out) {
    if (cwd == nullptr || path == nullptr || out == nullptr) {
        return false;
    }

    // Absolute path: copy and canonicalise
    if (path[0] == '/') {
        uint32_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) {
            out[i] = path[i];
            ++i;
        }
        out[i] = '\0';

        path_canonicalize(out);
        return true;
    }
```

绝对路径的情况很简单——直接拷贝后规范化。相对路径的情况需要拼接：

```cpp
    // Relative path: cwd + "/" + path
    uint32_t pos = 0;

    // Copy cwd
    while (cwd[pos] != '\0' && pos < PATH_MAX - 2) {
        out[pos] = cwd[pos];
        ++pos;
    }

    // Add separator if cwd doesn't end with '/'
    if (pos > 0 && out[pos - 1] != '/' && pos < PATH_MAX - 2) {
        out[pos++] = '/';
    }

    // Append path
    uint32_t j = 0;
    while (path[j] != '\0' && pos < PATH_MAX - 1) {
        out[pos++] = path[j++];
    }
    out[pos] = '\0';

    path_canonicalize(out);
    return true;
}
```

拼接逻辑注意了两个细节：`PATH_MAX - 2` 是为分隔符 `/` 和 NUL 预留空间；`out[pos - 1] != '/'` 检查避免了 `//` 开头的重复分隔符（虽然 canonicalize 能处理，但提前检查更干净）。

## path_util —— Syscall 层的统一入口

`kernel/syscall/path_util.hpp` 提供了两个函数。`validate_user_ptr` 是一个 inline 函数，做 x86_64 规范地址检查：

```cpp
inline bool validate_user_ptr(uint64_t ptr) {
    if (ptr == 0) {
        return false;
    }
    uint64_t bit47 = (ptr >> 47) & 1;
    uint64_t upper = ptr >> 48;
    if (bit47 == 0 && upper != 0) {
        return false;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return false;
    }
    return true;
}
```

x86_64 的虚拟地址只有低 48 位有效，bit 47 到 bit 63 必须"符号扩展"——要么全 0（用户空间），要么全 1（内核空间）。这段代码检查的就是这个约束。

`resolve_user_path` 的实现在 path_util.cpp 中：

```cpp
bool resolve_user_path(uint64_t path_virt, char* out) {
    if (!validate_user_ptr(path_virt)) {
        return false;
    }

    auto* path = reinterpret_cast<const char*>(path_virt);

    if (path[0] == '\0') {
        return false;
    }

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char* cwd = (current != nullptr) ? current->cwd : "/";

    return cinux::fs::path_resolve(cwd, path, out);
}
```

先验证指针，再检查空字符串，然后获取当前进程的 CWD，最后调用 path_resolve。如果 `Scheduler::current()` 返回 nullptr（理论上不应该，但防御性编程），CWD 回退到 `"/"`。

## 小结

这一章我们阅读了 CWD 存储和路径解析的全部内核代码。核心思路是：CWD 作为 Task 结构体的字段，path_canonicalize 做算法层面的路径清理，path_resolve 做 CWD 拼接，resolve_user_path 加上指针验证和进程上下文。这四层组合起来，就构成了一个完整的、cwd 感知的路径解析管线。下一章我们来看构建在这条管线之上的 syscall 实现。
