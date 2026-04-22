# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☐` 打完后打 tag，触发 prompts/ 工作流

---

## Phase 7 · 用户态与系统调用

### `024_shell`
**效果**：用户态 shell，`echo`/`help`/`clear` 可用

**前置条件（023 已完成）**：用户态编译基础设施、syscall 封装、`launch_first_user()` 启动机制、FPU/SSE 支持

- ☐ `user/libc/syscall.h` 添加 `sys_read(fd,buf,len)` 封装
- ☐ `user/shell/main.cpp`：`_start()` 主循环 `print_prompt → read_line(sys_read) → tokenize → dispatch → repeat`
- ☐ tokenizer：按空格切割，返回 `argc/argv`
- ☐ builtin 表：`{"echo",cmd_echo},{"help",cmd_help},{"clear",cmd_clear},{nullptr,nullptr}`
- ☐ `cmd_echo`：`write(1, argv[1..], ...)`；`cmd_clear`：`write(1, "\033[2J\033[H", 7)`（ANSI 清屏）；`cmd_help`：打印命令列表
- ☐ CMake 切换嵌入 binary 从 `hello` 到 `shell`（`user/CMakeLists.txt`）

---

## Phase 8 · 存储与文件系统

### `025_driver_ahci`
**效果**：串口输出 `[AHCI] Read sector 0: 55 AA`

- ☐ `kernel/drivers/pci.hpp/cpp`：`PCIDevice {bus,slot,func,vendor_id,device_id,class_code,subclass,prog_if,bar[6]}`；`pci_read/pci_write`（写 `0xCF8`，读 `0xCFC`）；`pci_find_ahci(out)`（枚举 class=0x01 subclass=0x06）
- ☐ `kernel/drivers/ahci.hpp/cpp`：`HBAmem [[gnu::packed]]`（cap/ghc/is/pi/...）；`HBAport [[gnu::packed]]`（clb/fb/is/ie/cmd/...）
- ☐ `ahci_init()`：映射 BAR5 MMIO（`VMM::map`），检测 `pi` 位图，为每活跃端口分配 Command List（32×32B）+ FIS Buffer（256B），物理连续且对齐
- ☐ `ahci_read(port,lba,count,buf)`：构造 CFIS（ATA READ DMA EXT=0x25）+ PRDT，写 `port.ci`，轮询 `port.is` 等待完成
- ☐ `ahci_write(port,lba,count,buf)`：同上，命令改为 ATA WRITE DMA EXT=0x35

---

### `026_fs_ramdisk`
**效果**：串口列出 initrd 中的文件名和大小

- ☐ `UstarHeader [[gnu::packed]]` 512 字节：`name[100]/mode[8]/uid/gid/size[12]/mtime[12]/checksum[8]/typeflag/magic[6]`；`static_assert(sizeof==512)`
- ☐ `octal_to_uint(s,len)`：ustar size 字段为八进制 ASCII 转 uint64
- ☐ `ramdisk_mount(void* base)`：遍历 ustar 条目（512 字节对齐），typeflag='0' 为文件，'5' 为目录，magic=`"ustar"` 验证
- ☐ CMake：将 initrd 归档嵌入内核镜像，通过 `_binary_initrd_start/end` 访问

---

### `027_fs_vfs`
**效果**：`open/read/write/close` syscall 框架可用

- ☐ `kernel/fs/vfs.hpp`：`struct Inode {ino,size,type,*fs_private, Ops{read,write,readdir}}`；`struct File {*inode,offset,flags}`；`struct FDTable {*fds[256], alloc(), close(fd)}`
- ☐ `template<T> concept FileSystemImpl` 约束 `lookup(path)→Inode*` 和 `mount(path)→bool`
- ☐ 挂载点表：`MountPoint {path[256], *fs}` 数组
- ☐ 新增 syscall：`SYS_open=2`，`SYS_close=3`；`sys_open` 查挂载点→`lookup`→分配 fd；`sys_read/write` 通过 fd→`File→Inode→Ops`

---

### `028_fs_ext2`
**效果**：挂载 QEMU ext2 分区，shell 中 `ls /` 和 `cat /etc/motd` 可用

