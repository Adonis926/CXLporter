#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
PORTER=${PORTER:-$WS/cxlporter}
VM0_PORT=${VM0_PORT:-2200}
VM1_PORT=${VM1_PORT:-2201}
LOCAL_PORT=${LOCAL_PORT:-2202}
DIR=${DIR:-/tmp/cxlporter}
FUNCTIONS=${FUNCTIONS:-"json:4194304 pyaes:4194304 html:8388608 touch:67108864"}
POLICIES=${POLICIES:-"mow moa hybrid"}
POOL_SIZES=${POOL_SIZES:-"1 4"}
REQUESTS=${REQUESTS:-5}
WARMUP=${WARMUP:-1}
KEEPALIVE_US=${KEEPALIVE_US:-0}
HYBRID_HOT_STRIDE=${HYBRID_HOT_STRIDE:-4}
KEY_BASE=${KEY_BASE:-91000}
OUT=${OUT:-$PORTER/results/cxlporter-$(date +%Y%m%d-%H%M%S).csv}
MANIFEST=${MANIFEST:-$PORTER/results/checkpoint-store-$(date +%Y%m%d-%H%M%S).csv}
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

vm_ssh() {
	local port=$1
	shift
	ssh "${SSH_OPTS[@]}" -p "$port" ubuntu@localhost "$@"
}

vm_scp() {
	local port=$1
	shift
	scp -P "$port" "${SSH_OPTS[@]}" "$@"
}

field() {
	local text=$1
	local name=$2

	printf '%s\n' "$text" | awk -v name="$name" '
		{
			for (i = 1; i <= NF; i++) {
				if (index($i, name "=") == 1) {
					sub("^[^=]*=", "", $i)
					value = $i
				}
			}
		}
		END { if (value != "") print value }
	'
}

build() {
	make -C "$PORTER" clean all
}

deploy() {
	local port

	for port in "$VM0_PORT" "$VM1_PORT" "$LOCAL_PORT"; do
		vm_ssh "$port" "mkdir -p '$DIR'"
		vm_scp "$port" "$PORTER/cxlporter" ubuntu@localhost:"$DIR/" >/dev/null
	done
	for port in "$VM0_PORT" "$VM1_PORT"; do
		vm_ssh "$port" "test -e /dev/cxlfork && sudo chmod 666 /dev/cxlfork"
	done
}

csv_header() {
	printf 'system,function,key,bytes,policy,pool,requests,warmup,keepalive_us,prepare_ns,restore_ns,dispatch_to_done_ns,fork_to_child_ns,workload_ns,child_total_ns,pages,faults,pte_image_build_ns,pt_page_image_build_ns,vma_tree_build_ns,state_image_build_ns,private_rebase_ns,global_detach_ns,notes\n'
}

manifest_header() {
	printf 'function,key,bytes,prepare_ns,pages,anon_pages,clean_file_pages,dirty_file_pages,pte_image_build_ns,pt_page_image_build_ns,vma_tree_build_ns,state_image_build_ns,private_rebase_ns,global_detach_ns\n'
}

validate_csv_columns() {
	local file=$1
	local expected=$2

	awk -F, -v expected="$expected" '
		NF != expected {
			printf "%s:%d: expected %d columns, got %d\n", FILENAME, FNR, expected, NF > "/dev/stderr"
			bad = 1
		}
		END { exit bad }
	' "$file"
}

run_checkpoint() {
	local fn=$1
	local bytes=$2
	local key=$3
	local out prepare_ns pages anon_pages clean_file_pages dirty_file_pages
	local pte_image_build_ns pt_page_image_build_ns vma_tree_build_ns state_image_build_ns private_rebase_ns global_detach_ns

	out=$(vm_ssh "$VM0_PORT" "cd '$DIR' && ./cxlporter checkpoint '$key' '$fn' '$bytes'")
	prepare_ns=$(field "$out" prepare_ns)
	pages=$(field "$out" pages)
	anon_pages=$(field "$out" anon_pages)
	clean_file_pages=$(field "$out" clean_file_pages)
	dirty_file_pages=$(field "$out" dirty_file_pages)
	pte_image_build_ns=$(field "$out" pte_image_build_ns)
	pt_page_image_build_ns=$(field "$out" pt_page_image_build_ns)
	vma_tree_build_ns=$(field "$out" vma_tree_build_ns)
	state_image_build_ns=$(field "$out" state_image_build_ns)
	private_rebase_ns=$(field "$out" private_rebase_ns)
	global_detach_ns=$(field "$out" global_detach_ns)
	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$fn" "$key" "$bytes" "$prepare_ns" "$pages" "$anon_pages" \
		"$clean_file_pages" "$dirty_file_pages" "$pte_image_build_ns" \
		"$pt_page_image_build_ns" "$vma_tree_build_ns" \
		"$state_image_build_ns" "$private_rebase_ns" "$global_detach_ns" >>"$MANIFEST"
	printf '[checkpoint] fn=%s key=%s bytes=%s prepare_ns=%s pages=%s\n' \
		"$fn" "$key" "$bytes" "$prepare_ns" "$pages"
}

