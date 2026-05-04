# 033-gui-desktop-3: GUI 初始化集成与桌面测试

## 导语

前两篇我们分别实现了桌面图标管理和应用启动器机制。现在要把这些部件和内核的启动序列串起来，并且用充分的测试来验证整个桌面环境工作正确。本篇聚焦三个部分：init.cpp 中的启动序列变化、icon_data.hpp 的一个关键修复、以及覆盖桌面环境的双轨测试策略。

完成本篇后，整个 033 tag 的桌面环境将完整可用——启动后看到桌面和图标，点击 Shell 图标弹出终端。前置知识是 tag 023 的 syscall init 流程和 tag 031 的 Terminal 窗口。

## 概念精讲

### init.cpp 启动序列的变化

在之前的 tag 中，init.cpp 的 GUI 初始化流程是这样的：挂载 ext2 文件系统，调用 gui_start() 获取 Terminal 指针，创建管道，连接管道到 Terminal，启动 Shell 进程。这个流程中 gui_start() 在管道创建之前被调用，Terminal 在管道就绪之前就已经存在了。虽然后续的 set_stdin_pipe/set_stdout_pipe 调用弥补了这个时间差，但从概念上说 Terminal 处于一个"先创建后配置"的状态——这不是最干净的设计。

新的序列是：挂载 ext2，创建管道，存储管道到 GUI 子系统（set_shell_pipes），启动 GUI（gui_start 不再返回 Terminal），启动 Shell。gui_start 内部只做鼠标初始化和图标注册，不创建任何窗口。终端窗口在用户点击 Shell 图标时才通过 create_shell_terminal 创建。

这个变化的好处是多方面的。首先，init.cpp 不再需要知道 Terminal 的具体类型——它只需要创建管道并交给 GUI 子系统。头文件依赖减少了一个（删除了 `#include "kernel/gui/terminal.hpp"`）。其次，GUI 子系统获得了完全的控制权——何时创建终端、如何配置终端、是否允许多个终端——这些都变成了 GUI 内部的决策，不需要修改 init.cpp。

这种"控制反转"在操作系统设计中很常见。Linux 的 init 进程不直接管理 GUI——它启动 display manager（如 GDM），display manager 再启动窗口系统。每一层只知道自己需要知道的东西。

### consteval 到 constexpr 的降级

icon_data.hpp 中有三个函数从 `consteval` 改为 `constexpr`：`hex_nibble`、`palette_lookup`、`build_icon`。consteval 是 C++20 的特性，要求函数必须在编译期求值——如果编译器无法在编译时确定结果，直接报错。constexpr 则允许函数在编译期或运行时求值。

为什么要降级？在某些编译器配置下（特别是 freestanding kernel build 加上 -O0 调试模式），consteval 可能因为编译器内联限制或调试选项导致编译失败。降级为 constexpr 保留了编译期求值的可能性（只要输入是编译期常量），同时允许在特殊情况下退化为运行时求值。

这是一个实用性妥协——在教学 OS 的语境下，编译通过比纯粹的 C++20 正确性更重要。好消息是，在我们的实际使用中，build_icon 的输入全部是字面量（调色板数组和 hex 字符串数组），所以编译器仍然会选择编译期求值。行为上没有任何变化，只是编译器不再强制要求编译期求值。

### 双轨测试策略——为什么需要两套测试

宿主端单元测试和内核端测试各有优势。宿主端测试在开发机上直接运行，编译快、调试方便、可以用 GDB 单步调试，但需要重新实现所有依赖（MockCanvas、MockPSFFont、甚至 MockWindowManager）。内核端测试在 QEMU 中运行，使用真实的 Framebuffer、Canvas 和 WindowManager，能发现宿主端 mock 无法覆盖的集成问题。

对于桌面环境这种多组件交互的场景，两套测试缺一不可。宿主端测试验证算法正确性（hit_test 边界、consume 重置、容量限制），内核端测试验证集成正确性（composite 不崩溃、图标和窗口的渲染顺序、真实的鼠标事件流）。

