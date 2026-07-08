#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
RUNTIME_IMAGE=${RUNTIME_IMAGE:-openwhisk/action-nodejs-v20:nightly}
FUNCTIONS=${FUNCTIONS:-"json:4194304 pyaes:4194304 html:8388608 touch:67108864"}
REQUESTS=${REQUESTS:-5}
WARMUP=${WARMUP:-1}
OUT=${OUT:-$ROOT/results/local-node-$(date +%Y%m%d-%H%M%S).csv}

field_json() {
  local json=$1
  local key=$2
  python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get(sys.argv[1], ""))' "$key" <<<"$json"
}

nsec_now() {
  date +%s%N
}

mkdir -p "$(dirname "$OUT")"
printf 'system,runtime,function,bytes,requests,warmup,invoke_ns,ow_wait_time_ms,ow_duration_ms,ow_init_ns,ow_workload_ns,notes\n' >"$OUT"

for entry in $FUNCTIONS; do
  fn=${entry%%:*}
  bytes=${entry#*:}
  for i in $(seq 1 "$WARMUP"); do
    docker run --rm --entrypoint node \
      -v "$ROOT/actions:/actions:ro" \
      "$RUNTIME_IMAGE" /actions/faas_workload.js "function=$fn" "bytes=$bytes" >/dev/null
  done

  sum_invoke=0
  sum_init=0
  sum_work=0
  for i in $(seq 1 "$REQUESTS"); do
    begin=$(nsec_now)
    out=$(docker run --rm --entrypoint node \
      -v "$ROOT/actions:/actions:ro" \
      "$RUNTIME_IMAGE" /actions/faas_workload.js "function=$fn" "bytes=$bytes")
    end=$(nsec_now)
    invoke_ns=$((end - begin))
    init_ns=$(field_json "$out" init_ns)
    workload_ns=$(field_json "$out" workload_ns)
    sum_invoke=$((sum_invoke + invoke_ns))
    sum_init=$((sum_init + init_ns))
    sum_work=$((sum_work + workload_ns))
  done
  avg_invoke=$((sum_invoke / REQUESTS))
  avg_init=$((sum_init / REQUESTS))
  avg_work=$((sum_work / REQUESTS))
  printf 'LocalDockerNode,nodejs:20,%s,%s,%s,%s,%s,0,0,%s,%s,direct OpenWhisk runtime container without OpenWhisk control plane\n' \
    "$fn" "$bytes" "$REQUESTS" "$WARMUP" "$avg_invoke" "$avg_init" "$avg_work" >>"$OUT"
  printf '[LocalDockerNode] fn=%s bytes=%s invoke_ns=%s init_ns=%s workload_ns=%s\n' \
    "$fn" "$bytes" "$avg_invoke" "$avg_init" "$avg_work"
done

echo "results=$OUT"
