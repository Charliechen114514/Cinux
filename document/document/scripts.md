# 构建脚本

> 里程碑: 全部 (支撑性)

## 功能概述

Cinux 的构建、调试和测试辅助脚本集合。

## 磁盘镜像
| 脚本 | 功能 |
|------|------|
| `scripts/build_image.sh` | 构建完整磁盘镜像 (sector 0=MBR, 1-15=stage2, 16+=mini.bin, 大内核, initrd) |
| `scripts/create_ext2_disk.sh` | 创建 ext2 文件系统镜像 |
| `scripts/create_ahci_test_disk.sh` | 创建 AHCI 测试磁盘 |
| `scripts/embed_binary.sh` | 嵌入二进制到内核镜像 |

## 调试
| 脚本 | 功能 |
|------|------|
| `scripts/debug_qemu.sh` | QEMU 调试启动 |
| `scripts/launch_qemu_debug.sh` | 带 GDB 的 QEMU 调试启动 |
| `scripts/.gdbinit` | GDB 配置 |

## 测试
| 脚本 | 功能 |
|------|------|
| `scripts/qemu_test_wrapper.sh` | QEMU 测试包装器 |
| `scripts/run_all_tests.sh.in` | 测试运行脚本模板 |

## 工具
| 脚本 | 功能 |
|------|------|
| `scripts/append_crc32.py` | CRC32 追加 |
| `scripts/check_memory_layout.py` | 内存布局检查 |
| `scripts/generate_large_elf.py` | 生成大型 ELF 测试文件 |
| `scripts/gen_psf_font.py` | PSF 字体生成 |
| `scripts/check_toolchain.sh` | 工具链检查 |

## 子目录
- `scripts/compile/` — 编译辅助脚本
- `scripts/log/` — 日志工具 (`logging.sh`)

## 源码位置
- `scripts/` — 所有脚本
