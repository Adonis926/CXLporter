#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
PORTER=${PORTER:-$WS/cxlporter}
ROOT=${ROOT:-$PORTER/openwhisk}
BENCH=${BENCH:-$ROOT/scripts/paper_source_benchmark.py}
SUMMARIZER=${SUMMARIZER:-$ROOT/scripts/paper_source_summarize.py}
KEY=${KEY:-2472401}
SAMPLES=${SAMPLES:-1000}
WARMUP=${WARMUP:-20}
BURST_SAMPLES=${BURST_SAMPLES:-1000}
BURST_RPS=${BURST_RPS:-150}
BURST_CONCURRENCY=${BURST_CONCURRENCY:-64}
RUN_ID=${RUN_ID:-paper-source-$(date +%Y%m%d-%H%M%S)}
OUTDIR=${OUTDIR:-$WS/results/paper-system/$RUN_ID}
APIHOST=${APIHOST:-http://127.0.0.1:3433}
AUTH=${AUTH:-23bc46b1-71f6-4ed5-8c54-816aa4f8c502:123zO3xZCLrMN6v2BKK1dXYFpXlPkccOFqm12CdAsMgRU4VrNZ9lyGVCGuMDGIwP}
ACTION=${ACTION:-paper_cxlfork_source}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2501}
INVOKES_CONCURRENT=${INVOKES_CONCURRENT:-128}
ACTION_MEMORY=${ACTION_MEMORY:-128}
ACTION_LOGSIZE=${ACTION_LOGSIZE:-0}
ACTION_CONCURRENCY=${ACTION_CONCURRENCY:-1}

mkdir -p "$OUTDIR"
exec > >(tee "$OUTDIR/benchmark.log") 2>&1

echo "event=begin run_id=$RUN_ID outdir=$OUTDIR"
INVOKES_CONCURRENT="$INVOKES_CONCURRENT" \
  "$ROOT/scripts/51-start-paper-source.sh"
OPENWHISK_LOG="$ROOT/paper-source/openwhisk-standalone.log"
OPENWHISK_LOG_START=$(wc -l <"$OPENWHISK_LOG")
ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -p 2400 ubuntu@127.0.0.1 \
  "sudo /tmp/cxlfork_capsule_k0 catalog-lookup '$KEY'"
START=0 ACTION="$ACTION" OWNER=paper FUNCTION=capsule-k0 VERSION=v1 \
  ACTION_MEMORY="$ACTION_MEMORY" ACTION_LOGSIZE="$ACTION_LOGSIZE" \
  ACTION_CONCURRENCY="$ACTION_CONCURRENCY" \
  "$ROOT/scripts/53-deploy-paper-source-action.sh"

{
  echo "run_id=$RUN_ID"
  echo "started_at=$(date --iso-8601=seconds)"
  echo "main_commit=$(git -C "$WS" rev-parse HEAD)"
  echo "kernel_commit=$(git -C "$WS/kernel/linux-6.6.32" rev-parse HEAD)"
  echo "cxlporter_commit=$(git -C "$PORTER" rev-parse HEAD)"
  echo "openwhisk_source_commit=$(git -C /home/xtl/cxlfork/openwhisk-7ef091c-paper rev-parse HEAD)"
  echo "openwhisk_jar_sha256=$(sha256sum "$ROOT/paper-source/openwhisk-standalone-cxlfork.jar" | awk '{print $1}')"
  echo "capsule_key=$KEY"
  echo "samples=$SAMPLES"
  echo "warmup=$WARMUP"
  echo "burst_samples=$BURST_SAMPLES"
  echo "burst_rps=$BURST_RPS"
  echo "burst_concurrency=$BURST_CONCURRENCY"
  echo "openwhisk_invokes_concurrent=$INVOKES_CONCURRENT"
  echo "action_memory_mb=$ACTION_MEMORY"
  echo "action_logsize_mb=$ACTION_LOGSIZE"
  echo "action_concurrency=$ACTION_CONCURRENCY"
  echo "host_kernel=$(uname -r)"
} >"$OUTDIR/environment.txt"
cp "$ROOT/paper-source/build-metadata.txt" "$OUTDIR/openwhisk-build-metadata.txt"