这两套测试加起来有 1658 行——比被测试的实现代码（约 200 行）多得多。这在 OS 开发中是正常的——GUI 逻辑的正确性对用户体验影响巨大，充分测试是必要的投入。

## 动手实现

### Step 1: 重构 init.cpp 启动序列

**目标**: 将管道创建和 GUI 启动的顺序调整为新的架构。

**设计思路**: 删除 init.cpp 中对 Terminal 类的依赖。将 `auto* term = gui_start()` 改为直接调用 `gui_start()`（无返回值）。将管道连接逻辑从"直接调用 Terminal 的 set_stdin_pipe/set_stdout_pipe"改为"调用 set_shell_pipes 存储指针"。

具体来说，init.cpp 中的 `#include "kernel/gui/terminal.hpp"` 需要删除，因为 init.cpp 不再直接操作 Terminal。`#include "kernel/gui/gui_init.hpp"` 保留——需要 gui_start 和 set_shell_pipes。管道创建代码本身不变——仍然用 new Pipe 创建，用 PipeReadOps/PipeWriteOps 包装，绑定到全局 fd 表的 fd 0 和 fd 1。

set_shell_pipes 必须在 gui_start 之前调用。kprintf 日志中打印管道指针地址，方便在串口输出中确认调用顺序。

**踩坑预警**: 如果你保留了旧的 `term->set_stdin_pipe(...)` 调用但 term 现在是 void（gui_start 返回 void），编译会直接报错。确保删除所有对 Terminal 指针的操作。另外，注意 init.cpp 中原来有一行 kprintf 打印管道连接信息——这行要保留但措辞需要更新（不再是"连接到 Terminal"而是"存储到 GUI 子系统"）。

**验证**: 构建内核并在 QEMU 中启动。串口日志应该显示 "[INIT] Terminal-shell pipes connected" 和 "[GUI] Shell pipes stored"，并且 set_shell_pipes 在 gui_start 之前被调用。如果你看到 "[GUI] Shell pipes stored: stdin=0x0 stdout=0x0"，说明传了空指针。

### Step 2: 修复 icon_data.hpp 的 constexpr 兼容性

**目标**: 将三个 consteval 函数降级为 constexpr。

**设计思路**: 简单地将 `consteval` 关键字替换为 `constexpr`。三个函数都要改：hex_nibble、palette_lookup、build_icon。功能完全不变——只要输入是编译期常量（我们的图标数据都是字面量），函数仍然在编译期求值。区别只是在编译器无法编译期求值时不会报错，而是退化为运行时求值。

不需要修改调用方——constexpr 和 consteval 的调用语法完全相同。`k_shell_icon` 和 `k_calc_icon` 仍然是 `constexpr std::array<uint32_t, 1024>`，在编译时生成。

**踩坑预警**: 修改后确保宿主端测试仍然通过。如果某个测试依赖编译期求值的结果（比如 static_assert 检查数组大小），改用 constexpr 后需要确认编译器仍然选择编译期路径。在我们的代码中没有这种情况——测试都是运行时验证的。

**验证**: 构建整个项目（内核 + 测试），确认无编译错误。运行宿主端测试中依赖 icon_data 的测试用例，确认结果不变。

### Step 3: 编写内核端桌面测试

**目标**: 在 QEMU 中验证 WindowManager 的桌面图标功能。

**设计思路**: 内核端测试使用真实的 Framebuffer 和 Canvas，初始化 WindowManager 并添加图标，然后验证 hit_test、consume、composite 的行为。测试入口函数在 main_test.cpp 中声明和调用——通过 `RUN_TEST` 宏注册，在 kernel_main 的测试序列中执行。

测试分为几个组。WM 初始化和图标注册组验证 init 后添加图标成功、容量满后返回 false、多个图标分别可命中。hit_test 边界组验证边界像素（左上角命中、右下角+1 像素未命中）、重叠时后注册图标优先。consume 和点击分派组验证无 pending 时返回 None、点击图标设置 action、桌面空白点击不设 action。composite 渲染组验证图标在窗口之下。

