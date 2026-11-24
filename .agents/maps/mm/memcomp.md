# mm/memcomp

The [backport](../../../patches/003-memcomp.patch) targets the common kernel revision selected in [build.sh](../../../build.sh). Read the patch hunk for the MM operation under review. The checked-out kernel under `workspace/kernel_ws/common` is useful for reading callers in context.

## Invariants and regression coverage

- A compressed PTE carries a memcomp token. Only swap-backed entries may reach `swp_swapcount()` in the madvise walk. Exercise repeated PAGEOUT and COLD on an already compressed mapping and check for `Bad swap file entry` in the same log window.
- MOVE and mincore can drop and reacquire page-table locks. Check token lifetime across retries, the PMD under the PTE lock, and the zero-token case. The [regression program](../../../tests/memcomp/regression.c) covers MOVE, residency, swap accounting and rollback.
- Keep the [busy-pin test hook](../../../tests/memcomp/pin-test.h) confined to a test kernel. Identify injected QEMU coverage separately from behavior observed on the phone.

## Compare like with like

Use the same input, codec and page count for a backend comparison. Record the guest CPU and memory settings, run order and pair count. Report first PAGEOUT and refault percentiles separately. For a retry tail, record how many runs actually retried and time the second syscall separately from any wait before it. Use a paired comparison of the same backend before and after a fix to check for a refault regression.

[PR #4](https://github.com/ferstar/kernel_manifest/pull/4) records the original DGX Spark ARM64 comparison, the repeat-PAGEOUT fix and the GT5 Pro device checks.
