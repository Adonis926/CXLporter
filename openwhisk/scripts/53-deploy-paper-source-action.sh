#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
WSK=${WSK:-$ROOT/wsk}
APIPORT=${APIPORT:-3433}
APIHOST=${APIHOST:-http://127.0.0.1:$APIPORT}
AUTH=${AUTH:-23bc46b1-71f6-4ed5-8c54-816aa4f8c502:123zO3xZCLrMN6v2BKK1dXYFpXlPkccOFqm12CdAsMgRU4VrNZ9lyGVCGuMDGIwP}
ACTION=${ACTION:-paper_cxlfork_source}
ACTION_FILE=${ACTION_FILE:-$ROOT/actions/faas_workload.js}
OWNER=${OWNER:-paper}
FUNCTION=${FUNCTION:-capsule-k0}
VERSION=${VERSION:-v1}
RUNTIME=${RUNTIME:-nodejs:20}
ACTION_MEMORY=${ACTION_MEMORY:-128}
ACTION_LOGSIZE=${ACTION_LOGSIZE:-0}
ACTION_CONCURRENCY=${ACTION_CONCURRENCY:-1}
START=${START:-1}

[[ -x "$WSK" ]] || {
  echo "missing wsk CLI: $WSK" >&2
  exit 1
}
[[ -f "$ACTION_FILE" ]] || {
  echo "missing placeholder action source: $ACTION_FILE" >&2
  exit 1
}
if [[ "$START" == 1 ]]; then
  "$ROOT/scripts/51-start-paper-source.sh"
fi

"$WSK" -i --apihost "$APIHOST" --auth "$AUTH" action update "$ACTION" "$ACTION_FILE" \
  --kind "$RUNTIME" --memory "$ACTION_MEMORY" --timeout 300000 \
  --logsize "$ACTION_LOGSIZE" --concurrency "$ACTION_CONCURRENCY" \
  --annotation cxlfork true \
  --annotation cxlfork-owner "$OWNER" \
  --annotation cxlfork-function "$FUNCTION" \
  --annotation cxlfork-version "$VERSION" >/dev/null

"$WSK" -i --apihost "$APIHOST" --auth "$AUTH" action get "$ACTION" --summary
echo "paper_source_action_ready action=$ACTION identity=$OWNER/$FUNCTION/$VERSION" \
  "memory_mb=$ACTION_MEMORY logsize_mb=$ACTION_LOGSIZE" \
  "concurrency=$ACTION_CONCURRENCY api=$APIHOST"
