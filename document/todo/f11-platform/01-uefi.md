# M2: UEFI 启动支持

> UEFI 固件下启动 Cinux。使用 GOP framebuffer、UEFI 内存映射。
> 保留 BIOS 启动兼容。

## 目标

UEFI 模式启动流程：
```
UEFI Firmware → CinuxBOOTX64.EFI (EFI 应用)
  → 获取 GOP framebuffer
  → 获取 UEFI 内存映射
  → ExitBootServices
  → 进入 Cinux 内核（长模式已就绪）
```

## 任务清单

### T1: EFI 应用入口

**文件**: `boot/uefi/boot.c`（新增，C 语言，EFI ABI）

```c
#include <efi.h>
#include <efilib.h>

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    // 1. 初始化 EFI lib
    // 2. 获取 GOP framebuffer
    // 3. 获取内存映射
    // 4. 加载内核 ELF
    // 5. ExitBootServices
    // 6. 跳转到内核入口
}
```

- [ ] EFI 应用框架（使用 GNU-EFI 或手写 EFI 头）
- [ ] EfiMain 入口
- [ ] 构建 EFI 可执行格式 (.efi)

### T2: GOP Framebuffer

```c
// 获取 Graphics Output Protocol
EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, &gop);

// 传递给内核
framebuffer_info.base = gop->Mode->FrameBufferBase;
framebuffer_info.size = gop->Mode->FrameBufferSize;
framebuffer_info.width = gop->Mode->Info->HorizontalResolution;
framebuffer_info.height = gop->Mode->Info->VerticalResolution;
framebuffer_info.pitch = gop->Mode->Info->PixelsPerScanLine * 4;
```

- [ ] GOP 获取和配置
- [ ] 设置分辨率（可选）
- [ ] framebuffer 信息传递给内核

### T3: UEFI 内存映射

```c
UINTN map_size, map_key, desc_size;
UINT32 desc_version;
EFI_MEMORY_DESCRIPTOR *map;

gBS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_version);
```

- [ ] 获取内存映射
- [ ] 转换为 Cinux BootInfo 格式
- [ ] 标记可用/保留/ACPI 区域

### T4: ExitBootServices + 内核跳转

- [ ] ExitBootServices(map_key) 调用
- [ ] 构造 BootInfo 结构体
- [ ] 跳转到内核入口（长模式已就绪，无需模式切换）
- [ ] 内核检测 UEFI vs BIOS 启动方式

### T5: FAT32 ESP 集成

- [ ] Cinux 内核放置在 FAT32 ESP 分区
- [ ] EFI 系统分区布局：/EFI/Cinux/BOOTX64.EFI + cinux.elf
- [ ] mount ESP 为 /boot

### T6: 构建集成

- [ ] CMake 添加 UEFI boot loader 构建目标
- [ ] 生成可启动 UEFI 磁盘镜像
- [ ] QEMU OVMF 验证：`qemu-system-x86_64 -bios /usr/share/OVMF/OVMF_CODE.fd`

## 产出物

- [ ] `boot/uefi/boot.c` — UEFI boot loader
- [ ] GOP framebuffer 支持
- [ ] UEFI 内存映射传递
- [ ] BIOS + UEFI 双启动兼容
- [ ] QEMU OVMF 验证
