#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PUBLISHER=$(realpath "$SCRIPT_DIR/../publish-tag-draft-assets.sh")
MOCK_CURL=$(realpath "$SCRIPT_DIR/mock-github-release-curl.py")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ringout-release-publisher-test.XXXXXXXX")
trap 'rm -rf -- "$WORK"' EXIT

die() {
  echo "publish-tag-draft-assets test: $*" >&2
  exit 1
}

REPO="$WORK/repo"
MOCK_BIN="$WORK/mock-bin"
TAG=v9.9.9-ell.1
REPOSITORY=Ell/RingOut
mkdir -p "$MOCK_BIN"
ln -s "$MOCK_CURL" "$MOCK_BIN/curl"
TRUE_BIN=$(type -P true)
[[ "$TRUE_BIN" == /* && -x "$TRUE_BIN" ]] || die "cannot find an absolute true executable"
ln -s "$TRUE_BIN" "$MOCK_BIN/sleep"

git init -q "$REPO"
git -C "$REPO" config user.name 'RingOut release-publisher test'
git -C "$REPO" config user.email 'release-publisher-test@example.invalid'
printf 'fixture\n' >"$REPO/fixture.txt"
git -C "$REPO" add fixture.txt
git -C "$REPO" commit -qm 'fixture commit'
git -C "$REPO" tag -a "$TAG" -m 'release fixture'
COMMIT=$(git -C "$REPO" rev-parse 'HEAD^{commit}')
TAG_OBJECT=$(git -C "$REPO" rev-parse "refs/tags/$TAG")
MARKER="<!-- ringout-release-source:v1 repository=$REPOSITORY tag=$TAG tag-object=$TAG_OBJECT commit=$COMMIT -->"

PAYLOAD="$WORK/RingOut-test.zip"
CHECKSUM="$PAYLOAD.sha256"
printf 'immutable release fixture\n' >"$PAYLOAD"
PAYLOAD_DIGEST=$(sha256sum "$PAYLOAD" | awk '{print $1}')
printf '%s  %s\n' "$PAYLOAD_DIGEST" "$(basename "$PAYLOAD")" >"$CHECKSUM"
ASSETS_JSON="$WORK/assets.json"
python3 - "$ASSETS_JSON" "$PAYLOAD" "$CHECKSUM" <<'PY'
import hashlib
import json
import pathlib
import sys

records = []
for asset_id, name in enumerate(sys.argv[2:], start=7001):
    path = pathlib.Path(name)
    records.append({
        "id": asset_id,
        "name": path.name,
        "digest": "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest(),
        "size": path.stat().st_size,
        "state": "uploaded",
    })
pathlib.Path(sys.argv[1]).write_text(json.dumps(records) + "\n")
PY

LAST_OUTPUT=""
LAST_STATE=""
LAST_STATUS=0
run_case() {
  local name=$1 scenario=$2 role=$3
  LAST_OUTPUT="$WORK/$name.output"
  LAST_STATE="$WORK/$name.state"
  set +e
  (
    cd "$REPO"
    PATH="$MOCK_BIN:$PATH" \
    GITHUB_REPOSITORY=$REPOSITORY \
    GITHUB_TOKEN=fixture-token \
    GITHUB_SHA=$COMMIT \
    GITHUB_REF_TYPE=tag \
    GITHUB_REF_NAME=$TAG \
    GITHUB_REF="refs/tags/$TAG" \
    GITHUB_API_URL=https://mock.invalid \
    MOCK_STATE=$LAST_STATE \
    MOCK_SCENARIO=$scenario \
    MOCK_TAG=$TAG \
    MOCK_TAG_OBJECT=$TAG_OBJECT \
    MOCK_COMMIT=$COMMIT \
    MOCK_MARKER=$MARKER \
    MOCK_ASSETS_JSON=$ASSETS_JSON \
      "$PUBLISHER" "$role" --tag "$TAG" "$PAYLOAD" "$CHECKSUM"
  ) >"$LAST_OUTPUT" 2>&1
  LAST_STATUS=$?
  set -e
}

expect_success() {
  ((LAST_STATUS == 0)) || {
    sed -n '1,200p' "$LAST_OUTPUT" >&2
    die "$1 unexpectedly failed with status $LAST_STATUS"
  }
}

expect_failure() {
  ((LAST_STATUS != 0)) || die "$1 unexpectedly succeeded"
}

expect_output() {
  grep -Fq -- "$1" "$LAST_OUTPUT" || {
    sed -n '1,200p' "$LAST_OUTPUT" >&2
    die "missing expected output: $1"
  }
}

assert_request_count() {
  local method=$1 path_fragment=$2 expected=$3
  python3 - "$LAST_STATE" "$method" "$path_fragment" "$expected" <<'PY'
import json
import pathlib
import sys

state = json.loads(pathlib.Path(sys.argv[1]).read_text())
method, fragment, expected = sys.argv[2], sys.argv[3], int(sys.argv[4])
actual = sum(item["method"] == method and fragment in item["url"] for item in state["log"])
if actual != expected:
    raise SystemExit(f"expected {expected} {method} requests containing {fragment!r}, got {actual}")
PY
}

assert_request_seen() {
  local method=$1 path_fragment=$2
  python3 - "$LAST_STATE" "$method" "$path_fragment" <<'PY'
import json
import pathlib
import sys

state = json.loads(pathlib.Path(sys.argv[1]).read_text())
if not any(item["method"] == sys.argv[2] and sys.argv[3] in item["url"] for item in state["log"]):
    raise SystemExit(f"did not see {sys.argv[2]} request containing {sys.argv[3]!r}")
PY
}

# The old tag endpoint is mocked as 404. A list-visible draft must still be
# found, validated, and left untouched by a joiner.
run_case list_visible visible --join-release
expect_success list_visible
assert_request_count GET /releases/tags/ 0
assert_request_count POST /releases 0

# Full 100-entry pages must not hide the exact-tag draft on a later page.
run_case pagination pagination --join-release
expect_success pagination
assert_request_seen GET 'page=2'
assert_request_count POST /releases 0

# Offset pagination may repeat the same release id if another release appears
# between requests. An identical id is one release, not an ambiguous duplicate.
run_case pagination_dedup pagination-dedup --join-release
expect_success pagination_dedup
assert_request_seen GET 'page=2'
assert_request_count POST /releases 0

# A joiner polls for the creator but is never authorized to create a release.
run_case join_waits join-delayed --join-release
expect_success join_waits
expect_output 'waiting for the Windows release-creator job'
assert_request_count POST /releases 0

# Validation failures discovered after one or more empty join polls must not be
# swallowed by Bash's conditional-function errexit rules.
run_case join_delayed_mismatch join-delayed-mismatch --join-release
expect_failure join_delayed_mismatch
expect_output 'draft release source marker is absent, different, or duplicated'
assert_request_count POST /releases 0

# A malformed or truncated authenticated release listing is an API failure,
# never evidence that the exact tag is absent and safe to create.
run_case malformed_list malformed-list --create-release
expect_failure malformed_list
expect_output 'could not parse release listing page 1'
assert_request_count POST /releases 0

run_case truncated_list truncated-list --create-release
expect_failure truncated_list
expect_output 'could not parse release listing page 1'
assert_request_count POST /releases 0

# The sole creator accepts a 201 only after the unique list-visible draft is
# reconciled and source-bound.
run_case create_201 create201 --create-release
expect_success create_201
expect_output 'created source-bound draft prerelease'
assert_request_count POST /releases 1

# Exercise the actual mutation path too: a fresh creator uploads both absent
# files, re-lists their REST digests, and completes the coherent postcondition.
run_case fresh_upload fresh-upload --create-release
expect_success fresh_upload
expect_output "uploaded asset: $(basename "$PAYLOAD")"
expect_output "uploaded asset: $(basename "$CHECKSUM")"
expect_output 'verified 2 immutable asset(s)'
assert_request_count POST /releases/1234/assets 2
python3 - "$LAST_STATE" <<'PY'
import json
import pathlib
import sys

state = json.loads(pathlib.Path(sys.argv[1]).read_text())
if state.get("post_releases") != 1:
    raise SystemExit(f"expected one release create POST, got {state.get('post_releases')}")
if len(state.get("uploaded_assets", [])) != 2:
    raise SystemExit(f"expected two uploaded assets, got {state.get('uploaded_assets')}")
PY

# A 422 can be reconciled only when a single exact, valid draft becomes
# visible; the error itself is never treated as evidence that one exists.
run_case create_422 race422 --create-release
expect_success create_422
expect_output 'release creation returned HTTP 422'
assert_request_count POST /releases 1

# A connection failure after GitHub committed the create is ambiguous. It must
# be reconciled by listing without a second create attempt.
run_case create_transport transport-created --create-release
expect_success create_transport
expect_output 'transport result is ambiguous'
assert_request_count POST /releases 1

# The same single-attempt reconciliation applies to an ambiguous 5xx.
run_case create_500 server500-created --create-release
expect_success create_500
expect_output 'returned ambiguous HTTP 500'
assert_request_count POST /releases 1

# A joiner times out read-only when no exact tag exists.
run_case join_zero zero --join-release
expect_failure join_zero
expect_output 'source-bound draft release was not visible for the join-release job'
assert_request_count POST /releases 0

# Likewise, a creator cannot proceed after a 422 if no unique draft appears.
run_case create_zero create-zero --create-release
expect_failure create_zero
expect_output 'draft release was not visible after creation result HTTP 422 curl-exit 0'
assert_request_count POST /releases 1

# Multiple exact-tag drafts are always ambiguous and must hard-fail before any
# upload or creation mutation.
run_case duplicate duplicate --join-release
expect_failure duplicate
expect_output "release listing contains 2 releases for tag $TAG"
assert_request_count POST /releases 0

# GitHub permits two simultaneous draft creations to both return 201. The
# creator must detect that state during reconciliation and stop before upload.
run_case duplicate_201 duplicate201 --create-release
expect_failure duplicate_201
expect_output "release listing contains 2 releases for tag $TAG"
assert_request_count POST /releases 1

# A unique draft with the wrong source marker is not joinable.
run_case source_mismatch source-mismatch --join-release
expect_failure source_mismatch
expect_output 'draft release source marker is absent, different, or duplicated'
assert_request_count POST /releases 0

# The marker and remote tag are not substitutes for the release object's own
# target binding; it must name the same exact peeled commit too.
run_case wrong_target wrong-target --join-release
expect_failure wrong_target
expect_output 'release target_commitish does not match the source commit'
assert_request_count POST /releases 0

echo "publish-tag-draft-assets tests passed"
