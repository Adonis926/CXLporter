#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/xtl/cxlfork/cxlfork-repro/cxlporter/openwhisk}
SOURCE=${SOURCE:-/home/xtl/cxlfork/openwhisk-7ef091c-paper}
SOURCE_URL=${SOURCE_URL:-https://github.com/apache/openwhisk.git}
PATCH=${PATCH:-$ROOT/patches/0001-invoker-add-source-integrated-CXLP2-container-path.patch}
PINNED_FILE=${PINNED_FILE:-$ROOT/paper-source/PINNED_COMMIT}
JAVA_HOME=${JAVA_HOME:-/home/xtl/cxlfork/toolchains/jdk-11}
NODE_BIN=${NODE_BIN:-/home/xtl/.nvm/versions/node/v24.18.0/bin}
GRADLE_USER_HOME=${GRADLE_USER_HOME:-/home/xtl/cxlfork/gradle-home-jdk11}
OUTPUT=${OUTPUT:-$ROOT/paper-source/openwhisk-standalone-cxlfork.jar}
METADATA=${METADATA:-$ROOT/paper-source/build-metadata.txt}

PINNED=$(tr -d '[:space:]' <"$PINNED_FILE")
[[ "$PINNED" =~ ^[0-9a-f]{40}$ ]] || {
  echo "invalid pinned OpenWhisk commit in $PINNED_FILE" >&2
  exit 1
}
[[ -f "$PATCH" ]] || {
  echo "missing OpenWhisk patch: $PATCH" >&2
  exit 1
}
[[ -x "$JAVA_HOME/bin/java" ]] || {
  echo "missing JDK 11: $JAVA_HOME/bin/java" >&2
  exit 1
}
[[ -x "$NODE_BIN/node" && -x "$NODE_BIN/npm" ]] || {
  echo "missing isolated Node/npm directory: $NODE_BIN" >&2
  exit 1
}

if [[ ! -d "$SOURCE/.git" ]]; then
  git clone "$SOURCE_URL" "$SOURCE"
  git -C "$SOURCE" checkout --detach "$PINNED"
fi

git -C "$SOURCE" cat-file -e "$PINNED^{commit}"
if git -C "$SOURCE" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  patch_state=already-applied
elif git -C "$SOURCE" apply --check "$PATCH" >/dev/null 2>&1; then
  if ! git -C "$SOURCE" diff --quiet || ! git -C "$SOURCE" diff --cached --quiet; then
    echo "refusing to apply the paper patch over unrelated OpenWhisk changes" >&2
    exit 1
  fi
  git -C "$SOURCE" checkout --detach "$PINNED"
  git -C "$SOURCE" apply "$PATCH"
  patch_state=applied-working-tree
else
  echo "OpenWhisk source is neither the pinned base nor the expected patched tree" >&2
  exit 1
fi

export JAVA_HOME GRADLE_USER_HOME
export PATH="$NODE_BIN:$JAVA_HOME/bin:$PATH"

(
  cd "$SOURCE"
  ./gradlew --no-daemon \
    :core:invoker:checkScalafmt \
    :tests:checkTestScalafmt
  ./gradlew --no-daemon \
    :tests:test \
    -x :tests:buildArtifacts \
    --tests org.apache.openwhisk.core.containerpool.cxlfork.CxlporterContainerTests
  ./gradlew --no-daemon :core:standalone:bootJar
)

artifact=$SOURCE/core/standalone/build/libs/openwhisk-standalone-1.0.1-SNAPSHOT.jar
[[ -f "$artifact" ]] || {
  echo "missing bootJar output: $artifact" >&2
  exit 1
}
install -m 0644 "$artifact" "$OUTPUT"

{
  echo "built_at=$(date --iso-8601=seconds)"
  echo "pinned_commit=$PINNED"
  echo "source_head=$(git -C "$SOURCE" rev-parse HEAD)"
  echo "patch_state=$patch_state"
  echo "patch_sha256=$(sha256sum "$PATCH" | awk '{print $1}')"
  echo "jar_sha256=$(sha256sum "$OUTPUT" | awk '{print $1}')"
  echo "java=$("$JAVA_HOME/bin/java" -version 2>&1 | sed -n '1p')"
  echo "node=$("$NODE_BIN/node" --version)"
  echo "npm=$("$NODE_BIN/npm" --version)"
} >"$METADATA"

cat "$METADATA"
echo "paper_openwhisk_build=ok jar=$OUTPUT"
