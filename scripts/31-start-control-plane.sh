#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
PORTER=${PORTER:-$WS/cxlporter}
VM0_PORT=${VM0_PORT:-2200}
VM1_PORT=${VM1_PORT:-2201}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2301}
GUEST_CONTROL_PORT=${GUEST_CONTROL_PORT:-7000}
POOL=${POOL:-4}
DIR=${DIR:-/tmp/cxlporter-control}
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

vm_ssh() {
  local port=$1
  shift
  ssh "${SSH_OPTS[@]}" -p "$port" ubuntu@localhost "$@"
}

vm_scp() {
  local port=$1
  shift
  scp -q -P "$port" "${SSH_OPTS[@]}" "$@"
}

make -C "$PORTER" clean all
for port in "$VM0_PORT" "$VM1_PORT"; do
  vm_ssh "$port" "mkdir -p '$DIR' && sudo chmod 666 /dev/cxlfork"
  vm_scp "$port" "$PORTER/cxlporter-control" ubuntu@localhost:"$DIR/"
done

vm_ssh "$VM1_PORT" "if [[ -f '$DIR/daemon.pid' ]]; then kill \$(cat '$DIR/daemon.pid') 2>/dev/null || true; fi"
vm_ssh "$VM1_PORT" "nohup '$DIR/cxlporter-control' serve '$GUEST_CONTROL_PORT' '$POOL' >'$DIR/daemon.log' 2>&1 </dev/null & echo \$! >'$DIR/daemon.pid'"

for _ in $(seq 1 50); do
  if "$PORTER/cxlporter-control" send "$CONTROL_HOST" "$CONTROL_PORT" \
      CXLP/1 PING id=host-control-smoke >/dev/null 2>&1; then
    vm_ssh "$VM1_PORT" "cat '$DIR/daemon.log'"
    echo "control_plane_ready endpoint=$CONTROL_HOST:$CONTROL_PORT pool=$POOL verify=ok"
    exit 0
  fi
  sleep 0.1
done

vm_ssh "$VM1_PORT" "cat '$DIR/daemon.log'" >&2 || true
echo "CXLporter daemon did not become reachable through QEMU hostfwd" >&2
exit 1