run_local() {
	local fn=$1
	local bytes=$2
	local out fork_to_child_ns workload_ns child_total_ns pages

	out=$(vm_ssh "$LOCAL_PORT" "cd '$DIR' && ./cxlporter local '$fn' '$bytes' '$REQUESTS' '$WARMUP'")
	fork_to_child_ns=$(field "$out" fork_to_child_ns)
	workload_ns=$(field "$out" workload_ns)
	child_total_ns=$(field "$out" child_total_ns)
	pages=$(field "$out" pages)
	printf 'LocalFork,%s,NA,%s,local,0,%s,%s,0,0,0,0,%s,%s,%s,%s,0,0,0,0,0,0,0,ordinary fork baseline\n' \
		"$fn" "$bytes" "$REQUESTS" "$WARMUP" "$fork_to_child_ns" \
		"$workload_ns" "$child_total_ns" "$pages" >>"$OUT"
	printf '[local] fn=%s bytes=%s fork_to_child_ns=%s workload_ns=%s child_total_ns=%s\n' \
		"$fn" "$bytes" "$fork_to_child_ns" "$workload_ns" "$child_total_ns"
}

run_invoke() {
	local fn=$1
	local bytes=$2
	local key=$3
	local policy=$4
	local pool=$5
	local hot_stride=0
	local out restore_ns dispatch_to_done_ns workload_ns child_total_ns pages faults

	if [[ "$policy" == "hybrid" ]]; then
		hot_stride=$HYBRID_HOT_STRIDE
	fi
	out=$(vm_ssh "$VM1_PORT" "cd '$DIR' && ./cxlporter invoke '$key' '$fn' '$bytes' '$policy' '$pool' '$REQUESTS' '$WARMUP' '$KEEPALIVE_US' '$hot_stride'")
	restore_ns=$(field "$out" restore_ns)
	dispatch_to_done_ns=$(field "$out" dispatch_to_done_ns)
	workload_ns=$(field "$out" workload_ns)
	child_total_ns=$(field "$out" child_total_ns)
	pages=$(field "$out" pages)
	faults=$(field "$out" faults)
	printf 'CXLporter,%s,%s,%s,%s,%s,%s,%s,%s,NA,%s,%s,0,%s,%s,%s,%s,NA,NA,NA,NA,NA,NA,ghost pool invoke with CXLfork tiering policy\n' \
		"$fn" "$key" "$bytes" "$policy" "$pool" "$REQUESTS" "$WARMUP" \
		"$KEEPALIVE_US" "$restore_ns" "$dispatch_to_done_ns" \
		"$workload_ns" "$child_total_ns" "$pages" "$faults" >>"$OUT"
	printf '[invoke] fn=%s key=%s policy=%s pool=%s restore_ns=%s dispatch_to_done_ns=%s workload_ns=%s faults=%s\n' \
		"$fn" "$key" "$policy" "$pool" "$restore_ns" "$dispatch_to_done_ns" \
		"$workload_ns" "$faults"
}

main() {
	local entry fn bytes key policy pool

	mkdir -p "$(dirname "$OUT")"
	csv_header >"$OUT"
	manifest_header >"$MANIFEST"
	build
	deploy

	key=$KEY_BASE
	for entry in $FUNCTIONS; do
		fn=${entry%%:*}
		bytes=${entry#*:}
		run_checkpoint "$fn" "$bytes" "$key"
		run_local "$fn" "$bytes"
		for pool in $POOL_SIZES; do
			for policy in $POLICIES; do
				run_invoke "$fn" "$bytes" "$key" "$policy" "$pool"
			done
		done
		key=$((key + 1))
	done
	validate_csv_columns "$OUT" 24
	validate_csv_columns "$MANIFEST" 14
	echo "results=$OUT"
	echo "manifest=$MANIFEST"
}

main "$@"
