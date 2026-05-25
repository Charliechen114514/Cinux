---
title: 验证内核加载
---

# 004 - 验证内核加载成功

## 目标
验证 mini kernel 成功从磁盘加载到内存 0x20000。

## 验证方法

### 1. 提取磁盘镜像中的内核
```bash
# 从 disk.img 提取内核部分（假设内核从某扇区开始）
dd if=disk.img bs=512 skip=N count=1 | xxd
```

### 2. GDB 查看内存
启动 QEMU with GDB server：
```bash
qemu-system-x86_64 -s -S disk.img ...
```

在 GDB 中：
```
(gdb) target remote :1234
(gdb) x/10x 0x20000
```

### 3. 成功标志
内存内容和磁盘镜像一致，说明：
- ✅ 磁盘读取成功
- ✅ 数据搬运到正确地址
- ✅ 内存内容未损坏

## 实际结果

```
(gdb) x/10x 0x20000
0x20000:        0xc4c748fa      0x00022080      0x48ed3148      0x0080c7c7
0x20010:        0xc7480002      0x022088c1      0xf9294800      0xf3c03148
0x20020:        0x3c8948aa      0x02208825
```

对应指令（反汇编）：
```
0x20000:  cli
0x20001:  mov    rsp, 0x20000
0x20008:  xor    ebp, ebp
0x2000a:  mov    rdi, 0x8000
0x20011:  mov    qword ptr [rbp + 0x2], 0x8020
0x20019:  and    ecx, 0x29f9
0x2001f:  xor    rax, rax
0x20022:  stos   qword ptr [rdi], rax
0x20024:  mov    ecx, dword ptr [rax + 0x2280]
```

## 踩坑记录

### 问题 1：加载的是 ELF 而不是 bin
**现象**：
```
0x20000:  0x464c457f  # "\x7FELF"
```

**原因**：`build_image.sh` 还在使用 ELF 文件，没有使用 `mini_kernel.bin`。

**解决**：修改构建脚本，使用 `objdump -O binary` 或 `ld -oformat binary` 生成纯二进制。

### 问题 2：C++ 代码没有被链接
**现象**：bin 文件只有 8 字节，只包含 boot.S 的入口代码。

**原因**：CMakeLists.txt 没有正确链接 main.cpp。

**解决**：确保 CMakeLists.txt 包含所有源文件：
```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    main.cpp
)
```

### 问题 3：entry point 地址不匹配
**现象**：跳转后 CPU hang 或 triple fault。

**原因**：链接脚本的 entry point 和 boot.S 调用地址不一致。

**解决**：
- 链接脚本：`ENTRY(_start)`
- 跳转地址：与链接地址一致（0x20000）

## 验证清单

- [ ] 磁盘镜像中内核段内容正确
- [ ] 内存 0x20000 处内容与镜像一致
- [ ] 反汇编代码符合预期
- [ ] entry point 地址正确（0x20000）
- [ ] 下一步：验证 protected mode 跳转成功
