#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
RUN_ROOT=${RUN_ROOT:-$ROOT/paper-source}
PIDFILE=${PIDFILE:-$RUN_ROOT/openwhisk-standalone.pid}
CLEAN_CONTAINERS=${CLEAN_CONTAINERS:-1}

if [[ -f "$PIDFILE" ]]; then
  pid=$(sed -n '1p' "$PIDFILE")
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" || true
    for _ in $(seq 1 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 1
    done
  fi
  rm -f "$PIDFILE"
fi

if [[ "$CLEAN_CONTAINERS" == 1 ]]; then
  other=$(pgrep -f '[o]penwhisk-standalone.*\.jar' || true)
  if [[ -n "$other" ]]; then
    echo "refusing to remove wsk0 containers while another standalone is running (pid=$other)" >&2
    exit 1
  fi
  docker ps -a --format '{{.Names}}' |
    { grep -E '^wsk0_' || true; } |
    xargs -r docker rm -f >/dev/null
fi

echo "paper_openwhisk_stopped clean_containers=$CLEAN_CONTAINERS"