run_case() {
  local scenario=$1
  local path=$2
  local samples=$3
  local concurrency=$4
  local rps=$5
  echo "event=case scenario=$scenario path=$path samples=$samples concurrency=$concurrency rps=$rps"
  python3 "$BENCH" \
    --scenario "$scenario" \
    --path "$path" \
    --samples "$samples" \
    --warmup "$WARMUP" \
    --concurrency "$concurrency" \
    --offered-rps "$rps" \
    --run-id "$RUN_ID-$scenario" \
    --output "$OUTDIR/$scenario.csv" \
    --raw-output "$OUTDIR/$scenario-raw.jsonl" \
    --control-host "$CONTROL_HOST" \
    --control-port "$CONTROL_PORT" \
    --apihost "$APIHOST" \
    --auth "$AUTH" \
    --action "$ACTION"
}

run_case direct-sequential direct "$SAMPLES" 1 0
run_case openwhisk-sequential openwhisk "$SAMPLES" 1 0
run_case direct-150rps direct "$BURST_SAMPLES" "$BURST_CONCURRENCY" "$BURST_RPS"
run_case openwhisk-150rps openwhisk "$BURST_SAMPLES" "$BURST_CONCURRENCY" "$BURST_RPS"

python3 "$SUMMARIZER" \
  "$OUTDIR/direct-sequential.csv" \
  "$OUTDIR/openwhisk-sequential.csv" \
  "$OUTDIR/direct-150rps.csv" \
  "$OUTDIR/openwhisk-150rps.csv" \
  --output "$OUTDIR/summary.csv" \
  --json-output "$OUTDIR/summary.json" \
  --analysis "$OUTDIR/analysis.md"
cat "$OUTDIR/analysis.md"

tail -n +$((OPENWHISK_LOG_START + 1)) "$OPENWHISK_LOG" \
  >"$OUTDIR/openwhisk-run.log"
grep "creating source-integrated CXLP/2 container" \
  "$OUTDIR/openwhisk-run.log" \
  >"$OUTDIR/source-factory-markers.log"
if grep -E 'docker logs wsk.*paper_cxlfork|collectLogs_error|Failed during use of warm container|http_post.429' \
  "$OUTDIR/openwhisk-run.log" >"$OUTDIR/openwhisk-run-errors.log"; then
  echo "OpenWhisk source path emitted a forbidden log/lifecycle/throttle error" >&2
  exit 1
fi
docker ps -a --format '{{.Names}} {{.Image}} {{.Status}}' \
  >"$OUTDIR/docker-containers.txt"

for port in 2400 2401; do
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -p "$port" ubuntu@127.0.0.1 sudo dmesg >"$OUTDIR/dmesg-$port.log"
  if grep -Ei 'BUG:|WARNING:|Oops:|kernel panic|use-after-free|Bad pagetable|general protection fault|cxlfork.*(error|fail)' \
    "$OUTDIR/dmesg-$port.log" >"$OUTDIR/dmesg-$port-errors.log"; then
    echo "kernel log check failed for guest port $port" >&2
    exit 1
  fi
done

make -C "$PORTER" cxlporter-control cxlporter-paper >/dev/null
scp -q -o BatchMode=yes -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null -P 2401 \
  "$PORTER/cxlporter-control" ubuntu@127.0.0.1:/tmp/cxlporter-control
{
  "$PORTER/cxlporter-paper" client \
    "$CONTROL_HOST" "$CONTROL_PORT" POOL_STATUS 18446744073709551000
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -p 2401 ubuntu@127.0.0.1 \
    "chmod +x /tmp/cxlporter-control
     sudo /tmp/cxlporter-control inspect '$KEY'
     sudo docker logs --tail 20 cxlfork-ghost-paper
     sudo docker top cxlfork-ghost-paper -eo pid,ppid,nlwp,stat,comm"
} >"$OUTDIR/postflight-audit.txt"
grep -q 'workers=16 verify=ok' "$OUTDIR/postflight-audit.txt"
grep -q 'readers=0 .* verify=ok' "$OUTDIR/postflight-audit.txt"
if awk 'NR > 1 && $4 ~ /^Z/ { found=1 } END { exit found ? 0 : 1 }' \
  "$OUTDIR/postflight-audit.txt"; then
  echo "postflight found a zombie process" >&2
  exit 1
fi

(
  cd "$OUTDIR"
  find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\0' |
    sort -z |
    xargs -0 -r sha256sum -- >SHA256SUMS
)
echo "event=complete verify=ok outdir=$OUTDIR"
