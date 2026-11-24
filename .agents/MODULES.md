# Module registry

Paths are relative to the repository root. Use the IDs below in commit decisions. Follow the module that owns the behavior as well as any shared build wiring.

- `build` — [build script](../build.sh), [usage and packaging](../README.md) and [.gitignore](../.gitignore). Owns source selection, patch order, kernel configuration and the AnyKernel3 archive.
- `kernel/codecs` — [LZ4](../patches/001-lz4.patch) and [Zstd](../patches/002-zstd.patch) backports in the common kernel.
- `kernel/kabi` — [SYSVIPC ABI padding](../patches/001.GKI-below-6.12-fix_sysvipc_kabi_6_7_8.patch) and the corresponding DroidSpaces configuration in `build.sh`.
- `mm/memcomp` — [backport](../patches/003-memcomp.patch), [runtime codec selection](../patches/005-memcomp-runtime-codec.patch), [Oplus MGLRU boot marker](../patches/006-oplus-memcomp-mglru.patch), [regression program](../tests/memcomp/regression.c), [test-only hooks](../tests/memcomp/pin-test.h) and backend selection in `build.sh`. See the [memcomp map](maps/mm/memcomp.md) for invariants and benchmark comparisons.
- `vendor/schedinfo` — [hot-thread allocation patch](../patches/004-schedinfo-hotthread.patch) and its application to the vendor tree. For a hot-thread fix, match workload PIDs in `top_hotthread` and check the kernel log from the same interval.
- `harness` — [repository instructions](../AGENTS.md), this registry and the module maps under `.agents/maps/`.
