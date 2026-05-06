# M6: Ext2 独立库 + Host 端测试

> 将 ext2 文件系统逻辑提取为可独立编译的库。
> 配合 F1 的 IBlockDevice 抽象，可在 Linux host 上独立测试 ext2 操作。

## 目标

ext2 作为 `libcinux-ext2` 独立库：
- 内核使用：通过 IBlockDevice 接口
- Host 测试：通过 RAMBlockDevice（文件模拟磁盘）

## 任务清单

### T1: ext2 代码解耦

**文件**: `kernel/fs/ext2*.hpp` / `.cpp`

当前 ext2 直接依赖内核头文件（kprintf、PMM、VMM 等）。解耦为：

```
libs/ext2/
├── include/
│   └── ext2/
│       ├── ext2_common.hpp    # 共享常量/类型
│       ├── ext2_superblock.hpp
│       ├── ext2_inode.hpp
│       ├── ext2_directory.hpp
│       ├── ext2_block.hpp
│       ├── ext2_init.hpp
│       └── ext2.hpp           # 统一 forward header
├── src/
│   ├── ext2_common.cpp
│   ├── ext2_superblock.cpp
│   ├── ext2_inode.cpp
│   ├── ext2_directory.cpp
│   ├── ext2_block.cpp
│   └── ext2_init.cpp
├── platform/
│   ├── kernel/                # 内核适配（kprintf → lib log）
│   │   └── ext2_platform.cpp
│   └── host/                  # Linux mock（printf 替代）
│       └── ext2_platform.cpp
└── test/
    ├── test_ext2.cpp           # Host 端单元测试
    └── test_disk.img           # 预制的 ext2 测试镜像
```

- [ ] 提取 ext2 核心逻辑为平台无关代码
- [ ] 平台抽象层：日志、内存分配、assert
- [ ] 内核适配层转发到 kprintf/kmalloc
- [ ] Host 适配层转发到 printf/malloc

### T2: RAMBlockDevice（Host 端）

```cpp
// Host 端块设备实现（文件模拟磁盘）
class FileBlockDevice : public IBlockDevice {
public:
    FileBlockDevice(const char* path);  // 打开磁盘镜像文件
    bool read_blocks(uint64_t block, uint64_t count, void* buf) override;
    bool write_blocks(uint64_t block, uint64_t count, const void* buf) override;
    uint64_t block_count() const override;
    uint64_t block_size() const override { return 1024; }
};
```

- [ ] FileBlockDevice 实现（使用 fopen/fread/fwrite）
- [ ] 支持创建指定大小的空 ext2 镜像

### T3: Host 端测试

- [ ] 挂载预制的 ext2 镜像
- [ ] 读取超级块 + 验证 magic
- [ ] 根目录列举文件
- [ ] 读取文件内容
- [ ] 创建文件 + 写入 + 重新读取验证

### T4: CMake 集成

- [ ] libs/ext2/CMakeLists.txt — 独立库构建
- [ ] 内核通过 target_link_libraries 引入
- [ ] Host 测试通过 CMake test 注册

## 产出物

- [ ] `libs/ext2/` 独立目录结构
- [ ] 平台抽象层
- [ ] FileBlockDevice host 实现
- [ ] Host 端 ext2 单元测试
- [ ] 内核通过 IBlockDevice 使用 ext2
