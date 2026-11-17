# Realme GT5 Pro (SM8650) Android 16 (B) Kernel Build

Automated GKI (`6.1.118`) kernel build script for Realme GT5 Pro on Android 16 (`AndroidB`), packaged with AnyKernel3.

## Features

- **Upstream Sources**: Realme GT5 Pro `AndroidB` (`vendor` + `common` pinned to exact commit hash, shallow cloned)
- **LZ4 v1.11.1**: Upgraded `lib/lz4` (`LZ4_FAST_DEC_LOOP` on AArch64 + Clang)
- **ZSTD**: Kernel `lib/zstd` backport patch (`patches/002-zstd.patch`)
- **DroidSpaces**: GKI kABI-safe IPC/Namespace/NAT support (`APPLY_DROIDSPACES=y` by default)

## Usage

```bash
./build.sh
```

### Environment Variables

| Variable | Default | Description |
| --- | --- | --- |
| `MANIFEST` | `gt5pro` | Target device identifier |
| `CUSTOM_SUFFIX` | `android16-11-o-v$(date +%Y%m%d)` | Kernel `LOCALVERSION` suffix |
| `APPLY_DROIDSPACES` | `y` | Apply DroidSpaces kABI patch & `gki_defconfig` options (`y`/`n`) |
| `SKIP_DEPS` | `y` | Skip `apt-get` dependency installation (`y`/`n`) |

## Output

Flashable AnyKernel3 zip is generated at:

```text
workspace/kernel_ws/ak3-<MANIFEST>-<CUSTOM_SUFFIX>.zip
```
