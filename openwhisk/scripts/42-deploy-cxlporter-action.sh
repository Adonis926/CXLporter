#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
WSK=${WSK:-$ROOT/wsk}
ACTION=${ACTION:-cxlporter_proxy}
RUNTIME=${RUNTIME:-nodejs:20}
CONTROL_HOST=${CONTROL_HOST:-172.17.0.1}
CONTROL_PORT=${CONTROL_PORT:-2301}

"$ROOT/scripts/start-standalone.sh" >/dev/null
"$WSK" -i action update "$ACTION" "$ROOT/actions/cxlporter_proxy.js" \
  --kind "$RUNTIME" --memory 256 --timeout 300000 \
  --param host "$CONTROL_HOST" --param port "$CONTROL_PORT" >/dev/null

out=$("$WSK" -i action invoke "$ACTION" --blocking --result \
  -p operation PING -p request_id deploy-smoke)
status=$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("status", ""))' <<<"$out")
verify=$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("verify", ""))' <<<"$out")
if [[ "$status" != "0" || "$verify" != "1" ]]; then
  echo "OpenWhisk CXLporter proxy smoke failed: $out" >&2
  exit 1
fi
echo "cxlporter_action_ready action=$ACTION endpoint=$CONTROL_HOST:$CONTROL_PORT verify=ok"
