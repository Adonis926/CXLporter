#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
PORTER=${PORTER:-$WS/cxlporter}
VM0_PORT=${VM0_PORT:-2200}
VM1_PORT=${VM1_PORT:-2201}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2301}
KEY=${KEY:-95000}
FOOTPRINTS=${FOOTPRINTS:-"4194304 67108864 201326592"}
POLICIES=${POLICIES:-"mow moa hybrid"}
POOL_SIZES=${POOL_SIZES:-"1 4 8"}
MICRO_SAMPLES=${MICRO_SAMPLES:-1000}
SCALING_SAMPLES=${SCALING_SAMPLES:-100}
CONCURRENCY_SAMPLES=${CONCURRENCY_SAMPLES:-1000}
WARMUP=${WARMUP:-20}
OPENWHISK_SCALING=${OPENWHISK_SCALING:-1}
RUN_ID=${RUN_ID:-cohort-control-$(date +%Y%m%d-%H%M%S)}
OUTDIR=${OUTDIR:-$PORTER/results/$RUN_ID}
B=$PORTER/cxlporter-control
BENCH=$PORTER/openwhisk/scripts/control_benchmark.py
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
COMBINED=$OUTDIR/control-samples.csv
MANIFEST=$OUTDIR/checkpoint-manifest.csv

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

append_samples() {
  local file=$1
  if [[ ! -f "$COMBINED" ]]; then
    cp "$file" "$COMBINED"
  else
    tail -n +2 "$file" >>"$COMBINED"
  fi
}

run_bench() {
  local path=$1 bytes=$2 policy=$3 mode=$4 samples=$5 concurrency=$6 pool=$7 tag=$8
  local file=$OUTDIR/samples-$tag.csv
  python3 "$BENCH" --path "$path" --function touch --bytes "$bytes" \
    --policy "$policy" --mode "$mode" --validation fast --samples "$samples" \
    --warmup "$WARMUP" --concurrency "$concurrency" --pool-size "$pool" \
    --run-id "$RUN_ID-$tag" --output "$file"
  append_samples "$file"
}

release_if_present() {
  local old old_generation
  if old=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control inspect '$KEY'" 2>/dev/null); then
    old_generation=$(field "$old" generation)
    vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control release '$KEY' '$old_generation'" >/dev/null
  fi
}

