#!/usr/bin/env bash
set -euo pipefail

# 启动 OpenWhisk standalone baseline 环境。
# PROXY 默认为空；如需代理应由调用者显式传入，不在脚本里擅自切换。
ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
JAR=${JAR:-$ROOT/openwhisk-standalone.jar}
WSK=${WSK:-$ROOT/wsk}
APIPORT=${APIPORT:-3233}
PIDFILE=${PIDFILE:-$ROOT/openwhisk-standalone.pid}
LOG=${LOG:-$ROOT/openwhisk-standalone.log}
PROXY=${PROXY:-}
INVOKES_PER_MINUTE=${INVOKES_PER_MINUTE:-100000}
INVOKES_CONCURRENT=${INVOKES_CONCURRENT:-30}
# standalone.conf 中 guest namespace 的固定开发凭据；可由环境变量覆盖。
STANDALONE_AUTH=${STANDALONE_AUTH:-23bc46b1-71f6-4ed5-8c54-816aa4f8c502:123zO3xZCLrMN6v2BKK1dXYFpXlPkccOFqm12CdAsMgRU4VrNZ9lyGVCGuMDGIwP}

mkdir -p "$ROOT/results"

if [[ ! -f "$JAR" ]]; then
  # 优先从已有 Docker image 中取 standalone jar，避免每次网络下载。
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
  # wsk CLI 不随仓库提交；首次运行时下载到 openwhisk/ 目录。
  echo "[setup] downloading wsk CLI"
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  export http_proxy="$PROXY" https_proxy="$PROXY" HTTP_PROXY="$PROXY" HTTPS_PROXY="$PROXY"
  curl -fsSL -o "$tmp/wsk.tgz" "https://github.com/apache/openwhisk-cli/releases/download/latest/OpenWhisk_CLI-latest-linux-amd64.tgz"
  tar -xzf "$tmp/wsk.tgz" -C "$tmp"
  install -m 0755 "$tmp/wsk" "$WSK"
fi

if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  # 只复用使用相同限流参数的进程，避免 benchmark 静默落回 standalone 的 60/min 默认值。
  pid=$(cat "$PIDFILE")
  argv=$(tr '\0' ' ' <"/proc/$pid/cmdline")
  minute_arg="-Dwhisk.config.limits-actions-invokes-perMinute=$INVOKES_PER_MINUTE"
  concurrent_arg="-Dwhisk.config.limits-actions-invokes-concurrent=$INVOKES_CONCURRENT"
  if [[ "$argv" != *"$minute_arg"* || "$argv" != *"$concurrent_arg"* ]]; then
    echo "OpenWhisk pid=$pid uses different throttle settings; run stop-standalone.sh before restart" >&2
    echo "wanted: $minute_arg $concurrent_arg" >&2
    exit 1
  fi
  echo "[start] OpenWhisk already running pid=$pid invokes_per_minute=$INVOKES_PER_MINUTE invokes_concurrent=$INVOKES_CONCURRENT"
else
  rm -f "$LOG"
  export http_proxy="$PROXY" https_proxy="$PROXY" HTTP_PROXY="$PROXY" HTTPS_PROXY="$PROXY"
  export PATH="$ROOT:$PATH"
  nohup java -Dwhisk.standalone.host.name=127.0.0.1 \
    -Dwhisk.config.limits-actions-invokes-perMinute="$INVOKES_PER_MINUTE" \
    -Dwhisk.config.limits-actions-invokes-concurrent="$INVOKES_CONCURRENT" \
    -jar "$JAR" --no-browser --no-ui --port "$APIPORT" \
    >"$LOG" 2>&1 &
  echo $! >"$PIDFILE"
  echo "[start] pid=$(cat "$PIDFILE") log=$LOG"
fi

for i in $(seq 1 180); do
  # 已配置过 wsk 时 standalone 不一定再次输出 property 命令，因此只依赖 API readiness。
  if curl -fsS "http://127.0.0.1:$APIPORT/api/v1" >/dev/null 2>&1; then
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

"$WSK" property set --apihost "http://127.0.0.1:$APIPORT" >/dev/null
"$WSK" property set --auth "$STANDALONE_AUTH" >/dev/null
"$WSK" -i action list >/dev/null

echo "openwhisk_ready apihost=http://127.0.0.1:$APIPORT wsk=$WSK pid=$(cat "$PIDFILE") invokes_per_minute=$INVOKES_PER_MINUTE invokes_concurrent=$INVOKES_CONCURRENT"
