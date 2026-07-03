#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork-ws/cxlporter/openwhisk}
JAR=${JAR:-$ROOT/openwhisk-standalone.jar}
WSK=${WSK:-$ROOT/wsk}
APIPORT=${APIPORT:-3233}
PIDFILE=${PIDFILE:-$ROOT/openwhisk-standalone.pid}
LOG=${LOG:-$ROOT/openwhisk-standalone.log}
PROXY=${PROXY:-http://qmyyxtl:Xiong0608@202.120.32.222:9081}

mkdir -p "$ROOT/results"

if [[ ! -f "$JAR" ]]; then
  if docker image inspect openwhisk/standalone:nightly >/dev/null 2>&1; then
    cid=$(docker create openwhisk/standalone:nightly)
    docker cp "$cid:/openwhisk-standalone.jar" "$JAR"
    docker rm -f "$cid" >/dev/null
  else
    echo "missing $JAR and openwhisk/standalone:nightly image" >&2
    exit 1
  fi
fi

if [[ ! -x "$WSK" ]]; then
  echo "[setup] downloading wsk CLI"
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  export http_proxy="$PROXY" https_proxy="$PROXY" HTTP_PROXY="$PROXY" HTTPS_PROXY="$PROXY"
  curl -fsSL -o "$tmp/wsk.tgz" "https://github.com/apache/openwhisk-cli/releases/download/latest/OpenWhisk_CLI-latest-linux-amd64.tgz"
  tar -xzf "$tmp/wsk.tgz" -C "$tmp"
  install -m 0755 "$tmp/wsk" "$WSK"
fi

if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "[start] OpenWhisk already running pid=$(cat "$PIDFILE")"
else
  rm -f "$LOG"
  export http_proxy="$PROXY" https_proxy="$PROXY" HTTP_PROXY="$PROXY" HTTPS_PROXY="$PROXY"
  export PATH="$ROOT:$PATH"
  nohup java -Dwhisk.standalone.host.name=127.0.0.1 \
    -jar "$JAR" --no-browser --no-ui --port "$APIPORT" \
    >"$LOG" 2>&1 &
  echo $! >"$PIDFILE"
  echo "[start] pid=$(cat "$PIDFILE") log=$LOG"
fi

for i in $(seq 1 180); do
  if grep -q "wsk property set" "$LOG" 2>/dev/null && curl -fsS "http://127.0.0.1:$APIPORT/api/v1" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "OpenWhisk exited early" >&2
    tail -160 "$LOG" >&2 || true
    exit 1
  fi
  sleep 2
  if [[ "$i" == 180 ]]; then
    echo "OpenWhisk did not become ready" >&2
    tail -160 "$LOG" >&2 || true
    exit 1
  fi
done

auth=$(grep -o -- "--auth '[^']*'" "$LOG" | tail -1 | sed "s/--auth '//;s/'//")
if [[ -z "$auth" ]]; then
  echo "could not parse auth from $LOG" >&2
  tail -80 "$LOG" >&2 || true
  exit 1
fi
"$WSK" property set --apihost "http://127.0.0.1:$APIPORT" >/dev/null
"$WSK" property set --auth "$auth" >/dev/null
"$WSK" -i action list >/dev/null

echo "openwhisk_ready apihost=http://127.0.0.1:$APIPORT wsk=$WSK pid=$(cat "$PIDFILE")"
