#!/system/bin/sh
# Fixed sysfs endpoint. Never probe capability with a write or root's test -w.
set -u
node=/sys/kernel/mm/memcomp/compressor
fail() { printf '%s\n' "$1" >&2; exit 1; }

case "$#:${1:-}" in
    1:status) ;;
    2:set)
        case "$2" in lz4|zstd) ;; *) fail '算法参数无效。';; esac ;;
    *) fail '用法：compressor.sh status 或 set lz4|zstd。' ;;
esac

read_state() {
    current=$(cat "$node") || fail '无法读取内核的 compressor 节点。'
    case "$current" in ''|*[!a-zA-Z0-9_-]*) fail '内核返回了无法识别的算法名称。';; esac
    mode=$(stat -c %a "$node" 2>/dev/null) || mode=unknown
    case "$mode" in
        644|0644)
            case "$current" in lz4|zstd) access=runtime;; *) access=unknown;; esac ;;
        444|0444) access="readonly" ;;
        *) access=unknown ;;
    esac
}

read_state
if [ "$1" = set ]; then
    case "$access" in
        runtime) ;;
        readonly) fail '当前内核只读，切换算法需要新版内核。' ;;
        *) fail '无法确认内核支持切换，已停止写入。' ;;
    esac
    if [ "$current" != "$2" ]; then
        printf '%s\n' "$2" > "$node" || fail '内核写入失败，请核对当前算法。'
    fi
    read_state
    [ "$current" = "$2" ] && [ "$access" = runtime ] || fail '回读与请求不符，请核对当前算法。'
fi
printf 'access %s\ncodec %s\n' "$access" "$current"
