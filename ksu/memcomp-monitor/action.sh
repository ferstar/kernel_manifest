#!/system/bin/sh
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd) || exit 1
data=$(sh "$root/snapshot.sh") || { echo 'Kernel counters unavailable'; exit 1; }
printf '%s\n' "$data" | awk '
{ v[$1]=$2 }
END {
    if (!v["available"]) { print "memcomp: unavailable"; exit }
    printf "memcomp (%s)  %s\n", v["codec"], v["kernel"]
    printf "Logical %.1f MiB | Compressed payload %.1f MiB | Pool %.1f MiB\n",v["logical"]/1048576,v["payload"]/1048576,v["pool"]/1048576
    printf "Logical minus pool: %.1f MiB (excludes metadata outside the pool)\n",(v["logical"]-v["pool"])/1048576
    if (v["payload"]>0) printf "Payload compression ratio: %.2fx\n",v["logical"]/v["payload"]
    if ("bg_store_ok" in v) printf "Successful stores: background %s, direct %s, other/pageout %s\n",v["bg_store_ok"],v["direct_store_ok"],v["pageout_store_ok"]
    printf "OSense profile mount: %s\n",v["policy"]
}'
