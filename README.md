# Realme GT5 Pro (SM8650) Android 16 kernel

The `realme/sm8650_b` branch builds GKI 6.1.118 for the Realme GT5 Pro on Android 16 (Android B). It uses memcomp to compress anonymous pages in memory, with LZ4 as the default. Both zram and zswap are disabled in this build.

## What this branch builds

- The build pins the Realme AndroidB common kernel to `e76bfb318e6c2abb2a888aa1a8cfa9488715d9b7` and takes the vendor sources from their `master` branch.
- The common kernel carries the memcomp backport, LZ4 v1.11.1 and a Zstd backport. DroidSpaces IPC, namespace and networking support is enabled by default.
- The vendor patch removes a redundant slab constructor in `oplus_bsp_schedinfo`. Hot-thread nodes are still zeroed on allocation.

## Memcomp compression algorithm

Open **Memcomp Monitor → 内存** to see which compressor the kernel is using. On kernels with runtime selection, choose LZ4 or Zstd for new stores. Pages already in the pool keep their original codec until they are read back or released. The setting lasts until reboot. The v20261124 kernel shows the active codec read-only.

A root shell can use the same interface. To select Zstd and read back the active codec, run the following. Use `lz4` to switch back.

```sh
echo zstd > /sys/kernel/mm/memcomp/compressor
cat /sys/kernel/mm/memcomp/compressor
```

## MGLRU on Oplus firmware

The RMX3888 boot script checks for an `Ezr:` line in `/proc/meminfo`.
When memcomp is ready, patch 006 reports
`Ezr: 0 kB (fake for Oplus MGLRU boot check)` so the script preserves
the kernel's MGLRU setting. The marker reports zero bytes. On the
tested firmware, `/sys/kernel/mm/lru_gen/enabled` read `0x0003` after boot.

On one RMX3888 running the same LZ4 kernel before patch 006, switching
among ten commonly used apps in a fixed order gave the following Android
framework foreground timings. Each app ran eight measured times per mode.

| Metric | MGLRU off | MGLRU on |
| --- | ---: | ---: |
| Foreground P50 | 110 ms | 109 ms |
| Foreground P95 | 185 ms | 155 ms |
| Process or Activity reloads | 0/80 | 0/80 |
| Warm repeat P95 (20 of the 80 returns per mode) | 156 ms | 150 ms |

The warm repeat ran on then off. The smaller gap after warm-up limits
how much of the pooled 30 ms difference can be attributed to MGLRU.
The post-flash check on the released kernel verifies the boot setting.

## Memcomp Monitor for KernelSU

Install the optional Memcomp Monitor ZIP in KernelSU to view pool usage and change the compressor. On a supported RMX3888 profile the module also disables the vendor swap and dynamic zram switches. The **驱动修复** page can back up, patch and restore the active slot's vendor ramdisk on unlocked devices with firmware `RMX3888_16.0.3.500`. Writing the partition requires a separate confirmation in the WebUI.

See the [module guide](ksu/memcomp-monitor/README.txt) for device behavior and the [developer guide](ksu/memcomp-monitor/DEVELOPING.md) for packaging and offline checks.

## Build

Use an x86-64 Linux host for the prebuilt Clang toolchain. On Debian or Ubuntu, run this from the repository root to install the build dependencies with APT and then build the kernel. The install step uses `sudo`.

```bash
SKIP_DEPS=n ./build.sh
```

Once the host has the required packages, use the default build command.

```bash
./build.sh
```

The script downloads Clang on first use and refreshes its source trees under `workspace/`. It resets and cleans the common, vendor and AnyKernel3 checkouts on subsequent builds. On hosts with more than 16 GiB of RAM, it also recreates `/tmp/out`. Keep local changes outside the build trees and that directory.

| Variable | Default | Effect |
| --- | --- | --- |
| `MANIFEST` | `gt5pro` | Device label in the ZIP filename. The source repositories are fixed to the GT5 Pro. |
| `CUSTOM_SUFFIX` | `android16-11-o-vYYYYMMDD` | Kernel release suffix and ZIP filename suffix, using the build date by default. |
| `APPLY_DROIDSPACES` | `y` | Set to `n` to skip the DroidSpaces kABI patch and its kernel configuration. |
| `SKIP_DEPS` | `y` | Skip APT by default. Set to `n` to update the package index and install build dependencies with `sudo` before building. |

## Build outputs

| Artifact | Path |
| --- | --- |
| ARM64 kernel image | `workspace/kernel_ws/common/out/arch/arm64/boot/Image` |
| AnyKernel3 ZIP containing that image | `workspace/kernel_ws/ak3-<MANIFEST>-<CUSTOM_SUFFIX>.zip` |

The GT5 Pro loads `oplus_bsp_schedinfo.ko` from the vendor ramdisk during early boot. The only build payload added to the AnyKernel3 ZIP is the kernel `Image`. To deploy the hot-thread fix in the device test, we packaged the rebuilt module separately in a matching `vendor_boot` image.

## Validation

### DGX Spark benchmark

On DGX Spark, an ARM64 KVM guest with 4 vCPUs and 1 GiB of RAM paged out 64 MiB and refaulted 4,096 pages per run. Both backends used LZ4. Each of the 10 pairs contained one zram run and one memcomp run, with the order alternating between pairs. The table gives the median of each per-run metric.

| Metric | zram | memcomp |
| --- | ---: | ---: |
| First PAGEOUT (64 MiB) | 101.615 ms | 97.790 ms |
| Refault latency P95 | 3.650 µs | 2.460 µs |
| Refault latency P99 | 14.750 µs | 2.800 µs |

### GT5 Pro device checks

Repeated `MADV_PAGEOUT` could stall for about 5.4 s while the kernel logged invalid swapfile lookups for compressed entries. After fixing the lookup, we compared one PAGEOUT call with two calls across 40 pairs on the phone. The second call took 38.281 µs at the median and 282.656 µs at most. The median within-pair difference in refault P95 was 0.0 µs. Neither test window logged `Bad swap file entry`.

With the patched vendor module loaded, `top_hotthread` recorded all eight CPU workers during a 16 s load. The kernel log showed no `new_slab` warning during that interval.

[PR #4](https://github.com/ferstar/kernel_manifest/pull/4) records the full test conditions and regression results. The [module registry](.agents/MODULES.md) lists the patches and regression code.