测试图标使用纯色填充——白色 Shell 图标、橙色 Calculator 图标。不需要复杂的像素图案，因为测试关注的是位置和命中测试，不是图像内容。

内核端测试中需要特别注意 Framebuffer 的初始化——它需要有效的 BootInfo 结构。测试代码通常在固定的物理地址（0x7000）读取 BootInfo，确保 bootloader 已经正确设置了该地址的数据。如果 BootInfo 不正确，Framebuffer::init 会失败，后续所有依赖 Canvas 的测试都会崩溃。

**踩坑预警**: CMakeLists.txt 中要把 test_desktop.cpp 添加到 big_kernel_test 的源文件列表。如果你忘了加，run_desktop_tests 函数的符号就不会被链接进来，内核启动时不会执行桌面测试。这个错误不会导致编译失败——只是测试不运行。

**验证**: 在 QEMU 中运行 big_kernel_test，串口输出应该显示 "Desktop Tests (033_gui_desktop)" 测试区块，所有 16 个测试通过。特别注意 composite 相关测试——它们检查真实 framebuffer 上的像素值。

### Step 4: 编写宿主端桌面测试

**目标**: 在开发机上验证桌面图标算法的正确性。

**设计思路**: 宿主端测试完全重新实现了 WindowManager 中与桌面图标相关的所有类型和逻辑。MockCanvas 实现了完整的像素写入、draw_bitmap、clear 等方法。MockWindowManager 完整复现了 add_desktop_icon、hit_test_icon、consume_pending_icon_action、handle_mouse 的桌面图标逻辑。测试覆盖了 add/hit/consume 的所有边界条件。

MockCanvas 的关键方法是 `pixel()` 访问器——测试用它在 composite 后检查特定像素的值。MockWindowManager 中的 hit_test_icon 和 hit_test（窗口）都要从后向前遍历，保持和真实代码一致——如果 mock 的遍历方向反了，Z 优先级测试就会失败。

CMakeLists.txt 中新增 test_desktop 可执行文件，并将其加入 test_host、test_all、test_verbose 的依赖列表。这样每次运行 `make test_host` 都会自动编译和运行桌面测试。

**踩坑预警**: MockWindowManager 中的 handle_mouse 实现要特别注意——它需要复现完整的点击分派逻辑（窗口优先 → 图标 → 桌面）。如果 mock 的分派逻辑和真实代码不一致，"窗口覆盖图标时不触发图标 action" 这个测试就会通过但实际行为是错的。

**验证**: 在构建目录中运行 `ctest -R desktop --output-on-failure`，所有测试应该通过。大约有 20 个测试用例覆盖了 add、hit、consume、click、composite 等所有维度。

## 构建与运行

最终构建分三步。

第一步构建宿主端测试并运行 ctest，确认 1123 行的宿主端测试全部通过。这些测试覆盖了桌面图标管理的所有算法正确性。

第二步构建 big_kernel_test 并在 QEMU 中运行，确认 535 行的内核端测试全部通过。这些测试验证了真实硬件环境下的集成正确性。

第三步构建 big_kernel 并在 QEMU 中启动，进行最终的视觉验证。你应该能看到深青色桌面背景、左侧两个图标（Shell 和 Calculator，白色标签居中）、鼠标光标可用。点击 Shell 图标，终端窗口弹出并显示 Shell 输出。整个流程从启动到桌面到交互一气呵成。

## 测试策略的深层考量

在动手完成所有步骤之后，我们来看看这个 tag 的测试策略中有哪些值得讨论的设计决策。

### 为什么宿主端测试要完全重实现 WindowManager

