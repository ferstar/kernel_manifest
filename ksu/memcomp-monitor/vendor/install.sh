#!/system/bin/sh
set -eu
umask 077
DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
STOCK=91e86c047df59ad99c8586ed7b211a6f0ef406fbba00090d5f484ddebe20abbb
FIXED=b118553adfe9aca055a60481e263dac6b714e091f97960e27fb78119322350ed
fail() { echo "schedinfo: $*" >&2; exit 1; }
digest() { local line; line=$(sha256sum "$1") || return 1; printf '%s\n' "${line%% *}"; }

mode=${1:-}
case "$mode" in
    --image) [ "$#" = 3 ] || fail "--image STOCK.img NEW.img"
        exec "$DIR/patch-image" "$2" "$DIR/oplus_bsp_schedinfo.ko" "$3" ;;
    --check-device) [ "$#" = 1 ] || fail "unexpected arguments" ;;
    --install|--restore) [ "$#" = 2 ] || fail "supply the confirmed slot (_a or _b)" ;;
    *) fail "choose --check-device, --image, --install or --restore" ;;
esac
[ "$(id -u)" = 0 ] || fail "root required"
[ "$(getprop ro.product.model)" = RMX3888 ] || fail "expected GT5 Pro RMX3888"
bootarg() { awk -F'"' -v key="androidboot.$1" '$1==key" = " {print $2}' /proc/bootconfig; }
[ "$(bootarg vbmeta.device_state)" = unlocked ] || fail "unlocked bootloader required"
slot=$(bootarg slot_suffix)
case "$slot" in _a|_b) ;; *) fail "unknown active slot" ;; esac
[ "$mode" = --check-device ] || [ "$2" = "$slot" ] || fail "active slot changed"
part=/dev/block/by-name/vendor_boot$slot
[ -b "$part" ] && [ "$(blockdev --getsize64 "$part")" = 201326592 ] || fail "unknown partition"
backupdir=/data/adb/gt5-memcomp-backup
private=$backupdir/vendor_boot${slot}-stock-v20261124.img
find_stock() {
    local path
    for path in "$private" "/data/media/0/diy/vendor_boot${slot}-stock-v20261124.img" "/data/media/0/diy/vendor_boot${slot}-RMX3888_16.0.3.500-original.img"; do
        if [ -f "$path" ] && [ "$(digest "$path")" = "$STOCK" ]; then printf '%s\n' "$path"; return 0; fi
    done
    return 1
}
current=$(digest "$part")
case "$current" in
    "$STOCK") state=stock ;;
    "$FIXED") state=fixed ;;
    *) state=unknown ;;
esac
if [ "$mode" = --check-device ]; then
    backup=missing
    if find_stock >/dev/null; then backup=verified; fi
    printf 'slot %s\nimage %s\nbackup %s\n' "$slot" "$state" "$backup"
    exit 0
fi
[ "$state" != unknown ] || fail "unrecognized image SHA-256: $current"
case "$mode:$state" in --install:fixed|--restore:stock) echo "Slot $slot already matches the requested image"; exit 0 ;; esac
[ -d /data/adb ] && [ -d /data/local/tmp ] || fail "decrypted /data required"
lock=/dev/gt5-schedinfo-install.lock
mkdir "$lock" || fail "another operation is active"
work=
partial=
cleanup() {
    [ -z "$partial" ] || rm -f "$partial"
    if [ -n "$work" ]; then rm -f "$work/before.img" "$work/new.img"; rmdir "$work" || true; fi
    rmdir "$lock" || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
work=$(mktemp -d /data/local/tmp/gt5-vendor.XXXXXX)
dd if="$part" of="$work/before.img" bs=4M conv=fsync
[ "$(digest "$work/before.img")" = "$current" ] || fail "pre-write snapshot mismatch"

if [ "$mode" = --install ]; then
    expected=$FIXED
    mkdir -p "$backupdir"
    if [ ! -f "$private" ]; then
        partial=$(mktemp "$backupdir/.stock.XXXXXX")
        dd if="$work/before.img" of="$partial" bs=4M conv=fsync
        [ "$(digest "$partial")" = "$STOCK" ] || fail "could not verify stock backup"
        mv "$partial" "$private"
        partial=
    fi
    [ "$(digest "$private")" = "$STOCK" ] || fail "private backup mismatch"
    "$DIR/patch-image" "$work/before.img" "$DIR/oplus_bsp_schedinfo.ko" "$work/new.img"
else
    expected=$STOCK
    original=$(find_stock) || fail "no verified stock backup for this slot"
    # Stage inside a root-private directory before writing any shared input.
    dd if="$original" of="$work/new.img" bs=4M conv=fsync
fi
[ "$(digest "$work/new.img")" = "$expected" ] || fail "candidate mismatch"
[ "$(bootarg slot_suffix)" = "$slot" ] && [ "$(digest "$part")" = "$current" ] || fail "slot or partition changed"
trap '' HUP INT TERM
if dd if="$work/new.img" of="$part" bs=4M conv=fsync && [ "$(digest "$part")" = "$expected" ]; then
    echo "Verified vendor_boot$slot. Reboot when ready."
elif [ "$(digest "$work/before.img")" = "$current" ] && dd if="$work/before.img" of="$part" bs=4M conv=fsync && [ "$(digest "$part")" = "$current" ]; then
    fail "write failed; restored and verified the pre-operation image"
else
    # Keep the verified input available if even recovery of the block failed.
    echo "Write and rollback failed. Pre-operation snapshot: $work/before.img" >&2
    work=
    exit 1
fi
