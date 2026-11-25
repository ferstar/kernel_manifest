# 真我 GT5 Pro（SM8650）Android 16 内核

`realme/sm8650_b` 分支用于为真我 GT5 Pro（RMX3888，Android 16 / Android B）构建 GKI `6.1.118` 内核。本分支使用 **memcomp** 替代传统的 `zram` / `zswap` 进行内存匿名页压缩，默认压缩算法为 **LZ4**。

## 本分支包含的改动

- **上游源码**：`common` 固定在真我官方 AndroidB 源码 `e76bfb318e6c2abb2a888aa1a8cfa9488715d9b7`，`vendor` 跟踪官方 `master` 分支。
- **内核特性**：集成 `memcomp` 6.1 回移植、**LZ4 v1.11.1**（带 ARM64 高速解压优化）、**Zstd** 回移植；默认开启 **DroidSpaces** 所需的 IPC / Namespace / 网络支持（kABI 安全）。
- **Vendor 修复**：移除了 `oplus_bsp_schedinfo` 中多余的 slab constructor（热线程节点分配时仍正常清零），消除高负载下的 `new_slab` 告警。

## 压缩算法在线切换（LZ4 / Zstd）

打开 **Memcomp Monitor → 内存** 可查看当前压缩算法；在支持运行时切换的内核版本上，可以直接切换 **LZ4** 或 **Zstd**（无需重启）：新压入的页使用新算法，已在池中的页保留原解码器直到读回或释放，重启后恢复默认 `lz4`。

也可以在 root shell 中直接操作：

```sh
echo zstd > /sys/kernel/mm/memcomp/compressor
cat /sys/kernel/mm/memcomp/compressor
```

（写 `lz4` 即可切回。）

## MGLRU 开机保活

RMX3888 原厂开机脚本会检查 `/proc/meminfo` 中是否存在 `Ezr:` 字段，缺失时会把 MGLRU 关掉。补丁 006 在 `memcomp` 就绪后输出 `Ezr: 0 kB (fake for Oplus MGLRU boot check)`，让开机脚本保留内核的 MGLRU 配置。实测开机后 `/sys/kernel/mm/lru_gen/enabled` 保持为 `0x0003`（仍支持通过 sysfs 手动修改）。

在同一台 RMX3888（LZ4 内核）上按固定顺序轮切 10 个常用 App（每种模式各测 80 次回前台，均为热切）：

| 指标 | MGLRU 关 | MGLRU 开 | 变化 |
| --- | ---: | ---: | --- |
| 前台 P50 | 110 ms | 109 ms | -0.9% |
| 前台 P95 | 185 ms | 155 ms | **-16.2%** |
| 进程 / Activity 重载 | 0/80 | 0/80 | 持平 |
| 充分预热终轮 P95（每模式后 20 次） | 156 ms | 150 ms | -3.8% |

> 注：充分预热终轮按“先开后关”顺序执行，热身效应缩小了差距，因此全量 30 ms 的 P95 差值并非全部来自 MGLRU。

## 配套 KernelSU 模块（Memcomp Monitor）

在 KernelSU 中刷入可选的 `ksu-gt5pro-memcomp-*.zip` 模块后：

- 可在 WebUI 中实时查看 `memcomp` 压缩池占用、在线切换 LZ4 / Zstd；
- 在支持的 RMX3888 机型上自动禁用厂商主动 swap 与动态 zram 开关，避免和 `memcomp` 打架；
- **驱动修复**页面支持在已解锁且固件为 `RMX3888_16.0.3.500` 的设备上备份、修补并恢复当前槽位的 `vendor_boot` ramdisk（写入分区前需要二次确认并做回读校验）。

更多设备端说明见 [模块说明](ksu/memcomp-monitor/README.txt)，打包与离线检查见 [开发文档](ksu/memcomp-monitor/DEVELOPING.md)。

## 编译方法

需在 **x86-64 Linux** 主机上构建（使用预编译 Clang 工具链）。在 Debian / Ubuntu 上首次构建时运行（会调用 `sudo apt` 安装依赖）：

```bash
SKIP_DEPS=n ./build.sh
```

依赖装齐后，日常直接运行：

```bash
./build.sh
```

脚本首次运行会自动下载 Clang 并初始化 `workspace/`；后续构建会自动重置并清理 `workspace/` 下的 `common`、`vendor` 与 `AnyKernel3` 目录，并在内存大于 16 GiB 的机器上重建 `/tmp/out` 作为编译输出目录，请勿在这些目录里保留未提交的本地修改。

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `MANIFEST` | `gt5pro` | 刷机包文件名里的机型标识（源码仓库固定为 GT5 Pro） |
| `CUSTOM_SUFFIX` | `YYYYMMDD` | 附加在固定 GKI 6.1 KMI 前缀后的构建标签（`android14-11-o-v<CUSTOM_SUFFIX>`） |
| `APPLY_DROIDSPACES` | `y` | 设为 `n` 可跳过 DroidSpaces kABI 补丁及对应内核配置 |
| `SKIP_DEPS` | `y` | 默认跳过 `apt` 依赖安装；设为 `n` 在构建前通过 `sudo` 安装依赖 |

## 编译产物

| 产物 | 路径 |
| --- | --- |
| ARM64 内核镜像 | `workspace/kernel_ws/common/out/arch/arm64/boot/Image` |
| AnyKernel3 卡刷包 | `workspace/kernel_ws/ak3-<MANIFEST>-android14-11-o-v<CUSTOM_SUFFIX>.zip` |

> GT5 Pro 在早期启动阶段从 `vendor_boot` ramdisk 加载 `oplus_bsp_schedinfo.ko`，AnyKernel3 刷机包本身只包含内核 `Image`。如需应用热线程修复，请通过配套 KSU 模块的 **驱动修复** 页面或单独打包 `vendor_boot` 部署。

## 测试与验证

### DGX Spark 原生 ARM64 KVM 对比

在 DGX Spark 上使用 4 vCPU / 1 GiB RAM 的原生 ARM64 KVM 虚拟机，每轮压出 64 MiB 匿名页并随机缺页回读 4,096 页（双方均使用 LZ4，共 10 组交替配对，下表为各轮中位数）：

| 指标 | zram | memcomp | 对比 |
| --- | ---: | ---: | --- |
| 首次 `MADV_PAGEOUT`（64 MiB） | 101.615 ms | 97.790 ms | 快 3.8% |
| 单页缺页延迟 P95 | 3.650 µs | **2.460 µs** | **快 32.6%** |
| 单页缺页延迟 P99 | 14.750 µs | **2.800 µs** | **快 5.3 倍** |

### GT5 Pro 真机验证

- **重复 `MADV_PAGEOUT` 修复**：修复前重复调用 `MADV_PAGEOUT` 会因对压缩条目做无效 swapfile 查询而卡顿约 5.4 s 并刷屏日志；修复后在真机跑 40 组对照，第二次 `PAGEOUT` 中位数耗时 **38.281 µs**（最大 282.656 µs），缺页 P95 组内中位差 `0.0 µs`，全程零 `Bad swap file entry`。
- **Vendor 模块修复**：加载修补后的 `oplus_bsp_schedinfo.ko` 后，16 s 负载期间 `top_hotthread` 正常记录全部 8 核，内核日志无 `new_slab` 告警。

完整测试条件与回归记录见 [PR #4](https://github.com/ferstar/kernel_manifest/pull/4)，补丁与回归测试清单见 [模块索引](.agents/MODULES.md)。