一个常见的疑问是：为什么要花 1123 行代码重新实现一个 MockWindowManager，而不是想办法把内核的 WindowManager 拿到宿主端测试中？答案是依赖关系太深了。WindowManager 依赖 Canvas（依赖 Framebuffer）、PSFFont（依赖 BIOS 字体加载）、Window（依赖事件系统）、DesktopIcon（依赖 icon_data）——层层展开后，你几乎要把整个 GUI 子系统都搬到宿主端。

重实现虽然代码量大，但有几个好处。首先是隔离性——MockWindowManager 的行为完全由测试控制，不会因为内核代码的修改而意外改变。其次是编译速度——mock 类很轻量，不需要链接任何内核代码。最后是灵活性——你可以在 mock 中暴露内部状态（如 pending_icon_action() 访问器）供测试直接检查，而不用通过公开 API 间接验证。

### 测试覆盖的边界案例

宿主端测试中有几个特别值得注意的边界案例。第一个是"零尺寸图标"——一个 width 或 height 为 0 的 DesktopIcon。contains() 方法在 width/height 为 0 时会立即返回 false（因为 mx < x + 0 永远不成立），所以零尺寸图标永远不会被命中。这个行为是正确的——零尺寸的东西不应该有可点击区域。

第二个是"图标在屏幕原点"——DesktopIcon 的 x 和 y 都为 0。contains() 使用 `>=` 和 `<` 比较，所以 (0, 0) 是包含在内的。这个边界值在实现中经常出错——如果你不小心用了 `>` 代替 `>=`，原点就会变成不命中。

第三个是"init 重置"——在添加图标后重新调用 init，确认计数器和 pending action 都被重置。这个测试确保了 WindowManager 可以在测试之间安全地重复初始化，不会出现状态泄漏。

### 与 xv6-gui 测试的对比

xv6-gui 几乎没有 GUI 测试——它依赖"在 QEMU 中跑起来看看"来验证 GUI 功能。这在简单的单窗口 GUI 中还行，但在有多窗口、多图标交互的场景下会遗漏很多 bug。比如"窗口覆盖图标时点击窗口不应该触发图标动作"这种边界行为，不做自动化测试几乎不可能每次手动验证。

## 调试技巧

**宿主端测试编译错误**: 检查 CMakeLists.txt 中的 include 目录是否包含了 mock 需要的头文件路径。test_framework.h 的路径要正确。如果链接时报 undefined reference，检查 CMakeLists.txt 中是否把 test_desktop.cpp 添加到了正确的可执行文件。

**内核测试中 get_pixel 返回错误颜色**: 确认 Framebuffer 和 Canvas 已经正确初始化。composite 前调了 clear(0) 吗？如果 clear 调了但 get_pixel 仍然返回 0，说明 Canvas 的 init 有问题——检查 BootInfo 中的 framebuffer 地址和尺寸是否正确。

**init.cpp 编译报错**: 确认删除了 `#include "kernel/gui/terminal.hpp"` 和所有对 Terminal 指针的引用。如果编译器报 "use of undeclared identifier 'term'"，说明还有遗漏的旧代码。搜索 init.cpp 中所有出现 "term" 的地方，确保没有残留。

**终端弹出但 Shell 无响应**: 检查 set_shell_pipes 的调用顺序和管道指针的有效性。在 create_shell_terminal 中打印 g_stdin_pipe 和 g_stdout_pipe 的地址，确认它们不是 0x0。

## 本章小结

| 变更 | 文件 | 关键点 |
|------|------|--------|
| 启动序列重构 | init.cpp | 管道存储在 GUI 启动前，删除 Terminal 依赖 |
| constexpr 修复 | icon_data.hpp | consteval 改 constexpr，编译兼容性 |
| 内核端测试 | kernel/test/test_desktop.cpp | 535 行，16 个测试，真实 Framebuffer |
| 宿主端测试 | test/unit/test_desktop.cpp | 1123 行，约 20 个测试，Mock 重实现 |
| CMake 集成 | 两个 CMakeLists.txt | 添加 test_desktop 到构建目标 |
| main_test 注册 | kernel/test/main_test.cpp | 声明并调用 run_desktop_tests |