checkpoint_and_register() {
  local bytes=$1 pool=$2 checkpoint generation page_checksum workload_checksum actual_bytes
  checkpoint=$(vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control checkpoint '$KEY' touch '$bytes' 2")
  printf '%s\n' "$checkpoint" >>"$OUTDIR/checkpoints.log"
  generation=$(field "$checkpoint" generation)
  actual_bytes=$(field "$checkpoint" bytes)
  page_checksum=$(field "$checkpoint" page_checksum)
  workload_checksum=$(field "$checkpoint" workload_checksum)
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    touch "$KEY" "$generation" "$(field "$checkpoint" slot)" "$actual_bytes" \
    "$(field "$checkpoint" pages)" "$page_checksum" "$workload_checksum" \
    "$(field "$checkpoint" total_ns)" "$(field "$checkpoint" lock_wait_ns)" \
    "$(field "$checkpoint" lifecycle_ns)" "$(field "$checkpoint" vma_preflight_ns)" \
    "$(field "$checkpoint" pte_walk_ns)" "$(field "$checkpoint" gup_ns)" \
    "$(field "$checkpoint" page_copy_ns)" "$(field "$checkpoint" pte_recheck_ns)" \
    "$(field "$checkpoint" metadata_ns)" "$(field "$checkpoint" checksum_ns)" \
    "$(field "$checkpoint" publish_ns)" "$(field "$checkpoint" other_ns)" "$pool" >>"$MANIFEST"
  $B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 REGISTER id="register-$bytes-$pool" \
    function=touch key="$KEY" generation="$generation" bytes="$actual_bytes" \
    page_checksum="$page_checksum" workload_checksum="$workload_checksum" \
    >>"$OUTDIR/register.log"
  CURRENT_GENERATION=$generation
}

mkdir -p "$OUTDIR"
printf 'function,key,generation,slot,bytes,pages,page_checksum,workload_checksum,total_ns,lock_wait_ns,lifecycle_ns,vma_preflight_ns,pte_walk_ns,gup_ns,page_copy_ns,pte_recheck_ns,metadata_ns,checksum_ns,publish_ns,other_ns,pool_size\n' >"$MANIFEST"

{
  echo "run_id=$RUN_ID"
  echo "date=$(date --iso-8601=seconds)"
  echo "main_commit=$(git -C "$WS" rev-parse HEAD)"
  echo "kernel_commit=$(git -C "$WS/kernel/linux-6.6.32" rev-parse HEAD)"
  echo "cxlporter_commit=$(git -C "$PORTER" rev-parse HEAD)"
  echo "openwhisk_build=$(cd "$PORTER/openwhisk" && ./wsk -i property get 2>/dev/null | awk '/whisk API build number/{print $NF}')"
  echo "docker=$(docker version --format '{{.Server.Version}}')"
  echo "host_kernel=$(uname -r)"
  echo "footprints=$FOOTPRINTS"
  echo "policies=$POLICIES"
  echo "pool_sizes=$POOL_SIZES"
  echo "micro_samples=$MICRO_SAMPLES"
  echo "scaling_samples=$SCALING_SAMPLES"
  echo "concurrency_samples=$CONCURRENCY_SAMPLES"
} >"$OUTDIR/environment.txt"

POOL=4 "$PORTER/scripts/31-start-control-plane.sh" >"$OUTDIR/start.log" 2>&1
"$PORTER/openwhisk/scripts/42-deploy-cxlporter-action.sh" >"$OUTDIR/action-deploy.log"
release_if_present

first=1
for bytes in $FOOTPRINTS; do
  checkpoint_and_register "$bytes" 4

  if [[ $first == 1 ]]; then
    run_bench direct "$bytes" mow invoke "$MICRO_SAMPLES" 1 4 direct-invoke-micro
    run_bench openwhisk "$bytes" mow invoke "$MICRO_SAMPLES" 1 4 openwhisk-invoke-micro
    run_bench direct "$bytes" readonly restore "$MICRO_SAMPLES" 1 4 direct-restore-micro
    run_bench openwhisk "$bytes" readonly restore "$MICRO_SAMPLES" 1 4 openwhisk-restore-micro

    for pool in $POOL_SIZES; do
      POOL=$pool "$PORTER/scripts/31-start-control-plane.sh" >"$OUTDIR/start-pool-$pool.log" 2>&1
      $B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 REGISTER id="register-concurrency-$pool" \
        function=touch key="$KEY" generation="$CURRENT_GENERATION" bytes="$bytes" \
        page_checksum="$(tail -1 "$MANIFEST" | cut -d, -f7)" \
        workload_checksum="$(tail -1 "$MANIFEST" | cut -d, -f8)" >/dev/null
      run_bench direct "$bytes" mow invoke "$CONCURRENCY_SAMPLES" "$pool" "$pool" "direct-concurrency-$pool"
    done
    POOL=4 "$PORTER/scripts/31-start-control-plane.sh" >"$OUTDIR/start-pool-4-final.log" 2>&1
    $B send "$CONTROL_HOST" "$CONTROL_PORT" CXLP/1 REGISTER id=register-after-concurrency \
      function=touch key="$KEY" generation="$CURRENT_GENERATION" bytes="$bytes" \
      page_checksum="$(tail -1 "$MANIFEST" | cut -d, -f7)" \
      workload_checksum="$(tail -1 "$MANIFEST" | cut -d, -f8)" >/dev/null
    first=0
  fi

  for policy in $POLICIES; do
    run_bench direct "$bytes" "$policy" invoke "$SCALING_SAMPLES" 1 4 "direct-$bytes-$policy"
    if [[ $OPENWHISK_SCALING == 1 ]]; then
      run_bench openwhisk "$bytes" "$policy" invoke "$SCALING_SAMPLES" 1 4 "openwhisk-$bytes-$policy"
    fi
  done
  run_bench direct "$bytes" mow cold "$SCALING_SAMPLES" 1 4 "direct-$bytes-cold"
  if [[ $OPENWHISK_SCALING == 1 ]]; then
    run_bench openwhisk "$bytes" mow cold "$SCALING_SAMPLES" 1 4 "openwhisk-$bytes-cold"
  fi

  vm_ssh "$VM0_PORT" "/tmp/cxlporter-control/cxlporter-control release '$KEY' '$CURRENT_GENERATION'" \
    >>"$OUTDIR/release.log"
done

python3 "$PORTER/openwhisk/scripts/summarize_control.py" \
  "$COMBINED" "$OUTDIR/control-summary.csv"

for port in "$VM0_PORT" "$VM1_PORT"; do
  vm_ssh "$port" "sudo dmesg" >"$OUTDIR/dmesg-$port.log"
  if grep -E 'BUG:|WARNING:|Oops:|use-after-free|Bad pagetable|general protection fault' "$OUTDIR/dmesg-$port.log"; then
    echo "kernel error found in dmesg-$port.log" >&2
    exit 1
  fi
done

sha256sum "$OUTDIR"/* >"$OUTDIR/SHA256SUMS"
echo "CONTROL_BENCHMARK_OK outdir=$OUTDIR samples=$(($(wc -l <"$COMBINED") - 1))"
