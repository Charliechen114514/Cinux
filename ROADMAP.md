# Cinux · 开发路线图 (ROADMAP)

> **Tag 规范**：`编号_大主题_小阶段`，如 `003_boot_long_mode`  
> **AI 用法**：复制任意 milestone 块喂给本地 AI 生成代码骨架 / 教程大纲  
> **Checkpoint**：所有 `☐` 打完后打 tag，触发 prompts/ 工作流

---

## Phase 8 · 存储与文件系统

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

### `028b_sync_safety`
**效果**：所有内核共享数据结构加锁保护，PMM/Heap/调度器/中断上下文可安全并发

- ☐ 重新审查所有的组件，看看哪一些组件需要保证线程安全，重新设计本Roadmap milestone
- ☐ `Spinlock` 改用 `std::atomic<bool>` 替换 `volatile bool locked_`，补齐 acquire/release 语义
- ☐ `PMM::alloc_page/alloc_pages/free_pages` 临界区加 `Spinlock`（find + set/clear 必须原子）
- ☐ `Heap::alloc/free` 临界区加 `Spinlock`（free list 遍历 + 修改原子化）
- ☐ `Scheduler` 运行队列加 `Spinlock`：`enqueue/dequeue/pick_next` 持锁，`tick()` 中关中断
- ☐ `Scheduler::tick_count_`、`Process` 的 `next_tid` / `next_stack_vaddr` 改用 `std::atomic`
- ☐ 键盘 IRQ handler 与 `poll()/enqueue()` 之间：ring buffer 访问加关中断保护或 `Spinlock`
- ☐ `FDTable::alloc/close` 加 `Spinlock`，防止 double-close / use-after-free
- ☐ `syscall_register()` / `syscall_dispatch()` 对 syscall table 加 `Spinlock` 或 RCU 风格（写时加锁、读时无需）
- ☐ 全局审计：`grep` 所有 static/global 可变状态，确认均已保护或标注「启动单线程只用一次」
- ☐ 新建 `test/unit/test_sync_safety.cpp`：为每个已加锁组件（Spinlock、PMM、Heap、Scheduler 队列、FDTable）编写并发压力测试 — 多线程同时操作，验证无数据竞争、无资源泄漏、状态一致

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