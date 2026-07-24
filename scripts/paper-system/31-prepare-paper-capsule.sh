#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
VM0_PORT=${VM0_PORT:-2400}
SSH_USER=${SSH_USER:-ubuntu}
SSH_HOST=${SSH_HOST:-127.0.0.1}
KEY=${1:?usage: $0 KEY}
PORTER=$WS/cxlporter
RUNTIME=$PORTER/cxlfork-capsule-runtime

make -C "$PORTER" cxlfork-capsule-runtime
scp -q -o BatchMode=yes -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -P "$VM0_PORT" \
	"$RUNTIME" \
	"$SSH_USER@$SSH_HOST:/tmp/cxlfork_capsule_k0"
ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -p "$VM0_PORT" \
	"$SSH_USER@$SSH_HOST" \
	"chmod 0755 /tmp/cxlfork_capsule_k0 &&
	 sudo /tmp/cxlfork_capsule_k0 prepare '$KEY' &&
	 sudo /tmp/cxlfork_capsule_k0 catalog-register '$KEY'"
