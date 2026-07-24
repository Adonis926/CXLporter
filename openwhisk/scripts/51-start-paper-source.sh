#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
RUN_ROOT=${RUN_ROOT:-$ROOT/paper-source}
JAR=${JAR:-$RUN_ROOT/openwhisk-standalone-cxlfork.jar}
JAVA=${JAVA:-/home/xtl/cxlfork/toolchains/jdk-11/bin/java}
APIPORT=${APIPORT:-3433}
PIDFILE=${PIDFILE:-$RUN_ROOT/openwhisk-standalone.pid}
LOG=${LOG:-$RUN_ROOT/openwhisk-standalone.log}
CXLPORTER_HOST=${CXLPORTER_HOST:-172.17.0.1}
CXLPORTER_PORT=${CXLPORTER_PORT:-2501}
INVOKES_PER_MINUTE=${INVOKES_PER_MINUTE:-100000}
INVOKES_CONCURRENT=${INVOKES_CONCURRENT:-128}

[[ -f "$JAR" ]] || {
  echo "missing $JAR; run 50-build-paper-source.sh first" >&2
  exit 1
}
[[ -x "$JAVA" ]] || {
  echo "missing Java runtime: $JAVA" >&2
  exit 1
}
mkdir -p "$RUN_ROOT/results"

if [[ -f "$PIDFILE" ]] && kill -0 "$(sed -n '1p' "$PIDFILE")" 2>/dev/null; then
  pid=$(sed -n '1p' "$PIDFILE")
  argv=$(tr '\0' ' ' <"/proc/$pid/cmdline")
  env=$(tr '\0' '\n' <"/proc/$pid/environ")
  [[ "$argv" == *"$JAR"* && "$argv" == *"--port $APIPORT"* ]] || {
    echo "pid=$pid does not match the requested Paper standalone" >&2
    exit 1
  }
  [[ "$argv" == *"-Dwhisk.config.limits-actions-invokes-perMinute=$INVOKES_PER_MINUTE"* ]] || {
    echo "pid=$pid does not use INVOKES_PER_MINUTE=$INVOKES_PER_MINUTE" >&2
    exit 1
  }
  [[ "$argv" == *"-Dwhisk.config.limits-actions-invokes-concurrent=$INVOKES_CONCURRENT"* ]] || {
    echo "pid=$pid does not use INVOKES_CONCURRENT=$INVOKES_CONCURRENT" >&2
    exit 1
  }
  grep -qx "CXLPORTER_ENABLED=true" <<<"$env"
  grep -qx "CXLPORTER_HOST=$CXLPORTER_HOST" <<<"$env"
  grep -qx "CXLPORTER_PORT=$CXLPORTER_PORT" <<<"$env"
else
  rm -f "$PIDFILE"
  other=$(pgrep -f '[o]penwhisk-standalone.*\.jar' || true)
  if [[ -n "$other" ]]; then
    echo "another OpenWhisk standalone is running (pid=$other); stop it before the Paper profile" >&2
    exit 1
  fi
  rm -f "$LOG"
  CXLPORTER_ENABLED=true \
  CXLPORTER_HOST="$CXLPORTER_HOST" \
  CXLPORTER_PORT="$CXLPORTER_PORT" \
  nohup "$JAVA" \
    -Dwhisk.standalone.host.name=127.0.0.1 \
    -Dwhisk.config.limits-actions-invokes-perMinute="$INVOKES_PER_MINUTE" \
    -Dwhisk.config.limits-actions-invokes-concurrent="$INVOKES_CONCURRENT" \
    -jar "$JAR" --no-browser --no-ui --port "$APIPORT" \
    >"$LOG" 2>&1 &
  echo $! >"$PIDFILE"
fi

for i in $(seq 1 180); do
  if curl -fsS "http://127.0.0.1:$APIPORT/api/v1" >/dev/null 2>&1; then
    echo "paper_openwhisk_ready apihost=http://127.0.0.1:$APIPORT pid=$(sed -n '1p' "$PIDFILE") cxlporter=$CXLPORTER_HOST:$CXLPORTER_PORT"
    exit 0
  fi
  if ! kill -0 "$(sed -n '1p' "$PIDFILE")" 2>/dev/null; then
    echo "Paper OpenWhisk exited before readiness" >&2
    tail -160 "$LOG" >&2 || true
    exit 1
  fi
  sleep 2
done

echo "Paper OpenWhisk did not become ready" >&2
tail -160 "$LOG" >&2 || true
exit 1
