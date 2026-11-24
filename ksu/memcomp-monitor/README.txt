Memcomp Monitor for Realme GT5 Pro (RMX3888). Install in KernelSU and
reboot. Open WebUI for live monitoring or Action for a text snapshot.

The page reads the kernel every five seconds while visible. Samples stay
in memory and are discarded when the page closes. Logical bytes, compressed
payload and the zsmalloc pool come from /sys/kernel/mm/memcomp/stat. Pool
savings exclude metadata outside zsmalloc. Reclaim activity uses deltas of
kernel store counters; it does not measure app latency or battery savings.

The home page shows the current compressor. Mode 0444 (v10) is read-only.
On a compatible 0644 kernel, tap LZ4 or Zstd on the home page to switch.
The helper accepts only lz4 and zstd, checks the mode on every request,
and confirms the selection by reading it back. The setting applies to
subsequent compression this boot. Previously compressed pages keep their
codec. Reboot restores the kernel default; the module saves no selection.

On RMX3888 with memcomp, the boot hook disables the OSense swap, dynamic
zram and optional cMem switches when present. OEM thermal memory, game,
camera and background policies stay at stock settings. Unsupported profiles
are skipped. Disable or remove the module and reboot to undo its mount.

The 驱动修复 page can inspect, apply or restore oplus_bsp_schedinfo.ko for
RMX3888_16.0.3.500 on an unlocked device. Installing the module, opening
the WebUI and refreshing memory stats never write a partition. Opening 驱动修复 checks the slot and the backup automatically.
Confirm the operation and its target in the dialog to write.
Stock backups live in /data/adb/gt5-memcomp-backup, outside the module.
A matching original image in /sdcard/diy is also accepted for restore.
Each candidate and partition readback is SHA-256 verified. No automatic
reboot occurs. Source for the image builder is in vendor/src/.
