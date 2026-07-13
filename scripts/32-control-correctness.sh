#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
PORTER=${PORTER:-$WS/cxlporter}
VM0_PORT=${VM0_PORT:-2200}
VM1_PORT=${VM1_PORT:-2201}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2301}
KEY=${KEY:-94000}
BYTES=${BYTES:-4194304}
POOL=${POOL:-4}
OUTDIR=${OUTDIR:-$PORTER/results/control-correctness-$(date +%Y%m%d-%H%M%S)}
B=$PORTER/cxlporter-control
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

vm_ssh() {
  local port=$1
  shift
  ssh "${SSH_OPTS[@]}" -p "$port" ubuntu@localhost "$@"
}

field() {
  local text=$1
  local name=$2
  awk -v key="$name" '{for (i=1;i<=NF;i++) {split($i,a,"="); if (a[1]==key) value=a[2]}} END{print value}' <<<"$text"
}

mkdir -p "$OUTDIR"
POOL=$POOL "$PORTER/scripts/31-start-control-plane.sh" >"$OUTDIR/start.log" 2>&1

# Clean only the selected test key, using its observed generation.
if old=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control inspect '$KEY'" 2>/dev/null); then
  old_generation=$(field "$old" generation)
  vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control release '$KEY' '$old_generation'" >/dev/null
fi

checkpoint=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control checkpoint '$KEY' touch '$BYTES' 2")
printf '%s\n' "$checkpoint" | tee "$OUTDIR/checkpoint.log"
generation=$(field "$checkpoint" generation)
bytes=$(field "$checkpoint" bytes)
page_checksum=$(field "$checkpoint" page_checksum)
workload_checksum=$(field "$checkpoint" workload_checksum)

register=$($B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 REGISTER id=correct-register \
  function=touch key="$KEY" generation="$generation" bytes="$bytes" \
  page_checksum="$page_checksum" workload_checksum="$workload_checksum")
printf '%s\n' "$register" | tee "$OUTDIR/register.log"

$B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 INVOKE id=correct-direct \
  function=touch policy=mow mode=invoke validation=fast | tee "$OUTDIR/direct.log"

"$PORTER/openwhisk/scripts/42-deploy-cxlporter-action.sh" | tee "$OUTDIR/action-deploy.log"
python3 "$PORTER/openwhisk/scripts/control_benchmark.py" \
  --path openwhisk --function touch --policy mow --mode invoke --bytes "$BYTES" \
  --samples 3 --warmup 1 --pool-size "$POOL" \
  --output "$OUTDIR/openwhisk-smoke.csv" | tee "$OUTDIR/openwhisk-smoke.log"

# Miss and stale-generation requests must fail without modifying the registry.
set +e
miss=$($B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 INVOKE id=correct-miss \
  function=json policy=mow mode=invoke validation=fast 2>&1)
miss_rc=$?
stale=$($B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 REGISTER id=correct-stale \
  function=touch key="$KEY" generation="$((generation + 1))" bytes="$bytes" \
  page_checksum="$page_checksum" workload_checksum="$workload_checksum" 2>&1)
stale_rc=$?
malformed=$($B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 INVOKE id=correct-malformed \
  function=touch policy=mow mode=invoke validation=fast unknown=1 2>&1)
malformed_rc=$?
set -e
printf '%s\n%s\n%s\n' "$miss" "$stale" "$malformed" | tee "$OUTDIR/failures.log"
[[ $miss_rc -ne 0 && "$miss" == *"status=2"* ]]
[[ $stale_rc -ne 0 && "$stale" == *"status=116"* ]]
[[ $malformed_rc -ne 0 && "$malformed" == *"error_stage=parse"* ]]

python3 "$PORTER/openwhisk/scripts/control_benchmark.py" \
  --path direct --function touch --policy mow --mode invoke --bytes "$BYTES" \
  --samples 16 --warmup 2 --concurrency 8 --pool-size "$POOL" \
  --output "$OUTDIR/concurrent.csv" | tee "$OUTDIR/concurrent.log"

# Hold a restored VMA long enough to prove active-reader release protection.
$B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 INVOKE id=correct-hold \
  function=touch policy=mow mode=invoke validation=fast hold_ms=1500 \
  >"$OUTDIR/hold.log" 2>&1 &
hold_pid=$!
sleep 0.3
active=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control inspect '$KEY'")
printf '%s\n' "$active" | tee "$OUTDIR/active-reader.log"
readers=$(field "$active" readers)
[[ "$readers" -ge 1 ]]
set +e
busy=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control release '$KEY' '$generation'" 2>&1)
busy_rc=$?
set -e
printf '%s\n' "$busy" | tee "$OUTDIR/active-release.log"
[[ $busy_rc -ne 0 && "$busy" == *"Device or resource busy"* ]]
wait "$hold_pid"

after=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control inspect '$KEY'")
printf '%s\n' "$after" | tee "$OUTDIR/readers-after.log"
[[ "$(field "$after" readers)" == "0" ]]
vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control release '$KEY' '$generation'" \
  | tee "$OUTDIR/final-release.log"

for port in "$VM0_PORT" "$VM1_PORT"; do
  log="$OUTDIR/dmesg-$port.log"
  vm_ssh "$port" "sudo dmesg" >"$log"
  if grep -E 'BUG:|WARNING:|Oops:|use-after-free|Bad pagetable|general protection fault' "$log"; then
    echo "kernel error found in $log" >&2
    exit 1
  fi
done

echo "CONTROL_CORRECTNESS_OK outdir=$OUTDIR generation=$generation"