- ☐ `Ext2Superblock [[gnu::packed]]`：`s_inodes_count/s_blocks_count/s_log_block_size/s_magic=0xEF53` 等关键字段
- ☐ `Ext2Inode [[gnu::packed]]`：`i_mode/i_uid/i_size/i_block[15]`（0-11 直接块，12 单重间接，13 双重间接）
- ☐ `Ext2DirEntry [[gnu::packed]]`：`inode/rec_len/name_len/file_type/name[]`
- ☐ `ext2_init()`：读 superblock（磁盘偏移 1024B），验证 magic，计算 block_size=`1024<<s_log_block_size`，读 block group descriptor table
- ☐ `ext2_read_inode(ino)`：group=(ino-1)/inodes_per_group，table_block=bg_inode_table，偏移=(ino-1)%inodes_per_group × inode_size
- ☐ `ext2_read_file(inode,buf,offset,len)`：遍历 `i_block[0-11]`（直接），支持 `i_block[12]`（单重间接块），不要求实现写
- ☐ `ext2_readdir(inode,index)`：遍历目录数据块的 `Ext2DirEntry` 链表（`rec_len` 步进）
- ☐ 挂载到 VFS：实现 `FileSystem` concept，`mount("/")`；shell 新增 `ls` 和 `cat` builtin 调 `sys_open/read/close`

---

## Phase 9 · GUI（长期目标）

### `029_gui_framebuffer`
**效果**：屏幕出现渐变色矩形和 `Cinux GUI` 字样

- ☐ `kernel/drivers/canvas.hpp`：`Canvas {*front_buf, *back_buf, width, height, pitch}`；`draw_pixel`，`draw_rect`，`draw_rect_outline`，`draw_line`（Bresenham），`draw_text`，`blit(dst_x,dst_y,src,w,h)`，`flip()`（back→front memcpy），`clear(color=0)`
- ☐ 双缓冲：`back_buf` 用 `kmalloc(width*height*4)` 分配，所有绘制写 back，`flip()` 一次拷贝
- ☐ 测试：绘制 10 个随机色矩形 + 标题文字，`flip()` 后屏幕显示

---

### `030_gui_wm_basic`
**效果**：可拖动窗口，Z-order 正确

- ☐ PS/2 鼠标驱动：初始化 8042 鼠标（CMD `0xA8` 启用，`0xF4` 发给鼠标激活），IRQ12 handler 解析 3 字节包（buttons/dx/dy），维护全局 `mouse_x/y`
- ☐ `kernel/gui/window.hpp`：`Window {x,y,w,h,title[64],*canvas,visible,focused,id}`；`Event {type,union{mouse,key}}`；环形事件队列
- ☐ `class WindowManager {*windows_[64],count_,*focused_,mouse_x_,mouse_y_}`：`create/destroy/raise/composite()`，`handle_mouse(dx,dy,buttons)`，`handle_key(ascii)`
- ☐ `composite()`：从低 Z-order 到高，依次 blit 各 window canvas 到屏幕 back_buf，最后 `flip()`
- ☐ 拖动：`handle_mouse` 检测 button1 按下+移动，更新 focused window 的 x/y

---

### `031_gui_native_app`
**效果**：屏幕出现可交互的终端模拟器窗口

- ☐ `user/apps/terminal.cpp`：`Terminal extends Window`；`screen_[80][25]` 字符缓冲，`cursor_x_/y_`，关联 shell 进程
- ☐ `on_key(ascii)`：发给 shell 进程（通过 pipe/共享内存）
- ☐ `on_paint()`：遍历 `screen_` 调 `canvas->draw_text`，光标用反色块
- ☐ `shell` 输出重定向到 `Terminal::write(str,len)`，更新 `screen_` 并 `on_paint()`
- ☐ WM 注册 terminal 窗口，标题栏 `Cinux Terminal`，最小化/关闭按钮占位

---

## 附录 · AI Checkpoint 工作流

```
☑ 所有 checkbox 完成
  → git tag 编号_大主题_小阶段
  → 复制本 milestone 块 → prompts/03_code_review.md {{code_snippet}}
  → 复制本 milestone 块 → prompts/04_test_generation.md {{interface_snippet}}
  → 测试全绿
  → 复制本 milestone 块 → prompts/01_tutorial_hands_on.md
  → 复制本 milestone 块 → prompts/02_tutorial_readthrough.md（附完整代码）
  → 更新本文件 ☐→☑，更新 README.md 进度表
```

### 占位符速填

| 占位符 | 来源 |
|--------|------|
| `{{current_tag}}` | 刚打的 git tag |
| `{{prev_tag}}` | `git tag` 列表倒数第二 |
| `{{phase_title}}` | milestone `###` 标题 |
| `{{milestone_goal}}` | 本节「效果」一行 |
| `{{key_files}}` | 本节所有「涉及文件」 |
| `{{checklist_items}}` | 本节所有 `☐` 条目 |
| `{{code_snippet}}` | 你写完的实际代码 |