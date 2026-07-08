#!/usr/bin/env bash
set -euo pipefail
ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
PIDFILE=${PIDFILE:-$ROOT/openwhisk-standalone.pid}
if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  kill "$(cat "$PIDFILE")" || true
  for _ in $(seq 1 20); do
    kill -0 "$(cat "$PIDFILE")" 2>/dev/null || break
    sleep 1
  done
fi
rm -f "$PIDFILE"
docker ps -a --format '{{.Names}}' | grep -E '^wsk' | xargs -r docker rm -f >/dev/null 2>&1 || true
echo "openwhisk_stopped"
