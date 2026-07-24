#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
VM0_PORT=${VM0_PORT:-2400}
SSH_USER=${SSH_USER:-ubuntu}
SSH_HOST=${SSH_HOST:-127.0.0.1}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2501}
KEY=${1:-56001}
PORTER=$WS/cxlporter/cxlporter-paper
PREPARE=$WS/cxlporter/scripts/paper-system/31-prepare-paper-capsule.sh

cleanup() {
	ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -p "$VM0_PORT" \
		"$SSH_USER@$SSH_HOST" \
		"sudo /tmp/cxlfork_capsule_k0 catalog-evict '$KEY' || true
		 sudo /tmp/cxlfork_capsule_k0 release '$KEY' || true" \
		>/dev/null 2>&1 || true
}
trap cleanup EXIT

"$PREPARE" "$KEY"
"$PORTER" client "$CONTROL_HOST" "$CONTROL_PORT" PING 100
"$PORTER" client "$CONTROL_HOST" "$CONTROL_PORT" POOL_STATUS 101

for request_id in 102 103; do
	line=$("$PORTER" client "$CONTROL_HOST" "$CONTROL_PORT" \
		ACTIVATE "$request_id" paper capsule-k0 v1)
	printf '%s\n' "$line"
	grep -q '^CXLP/2 OK ' <<<"$line"
	grep -q 'status=0 ' <<<"$line"
	grep -q 'child_status=0 ' <<<"$line"
	grep -q 'verify=1 ' <<<"$line"
	grep -Eq 'kernel_restore_ns=[1-9][0-9]* ' <<<"$line"
done

echo "paper_cxlp2_smoke key=$KEY activations=2 verify=ok"
