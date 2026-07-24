#!/usr/bin/env bash
set -euo pipefail

WS=${WS:-/home/xtl/cxlfork/cxlfork-repro}
ROOT=${ROOT:-$WS/cxlporter/openwhisk}
WSK=${WSK:-$ROOT/wsk}
APIPORT=${APIPORT:-3433}
APIHOST=${APIHOST:-http://127.0.0.1:$APIPORT}
AUTH=${AUTH:-23bc46b1-71f6-4ed5-8c54-816aa4f8c502:123zO3xZCLrMN6v2BKK1dXYFpXlPkccOFqm12CdAsMgRU4VrNZ9lyGVCGuMDGIwP}
KEY=${KEY:-2472401}
PREPARE=${PREPARE:-0}
ACTION=${ACTION:-paper_cxlfork_source}
MISSING_ACTION=${MISSING_ACTION:-paper_cxlfork_missing}
NORMAL_ACTION=${NORMAL_ACTION:-paper_normal_source}
RESULT_DIR=${RESULT_DIR:-$WS/results/paper-system/$(date +%Y%m%d-%H%M%S)-openwhisk-source}
LOG=$RESULT_DIR/acceptance.log

mkdir -p "$RESULT_DIR"
exec > >(tee "$LOG") 2>&1

echo "event=begin result_dir=$RESULT_DIR key=$KEY"
"$ROOT/scripts/52-stop-paper-source.sh" || true
"$ROOT/scripts/51-start-paper-source.sh"
if [[ "$PREPARE" == 1 ]]; then
  "$WS/cxlporter/scripts/paper-system/31-prepare-paper-capsule.sh" "$KEY"
else
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -p 2400 ubuntu@127.0.0.1 \
    "sudo /tmp/cxlfork_capsule_k0 catalog-lookup '$KEY'" || {
      echo "catalog template key=$KEY is not ready; prepare it first or rerun with PREPARE=1 on a free identity" >&2
      exit 1
    }
fi

START=0 ACTION="$ACTION" OWNER=paper FUNCTION=capsule-k0 VERSION=v1 \
  "$ROOT/scripts/53-deploy-paper-source-action.sh"

invoke() {
  local action=$1
  local output=$2
  # wsk writes an application-error result to stderr while returning non-zero.
  # Merge both streams so the raw JSON is retained for negative-path analysis.
  "$WSK" -i --apihost "$APIHOST" --auth "$AUTH" action invoke "$action" --blocking --result >"$output" 2>&1
  cat "$output"
}

echo "event=source-success-1"
invoke "$ACTION" "$RESULT_DIR/source-success-1.json"
echo "event=source-success-2"
invoke "$ACTION" "$RESULT_DIR/source-success-2.json"

python3 - "$RESULT_DIR/source-success-1.json" "$RESULT_DIR/source-success-2.json" <<'PY'
import json
import sys

for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as stream:
        result = json.load(stream)
    assert result["openwhisk_source_integrated"] is True
    assert result["status"] == "0"
    assert result["verify"] == "1"
    assert int(result["kernel_restore_ns"]) > 0
    proof = result["runtime_proof"]
    for marker in ("shared-leaf-proof", "context-after-fd-close", "target-continued"):
        assert marker in proof
print("source_success_verify=ok")
PY

echo "event=missing-catalog"
START=0 ACTION="$MISSING_ACTION" OWNER=paper FUNCTION=missing-capsule VERSION=v1 \
  "$ROOT/scripts/53-deploy-paper-source-action.sh"
invoke "$MISSING_ACTION" "$RESULT_DIR/missing-catalog.json" || true
python3 - "$RESULT_DIR/missing-catalog.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)["error"]
assert result["openwhisk_source_integrated"] is True
assert result["status"] == "-2"
assert result["verify"] == "0"
print("missing_catalog_verify=ok")
PY

echo "event=normal-action-control"
"$WSK" -i --apihost "$APIHOST" --auth "$AUTH" action update "$NORMAL_ACTION" "$ROOT/actions/faas_workload.js" \
  --kind nodejs:20 --memory 256 --timeout 300000 >/dev/null
"$WSK" -i --apihost "$APIHOST" --auth "$AUTH" action invoke "$NORMAL_ACTION" --blocking --result \
  -p function touch -p bytes 4096 >"$RESULT_DIR/normal-action.json"
cat "$RESULT_DIR/normal-action.json"
python3 - "$RESULT_DIR/normal-action.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
assert result["function"] == "touch"
assert result["bytes"] == 4096
assert "openwhisk_source_integrated" not in result
print("normal_action_verify=ok")
PY

cp "$ROOT/paper-source/build-metadata.txt" "$RESULT_DIR/" 2>/dev/null || true
grep "creating source-integrated CXLP/2 container" "$ROOT/paper-source/openwhisk-standalone.log" \
  >"$RESULT_DIR/source-factory-markers.log"
if grep -E "sending arguments to /guest/paper_cxlfork_(source|missing)" \
  "$ROOT/paper-source/openwhisk-standalone.log" >"$RESULT_DIR/unexpected-docker-path.log"; then
  echo "annotated action entered DockerContainer.run" >&2
  exit 1
fi
docker ps -a --format '{{.Names}} {{.Image}} {{.Status}}' >"$RESULT_DIR/docker-containers.txt"

for port in 2400 2401; do
  ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -p "$port" ubuntu@127.0.0.1 sudo dmesg >"$RESULT_DIR/dmesg-$port.log"
  if grep -Ei 'BUG:|WARNING:|Oops:|kernel panic|cxlfork.*(error|fail)' \
    "$RESULT_DIR/dmesg-$port.log" >"$RESULT_DIR/dmesg-$port-errors.log"; then
    echo "kernel log check failed for guest port $port" >&2
    exit 1
  fi
done

echo "event=complete verify=ok result_dir=$RESULT_DIR"
