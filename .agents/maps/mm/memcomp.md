# mm/memcomp

The [backport](../../../patches/003-memcomp.patch) targets the common kernel revision selected in [build.sh](../../../build.sh). Read the patch hunk for the MM operation under review. The checked-out kernel under `workspace/kernel_ws/common` is useful for reading callers in context.

## Invariants and regression coverage

- On Oplus firmware, the [boot compatibility patch](../../../patches/006-oplus-memcomp-mglru.patch) appends an annotated zero-size `Ezr:` field to `/proc/meminfo` when MGLRU is built and memcomp is ready. The vendor `post_fs_data()` function checks for the field before writing zero to `lru_gen/enabled`. The reported size is always zero.
- A compressed PTE carries a memcomp token. Only swap-backed entries may reach `swp_swapcount()` in the madvise walk. Exercise repeated PAGEOUT and COLD on an already compressed mapping and check for `Bad swap file entry` in the same log window.
- MOVE and mincore can drop and reacquire page-table locks. Check token lifetime across retries, the PMD under the PTE lock, and the zero-token case. The [regression program](../../../tests/memcomp/regression.c) covers MOVE, residency, swap accounting and rollback.
- Keep the [busy-pin test hook](../../../tests/memcomp/pin-test.h) confined to a test kernel. Identify injected QEMU coverage separately from behavior observed on the phone.

## Runtime compressor selection

Apply the [runtime codec patch](../../../patches/005-memcomp-runtime-codec.patch) after the backport. The phone defconfig builds both crypto algorithms into the kernel. The default is `lz4`. The v10 boot option `memcomp.compressor=<crypto name>` still accepts driver names and other algorithms. Startup waits up to ten seconds for a late driver, tries `zstd` on failure, and leaves memcomp disabled if neither choice works.

- Read `/sys/kernel/mm/memcomp/compressor` for the current choice (the requested name during startup). The attribute has mode `0644`. A write accepts exactly `lz4` or `zstd`, optionally followed by one newline. Invalid bytes return `EINVAL`. A valid write during startup returns `EAGAIN`, or `ENODEV` after failed startup. Selecting the active codec succeeds. Once a different boot codec has been switched away from, it can only be selected again at boot.
- A synchronous lease snapshots the encoder and its ID together. An asynchronous batch owns its codec and returns to that codec's pool even if a switch completes. A synchronous retry uses the retrying lease's codec. The completed frame carries its producer's one-byte ID at the end, including when direct staging has already released the encoder.
- The stored length includes the ID for zsmalloc allocation, charge, raw thresholds and packed offsets. SAMEFILL, slotless zero and full-page raw data keep their original representations. Packed writeback and defrag copy the complete tagged frame. RAM faults, single-page writeback, packed faults and packed swapoff decode through `memcomp_decompress_payload()`. Swapoff recompression writes a fresh tag. The decoder checks the tag and the decompressed page length.
- The first successful selection retains one transform per existing encoder lease (at most 32) and one per possible CPU. The crypto driver determines the workspace size. Zstd's generic transform allocates both a compression and a decompression workspace. Staging, synchronous output buffers and zsmalloc are shared across codecs. With a true async driver, each provisioned batch adds 32 page buffers plus requests, up to one batch per possible CPU capped at `BITS_PER_LONG`. Without a usable batch, stores use the synchronous path.
- The sysfs mutex serializes writers. The writer fully initializes the new transforms before publishing the decoder bank and active selection with release ordering. Acquire loads pair with those publications. A failed synchronous allocation frees the partial bank and returns the crypto/allocation error. Published banks stay available until reboot for outstanding work and old pages. Repeated switches reuse them. A custom boot choice plus both runtime codecs can retain three banks.

For runtime verification, keep distinguishable nonuniform pages compressed under each codec, switch in both directions, then compare every refaulted byte. Race writes with reclaim and refault. Exercise anonymous and shmem pages, single-page and packed writeback, swapoff and defrag where feasible. Record which paths actually ran using counter deltas. Test a bad name, trailing whitespace, a second newline and embedded NUL with a length-aware write. Verify the mode, readback and byte integrity after a rejected write. For allocation-failure coverage, verify that the previous selection still works, its pages refault correctly, and a later switch can succeed.

### Reproduce a partial encoder allocation failure on ARM64 KVM

Use the [one-shot allocation hook](../../../tests/memcomp/codec-alloc-fail.h) with a completed normal KVM build of the same source. From the manifest root, set `kvm_source` and `kvm_out` to its source and output paths. The separate output copy lets both builds use the same source and configuration.

```sh
run=$(mktemp -d /tmp/gt5pro-codec-alloc-fail.XXXXXX)
cp -a "$kvm_out" "$run/out"
PATH="$PWD/workspace/toolchains/clang/bin:$PATH" \
  make -C "$kvm_source" O="$run/out" -j"$(nproc)" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  KBUILD_BUILD_USER=ferstar KBUILD_BUILD_HOST=ferstar.org \
  "CFLAGS_memcomp.o=-include $PWD/tests/memcomp/codec-alloc-fail.h" Image
```

Boot `$run/out/arch/arm64/boot/Image` with `memcomp_test.fail_zstd_alloc=1` and the default LZ4 compressor. Keep an LZ4 page compressed, then write `zstd`. The hook saves the first real encoder, returns `-ENOMEM` for encoder 2 and disarms. Expect `memcomp-test: injecting -ENOMEM at zstd encoder 2` followed by `memcomp-test: rollback freed zstd encoder 1`; the latter is emitted after the real free returns. Check the write errno, LZ4 readback and old page contents. Retry the same write and check Zstd readback and refault. Omitting the command-line option leaves allocation behavior unchanged. The phone build uses the production patch alone.

Build [the fault-test init](../../../tests/memcomp/codec-alloc-fail.c) as a static ARM64 executable with `-O2 -pthread`, name it `init` at the root of a newc initramfs, and boot with `rdinit=/init`. The test expects 4 KiB pages, a 1 GiB guest, debug statistics and no active swap device. Under memory pressure it checks the write error, actual codec, per-page data, PAGEOUT/refault progress and a successful subsequent switch. Use a console log level of 7 to capture the injection and cleanup messages.

## Compare like with like

Use the same input, codec and page count for a backend comparison. Record the guest CPU and memory settings, run order and pair count. Report first PAGEOUT and refault percentiles separately. For a retry tail, record how many runs actually retried and time the second syscall separately from any wait before it. Use a paired comparison of the same backend before and after a fix to check for a refault regression.

[PR #4](https://github.com/ferstar/kernel_manifest/pull/4) records the original DGX Spark ARM64 comparison, the repeat-PAGEOUT fix and the GT5 Pro device checks.
