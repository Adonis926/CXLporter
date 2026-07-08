#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
WSK=${WSK:-$ROOT/wsk}
ACTION=${ACTION:-cxlfork_faas_workload}
RUNTIME=${RUNTIME:-nodejs:20}
FUNCTIONS=${FUNCTIONS:-"json:4194304 pyaes:4194304 html:8388608 touch:67108864"}
REQUESTS=${REQUESTS:-5}
WARMUP=${WARMUP:-1}
OUT=${OUT:-$ROOT/results/openwhisk-$(date +%Y%m%d-%H%M%S).csv}

field_json() {
  local json=$1
  local key=$2
  python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get(sys.argv[1], ""))' "$key" <<<"$json"
}

nsec_now() {
  date +%s%N
}

mkdir -p "$(dirname "$OUT")"
"$ROOT/scripts/start-standalone.sh" >/dev/null

case "$RUNTIME" in
  nodejs:*) ACTION_FILE="$ROOT/actions/faas_workload.js" ;;
  python:*) ACTION_FILE="$ROOT/actions/faas_workload.py" ;;
  *)
    echo "unsupported RUNTIME=$RUNTIME; expected nodejs:* or python:*" >&2
    exit 1
    ;;
esac

"$WSK" -i action update "$ACTION" "$ACTION_FILE" --kind "$RUNTIME" --memory 512 --timeout 300000 >/dev/null
printf 'system,runtime,function,bytes,requests,warmup,invoke_ns,ow_wait_time_ms,ow_duration_ms,ow_init_ns,ow_workload_ns,notes\n' >"$OUT"

for entry in $FUNCTIONS; do
  fn=${entry%%:*}
  bytes=${entry#*:}
  for i in $(seq 1 "$WARMUP"); do
    "$WSK" -i action invoke "$ACTION" --blocking --result -p function "$fn" -p bytes "$bytes" >/dev/null
  done

  sum_invoke=0
  sum_wait=0
  sum_duration=0
  sum_init=0
  sum_work=0
  for i in $(seq 1 "$REQUESTS"); do
    begin=$(nsec_now)
    out=$("$WSK" -i action invoke "$ACTION" --blocking --result -p function "$fn" -p bytes "$bytes")
    end=$(nsec_now)
    invoke_ns=$((end - begin))
    init_ns=$(field_json "$out" init_ns)
    workload_ns=$(field_json "$out" workload_ns)
    # Activation metadata has wait/duration; fetch latest activation for platform-side metrics.
    act=$("$WSK" -i activation list --limit 1 | awk 'NR==2 {print $3}')
    meta=$("$WSK" -i activation get "$act" 2>/dev/null || true)
    wait_ms=$(printf '%s\n' "$meta" | awk '/"waitTime"/ {gsub(/[^0-9]/,"",$2); print $2; exit}')
    duration_ms=$(printf '%s\n' "$meta" | awk '/"duration"/ {gsub(/[^0-9]/,"",$2); print $2; exit}')
    : "${wait_ms:=0}"
    : "${duration_ms:=0}"
    sum_invoke=$((sum_invoke + invoke_ns))
    sum_wait=$((sum_wait + wait_ms))
    sum_duration=$((sum_duration + duration_ms))
    sum_init=$((sum_init + init_ns))
    sum_work=$((sum_work + workload_ns))
  done
  avg_invoke=$((sum_invoke / REQUESTS))
  avg_wait=$((sum_wait / REQUESTS))
  avg_duration=$((sum_duration / REQUESTS))
  avg_init=$((sum_init / REQUESTS))
  avg_work=$((sum_work / REQUESTS))
  printf 'OpenWhisk,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,standalone OpenWhisk blocking invoke with Docker runtime\n' \
    "$RUNTIME" "$fn" "$bytes" "$REQUESTS" "$WARMUP" "$avg_invoke" "$avg_wait" "$avg_duration" "$avg_init" "$avg_work" >>"$OUT"
  printf '[OpenWhisk] runtime=%s fn=%s bytes=%s invoke_ns=%s duration_ms=%s init_ns=%s workload_ns=%s\n' \
    "$RUNTIME" "$fn" "$bytes" "$avg_invoke" "$avg_duration" "$avg_init" "$avg_work"
done

echo "results=$OUT"
