#!/system/bin/sh
# One read of the three counters per sample. Additional counters are optional.
base=/sys/kernel/mm/memcomp
profile=/my_stock/etc/extension/sys_osense_memory_config.xml
printf 'kernel %s\n' "$(uname -r)"
awk '{print "uptime", $1}' /proc/uptime
if [ ! -r "$base/stat" ]; then
    echo "available 0"
    exit 0
fi
IFS=' ' read -r logical payload pool rest < "$base/stat" || exit 1
for n in "$logical" "$payload" "$pool"; do
    case "$n" in ''|*[!0-9]*) exit 1;; esac
done
[ -z "$rest" ] || exit 1
printf 'available 1\nlogical %s\npayload %s\npool %s\n' "$logical" "$payload" "$pool"
if [ -r "$base/compressor" ]; then printf 'codec %s\n' "$(cat "$base/compressor")"; fi
if [ -r "$base/events" ]; then
    awk '$1 ~ /^(bg_store_ok|direct_store_ok|pageout_store_ok|incompressible|compress_fail)$/ && $2 ~ /^[0-9]+$/ {print $1, $2}' "$base/events"
fi
if [ -r /proc/self/mountinfo ]; then
    if [ -r "$profile" ]; then
        awk -v p="$profile" '$5==p {state=(index($4, "/gt5-mc-profile.") == 1 && $4 ~ /\/osense[.]xml$/ ? "applied" : "external")} END {print "policy", (state ? state : "stock")}' /proc/self/mountinfo
    else echo "policy unavailable"; fi
fi
