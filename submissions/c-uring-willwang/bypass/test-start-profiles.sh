#!/usr/bin/env bash
# Exercise the entry point without opening sockets or requiring capabilities.
set -euo pipefail
entry=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
mkdir "$scratch/bin"
cp "$entry/start.sh" "$scratch/start.sh"
for name in server server-bypass server-bypass-fixed; do
    cat > "$scratch/bin/$name" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "${0##*/}" "$@"
STUB
    chmod +x "$scratch/bin/$name"
done

route() {
    local expected=$1 i
    shift
    local -a result argv=("$@")
    if [[ ${argv[0]:-} == --bypass ]]; then argv=("${argv[@]:1}"); fi
    mapfile -t result < <("$scratch/start.sh" "$@")
    [[ ${result[0]:-} == "$expected" && ${#result[@]} == $((${#argv[@]} + 1)) ]]
    for i in "${!argv[@]}"; do [[ ${result[i+1]} == "${argv[i]}" ]]; done
}
route server
route server-bypass-fixed --bypass --mode xdp --packet-gso --interface 'test interface'
route server-bypass-fixed --bypass --packet-rx-frames=512
route server-bypass-fixed --bypass -- --packet-tx-frames=512
route server-bypass --bypass --packet-tx-frames 512
route server-bypass --bypass --mode packet --packet-tx-frames=4096
route server-bypass --bypass --packet-t=512
for help in --h --he --hel --help; do
    route server-bypass --bypass "$help"
done
if "$scratch/start.sh" unknown >/dev/null 2>&1; then exit 1; fi
if [[ -x $entry/bin/server-bypass-fixed && -x $entry/bin/server-bypass ]]; then
    "$entry/start.sh" --bypass --help | grep -q -- '--packet-tx-frames'
    if "$entry/bin/server-bypass-fixed" --help | grep -q -- '--packet-tx-frames'; then exit 1; fi
fi
echo 'Native, fixed 512-slot, configurable-ring and help dispatch passed.'
