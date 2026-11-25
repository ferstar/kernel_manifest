# 真我 GT5 Pro（RMX3888）Android 16 内核

面向真我 GT5 Pro（SM8650，Android 16 / GKI `6.1.118`）的自编译内核，用 **memcomp** 替代 `zram`/`zswap`（默认 **LZ4**）。

## 核心特性

- **memcomp 内存压缩**：集成 6.1 回移植 + **LZ4 v1.11.1**（ARM64 高速解压优化）+ **Zstd v1.6.0** 回移植，支持运行时免重启热切算法：
  ```sh
  echo zstd > /sys/kernel/mm/memcomp/compressor   # 切回用 lz4
  ```
- **MGLRU 开机保活**：补丁 006 兼容 Oplus 开机检测，开机保持 `/sys/kernel/mm/lru_gen/enabled = 0x0003`。
- **支持 DroidSpaces**：默认开启 kABI 安全的 IPC / Namespace / NAT 支持。
- **配套 KSU 模块（Memcomp Monitor）**：WebUI 实时看压缩池、在线切 LZ4/Zstd、屏蔽厂商主动回收干扰，内置 `RMX3888_16.0.3.500` 的 `oplus_bsp_schedinfo` 驱动修复入口（详见 [模块说明](ksu/memcomp-monitor/README.txt) / [开发文档](ksu/memcomp-monitor/DEVELOPING.md)）。

## 实测数据

**DGX Spark 原生 ARM64 KVM 对比（64 MiB 匿名页，LZ4，10 组配对中位数）：**

| 指标 | zram | memcomp | 对比 |
| --- | ---: | ---: | --- |
| 首次 `MADV_PAGEOUT` | 101.62 ms | 97.79 ms | 快 3.8% |
| 单页缺页 P95 | 3.65 µs | **2.46 µs** | **快 32.6%** |
| 单页缺页 P99 | 14.75 µs | **2.80 µs** | **快 5.3 倍** |

**GT5 Pro 真机 10 App 热切（各 80 次）：** MGLRU 开启后前台 P95 从 `185 ms` 降至 **`155 ms`（-16.2%）**，重载 `0/80`。完整测试记录见 [PR #4](https://github.com/ferstar/kernel_manifest/pull/4) 与 [模块索引](.agents/MODULES.md)。

## 编译

在 x86-64 Linux（Debian / Ubuntu）下运行：

```bash
SKIP_DEPS=n ./build.sh   # 首次运行安装依赖并构建
./build.sh               # 日常构建
```

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `MANIFEST` | `gt5pro` | 输出包机型标识 |
| `CUSTOM_SUFFIX` | `YYYYMMDD` | 固定前缀 `android14-11-o-v` 后的构建标签 |
| `APPLY_DROIDSPACES` | `y` | `n` 跳过 DroidSpaces 补丁与配置 |
| `SKIP_DEPS` | `y` | `n` 构建前用 `sudo apt` 安装依赖 |

产物位置：`workspace/kernel_ws/ak3-<MANIFEST>-android14-11-o-v<CUSTOM_SUFFIX>.zip`（内核镜像位于 `workspace/kernel_ws/common/out/arch/arm64/boot/Image`）。
