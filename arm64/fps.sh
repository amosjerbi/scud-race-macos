#!/bin/sh
# fps.sh <binary> <seconds> : average presented fps over the second half of a run (attract mode)
B="$1"; T="${2:-30}"; L=$(mktemp)
SCUDRACE_HOME="${SCUDRACE_HOME:-$(cd "$(dirname "$0")/../native-mac/game" && pwd)}" SCUD_HOST_FPS=1 "$B" > "$L" 2>&1 & P=$!
sleep "$T"; kill $P 2>/dev/null; wait $P 2>/dev/null
grep "presented" "$L" | awk '{v[NR]=$3} END {s=0; n=0; for (i=int(NR/2)+1; i<=NR; i++) {s+=v[i]; n++} printf "%.1f fps (last %d samples)\n", n?s/n:0, n}'
grep -E "FATAL|CRASH" "$L"; rm -f "$L"
