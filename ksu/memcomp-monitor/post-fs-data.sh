#!/system/bin/sh
set -u
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd) || exit 1
profile=/my_stock/etc/extension/sys_osense_memory_config.xml
[ "$(getprop ro.product.model)" = RMX3888 ] && [ -r /sys/kernel/mm/memcomp/stat ] || exit 0
[ -n "$(cat /sys/kernel/mm/memcomp/compressor 2>/dev/null)" ] && [ -r "$profile" ] || exit 0
if awk -v path="$profile" '$5==path {mounted=1} END{exit !mounted}' /proc/self/mountinfo; then exit 0; fi
work=$(mktemp -d /dev/gt5-mc-profile.XXXXXX) || exit 1
cleanup() { rm -f "$work/osense.xml"; rmdir "$work"; }
trap cleanup EXIT
/system/bin/cp --preserve=all "$profile" "$work/osense.xml" || exit 1
if ! awk -f "$root/osense.awk" "$profile" > "$work/osense.xml"; then
    /system/bin/log -p w -t MemcompMonitor "Unrecognized OSense profile"
    exit 0
fi
cmp -s "$profile" "$work/osense.xml" && exit 0
/system/bin/mount --bind "$work/osense.xml" "$profile" || exit 1
trap - EXIT
/system/bin/log -t MemcompMonitor "Applied OSense compatibility profile"
