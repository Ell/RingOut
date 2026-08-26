#!/usr/bin/env bash
# Create (or join) the source-bound draft prerelease for an immutable tag and
# upload verified assets without ever deleting or replacing an existing asset.
#
# Usage:
#   publish-tag-draft-assets.sh --create-release --tag v1.2.1-ell.11 FILE FILE.sha256 [...]
#   publish-tag-draft-assets.sh --join-release --tag v1.2.1-ell.11 FILE FILE.sha256 [...]
#
# Every payload must have an adjacent .sha256 file in the argument list. The
# Windows is the only release creator; other platform jobs wait for and join
# its draft. Equal assets are idempotent, while a same-name/different-digest
# asset is a hard failure.
set -euo pipefail

die() {
  echo "release upload: $*" >&2
  exit 1
}

TAG=""
RELEASE_ROLE=""
while (($#)); do
  case "$1" in
    --create-release|--join-release)
      [[ -z "$RELEASE_ROLE" ]] || die "release role was specified more than once"
      RELEASE_ROLE=${1#--}
      shift
      ;;
    --tag)
      (($# >= 2)) || die "--tag needs a value"
      TAG=$2
      shift 2
      ;;
    --)
      shift
      break
      ;;
    -*) die "unknown option: $1" ;;
    *) break ;;
  esac
done

[[ -n "$RELEASE_ROLE" ]] || die "exactly one of --create-release or --join-release is required"
[[ -n "$TAG" ]] || die "--tag is required"
[[ "$TAG" =~ ^v[0-9][0-9A-Za-z._+-]*$ ]] || die "invalid release tag: $TAG"
(($# >= 2)) || die "at least one payload and checksum are required"

REPOSITORY=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}
[[ "$REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
  die "invalid GITHUB_REPOSITORY: $REPOSITORY"
TOKEN=${GITHUB_TOKEN:-${GH_TOKEN:-}}
[[ -n "$TOKEN" ]] || die "GITHUB_TOKEN (or GH_TOKEN) is required"
EXPECTED_SHA=${GITHUB_SHA:?GITHUB_SHA is required}
[[ "$EXPECTED_SHA" =~ ^[0-9a-fA-F]{40}$ ]] || die "invalid GITHUB_SHA: $EXPECTED_SHA"
EXPECTED_SHA=${EXPECTED_SHA,,}

[[ ${GITHUB_REF_TYPE:?GITHUB_REF_TYPE is required} == tag ]] ||
  die "this publisher only accepts tag-triggered jobs"
[[ ${GITHUB_REF_NAME:?GITHUB_REF_NAME is required} == "$TAG" ]] ||
  die "GITHUB_REF_NAME does not match $TAG"
[[ ${GITHUB_REF:?GITHUB_REF is required} == "refs/tags/$TAG" ]] ||
  die "GITHUB_REF does not match refs/tags/$TAG"

command -v curl >/dev/null || die "curl is required"
command -v git >/dev/null || die "git is required"
command -v python3 >/dev/null || die "python3 is required"
command -v realpath >/dev/null || die "realpath is required"
command -v sha256sum >/dev/null || die "sha256sum is required"
command -v stat >/dev/null || die "stat is required"

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || die "not in a Git checkout"
cd "$REPO_ROOT"

SOURCE_HEAD=$(git rev-parse --verify 'HEAD^{commit}' 2>/dev/null) ||
  die "cannot resolve HEAD"
SOURCE_TAG_OBJECT=$(git rev-parse --verify "refs/tags/$TAG" 2>/dev/null) ||
  die "local tag refs/tags/$TAG is missing"
SOURCE_COMMIT=$(git rev-parse --verify "refs/tags/$TAG^{commit}" 2>/dev/null) ||
  die "local tag $TAG does not peel to a commit"
SOURCE_HEAD=${SOURCE_HEAD,,}
SOURCE_TAG_OBJECT=${SOURCE_TAG_OBJECT,,}
SOURCE_COMMIT=${SOURCE_COMMIT,,}
# On a tag-push workflow GitHub exposes the triggering peeled commit as
# GITHUB_SHA. The Git refs API separately exposes the annotated tag object; both
# identities are checked here and again remotely rather than conflating them.
[[ "$SOURCE_HEAD" == "$SOURCE_COMMIT" ]] ||
  die "HEAD $SOURCE_HEAD is not tag $TAG commit $SOURCE_COMMIT"
[[ "$EXPECTED_SHA" == "$SOURCE_COMMIT" ]] ||
  die "GITHUB_SHA $EXPECTED_SHA is not tag $TAG commit $SOURCE_COMMIT"

SOURCE_MARKER="<!-- ringout-release-source:v1 repository=$REPOSITORY tag=$TAG tag-object=$SOURCE_TAG_OBJECT commit=$SOURCE_COMMIT -->"
RELEASE_TITLE="Ring Out $TAG (experimental builds)"
RELEASE_NOTES="Experimental release assets. No game data is included. Inspect each package, source-provenance file, checksum, and platform QA result before publishing."
RELEASE_BODY="$RELEASE_NOTES"$'\n\n'"$SOURCE_MARKER"

declare -A ASSET_PATHS=()
ASSET_NAMES=()
for path in "$@"; do
  [[ -f "$path" && -s "$path" ]] || die "asset is not a nonempty regular file: $path"
  name=$(basename -- "$path")
  [[ "$name" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] ||
    die "unsupported asset name: $name"
  [[ -z ${ASSET_PATHS[$name]+x} ]] || die "duplicate asset basename: $name"
  ASSET_PATHS[$name]=$(realpath -- "$path")
  ASSET_NAMES+=("$name")
done

# Require complete payload/checksum pairs before making any API mutation, and
# validate that every checksum names and hashes its adjacent payload exactly.
for name in "${ASSET_NAMES[@]}"; do
  if [[ "$name" == *.sha256 ]]; then
    payload=${name%.sha256}
    [[ -n ${ASSET_PATHS[$payload]+x} ]] ||
      die "checksum $name has no matching payload argument"
    python3 - "${ASSET_PATHS[$name]}" "${ASSET_PATHS[$payload]}" "$payload" <<'PY'
import hashlib
import pathlib
import re
import sys

checksum_path = pathlib.Path(sys.argv[1])
payload_path = pathlib.Path(sys.argv[2])
expected_name = sys.argv[3]
try:
    text = checksum_path.read_text(encoding="ascii")
except (OSError, UnicodeError) as exc:
    raise SystemExit(f"release upload: cannot read {checksum_path}: {exc}")
match = re.fullmatch(r"([0-9a-f]{64})  ([^\r\n]+)\n", text)
if not match or match.group(2) != expected_name:
    raise SystemExit(
        f"release upload: {checksum_path.name} must contain exactly "
        f"'<64 lowercase hex>  {expected_name}\\n'"
    )
digest = hashlib.sha256()
with payload_path.open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
if digest.hexdigest() != match.group(1).lower():
    raise SystemExit(f"release upload: checksum mismatch for {expected_name}")
PY
  else
    checksum_name="$name.sha256"
    [[ -n ${ASSET_PATHS[$checksum_name]+x} ]] ||
      die "payload $name has no matching .sha256 argument"
  fi
done

TMPDIR_RELEASE=$(mktemp -d)
trap 'rm -rf -- "$TMPDIR_RELEASE"' EXIT
API_ROOT=${GITHUB_API_URL:-https://api.github.com}
API_ROOT=${API_ROOT%/}
API="$API_ROOT/repos/$REPOSITORY"
AUTH_HEADERS=(
  -H "Authorization: Bearer $TOKEN"
  -H 'Accept: application/vnd.github+json'
  -H 'X-GitHub-Api-Version: 2022-11-28'
  -H 'User-Agent: RingOut-release-publisher'
)
REQUEST_NUMBER=0
HTTP_CODE=""
HTTP_BODY=""
CURL_EXIT=0

api_request_impl() {
  local retry_mode=$1 method=$2 url=$3 data_file=${4:-}
  local content_type=${5:-application/json}
  REQUEST_NUMBER=$((REQUEST_NUMBER + 1))
  HTTP_BODY="$TMPDIR_RELEASE/response-$REQUEST_NUMBER.json"
  local args=(
    --disable
    --silent --show-error
    --connect-timeout 20
    -o "$HTTP_BODY" -w '%{http_code}'
    "${AUTH_HEADERS[@]}"
    -X "$method"
  )
  if [[ "$retry_mode" == retry ]]; then
    args+=(--retry 5 --retry-all-errors --retry-delay 1 --retry-max-time 90)
  fi
  if [[ -n "$data_file" ]]; then
    args+=(-H "Content-Type: $content_type" --data-binary "@$data_file")
  fi
  set +e
  HTTP_CODE=$(curl "${args[@]}" "$url")
  CURL_EXIT=$?
  set -e
  [[ "$HTTP_CODE" =~ ^[0-9]{3}$ ]] || HTTP_CODE=000
  if [[ "$retry_mode" == retry ]]; then
    ((CURL_EXIT == 0)) ||
      die "GitHub API request failed ($method $url, curl exit $CURL_EXIT)"
    [[ "$HTTP_CODE" != 000 ]] || die "GitHub API returned no HTTP status"
  fi
}

api_request() {
  api_request_impl retry "$@"
}

# Creating a draft is not idempotent: GitHub permits multiple drafts with the
# same tag_name. Never let curl resend this POST. A transport failure or 5xx is
# reconciled through authenticated read-only listing instead.
api_request_once() {
  api_request_impl once "$@"
}

api_error() {
  python3 - "$HTTP_BODY" <<'PY'
import json
import pathlib
import sys

try:
    value = json.loads(pathlib.Path(sys.argv[1]).read_text())
    print(value.get("message", "unknown GitHub API error") if isinstance(value, dict) else "unknown GitHub API error")
except Exception:
    print("unparseable GitHub API error")
PY
}

urlencode() {
  python3 - "$1" <<'PY'
import sys
import urllib.parse
print(urllib.parse.quote(sys.argv[1], safe=""))
PY
}

validate_local_source() {
  local head tag_object commit
  head=$(git rev-parse --verify 'HEAD^{commit}' 2>/dev/null) || die "cannot re-resolve HEAD"
  tag_object=$(git rev-parse --verify "refs/tags/$TAG" 2>/dev/null) ||
    die "local tag disappeared during upload"
  commit=$(git rev-parse --verify "refs/tags/$TAG^{commit}" 2>/dev/null) ||
    die "local tag no longer peels to a commit"
  [[ "${head,,}" == "$SOURCE_HEAD" ]] || die "HEAD changed during release upload"
  [[ "${tag_object,,}" == "$SOURCE_TAG_OBJECT" ]] ||
    die "local tag object changed during release upload"
  [[ "${commit,,}" == "$SOURCE_COMMIT" ]] ||
    die "local tag commit changed during release upload"
}

json_tag_ref() {
  python3 - "$HTTP_BODY" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text())
obj = value.get("object") or {}
print(f'{obj.get("type", "")}|{obj.get("sha", "")}')
PY
}

validate_remote_source() {
  local encoded_tag record object_type object_sha depth
  encoded_tag=$(urlencode "$TAG")
  api_request GET "$API/git/ref/tags/$encoded_tag"
  [[ "$HTTP_CODE" == 200 ]] ||
    die "remote tag lookup failed with HTTP $HTTP_CODE: $(api_error)"
  record=$(json_tag_ref)
  IFS='|' read -r object_type object_sha <<<"$record"
  object_sha=${object_sha,,}
  [[ "$object_sha" == "$SOURCE_TAG_OBJECT" ]] ||
    die "remote tag object $object_sha does not match local $SOURCE_TAG_OBJECT"

  depth=0
  while [[ "$object_type" == tag ]]; do
    depth=$((depth + 1))
    ((depth <= 16)) || die "remote tag nesting is unexpectedly deep"
    api_request GET "$API/git/tags/$object_sha"
    [[ "$HTTP_CODE" == 200 ]] ||
      die "remote annotated-tag lookup failed with HTTP $HTTP_CODE: $(api_error)"
    record=$(json_tag_ref)
    IFS='|' read -r object_type object_sha <<<"$record"
    object_sha=${object_sha,,}
  done
  [[ "$object_type" == commit ]] || die "remote tag peels to unsupported object type: $object_type"
  [[ "$object_sha" == "$SOURCE_COMMIT" ]] ||
    die "remote tag commit $object_sha does not match local $SOURCE_COMMIT"
}

validate_source() {
  validate_local_source
  validate_remote_source
}

RELEASE_JSON=""
get_release() {
  local page=1 count page_json combined match_count selected
  local matches="$TMPDIR_RELEASE/release-matches.json"
  RELEASE_JSON=""
  printf '[]\n' >"$matches" || die "cannot initialize release-listing accumulator"
  while :; do
    # GitHub's tag-specific release endpoint does not expose draft releases,
    # even to an authenticated caller. Enumerate the authenticated release
    # collection instead and accept only one exact tag_name match.
    api_request GET "$API/releases?per_page=100&page=$page"
    [[ "$HTTP_CODE" == 200 ]] ||
      die "release listing failed with HTTP $HTTP_CODE: $(api_error)"
    page_json=$HTTP_BODY
    count=$(python3 - "$page_json" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
    raise SystemExit("release upload: GitHub release listing is not an array of objects")
print(len(value))
PY
) || die "could not parse release listing page $page"
    combined="$TMPDIR_RELEASE/release-matches-$page.json"
    python3 - "$matches" "$page_json" "$combined" "$TAG" <<'PY' || die "could not accumulate release listing page $page"
import json
import pathlib
import sys

left = json.loads(pathlib.Path(sys.argv[1]).read_text())
right = json.loads(pathlib.Path(sys.argv[2]).read_text())
tag = sys.argv[4]
for release in right:
    if release.get("tag_name") != tag:
        continue
    release_id = release.get("id")
    previous = next(
        (
            item
            for item in left
            if type(release_id) is int
            and type(item.get("id")) is int
            and item.get("id") == release_id
        ),
        None,
    )
    if previous is None:
        left.append(release)
        continue
    critical = (
        "id",
        "tag_name",
        "target_commitish",
        "draft",
        "prerelease",
        "body",
        "upload_url",
    )
    if any(previous.get(key) != release.get(key) for key in critical):
        raise SystemExit(
            f"release upload: release id {release_id} changed during paginated listing"
        )
pathlib.Path(sys.argv[3]).write_text(
    json.dumps(left) + "\n"
)
PY
    [[ -s "$combined" ]] || die "could not accumulate release listing page $page"
    mv -- "$combined" "$matches" || die "could not retain release listing page $page"
    ((count == 100)) || break
    page=$((page + 1))
    ((page <= 100)) || die "repository has too many releases to inspect safely"
  done

  match_count=$(python3 - "$matches" <<'PY'
import json
import pathlib
import sys
print(len(json.loads(pathlib.Path(sys.argv[1]).read_text())))
PY
) || die "could not count exact-tag release matches"
  case "$match_count" in
    0) return 1 ;;
    1)
      selected="$TMPDIR_RELEASE/release-selected-$REQUEST_NUMBER.json"
      python3 - "$matches" "$selected" <<'PY' || die "could not retain the selected draft release"
import json
import pathlib
import sys
matches = json.loads(pathlib.Path(sys.argv[1]).read_text())
pathlib.Path(sys.argv[2]).write_text(json.dumps(matches[0]) + "\n")
PY
      [[ -s "$selected" ]] || die "could not retain the selected draft release"
      RELEASE_JSON=$selected
      return 0
      ;;
    *) die "release listing contains $match_count releases for tag $TAG" ;;
  esac
}

validate_release() {
  python3 - "$RELEASE_JSON" "$TAG" "$SOURCE_MARKER" "$SOURCE_COMMIT" <<'PY' || die "draft release validation failed"
import json
import pathlib
import sys

release = json.loads(pathlib.Path(sys.argv[1]).read_text())
tag, marker, commit = sys.argv[2:]
if release.get("tag_name") != tag:
    raise SystemExit("release upload: release tag_name does not match the triggering tag")
if release.get("target_commitish") != commit:
    raise SystemExit("release upload: release target_commitish does not match the source commit")
if release.get("draft") is not True:
    raise SystemExit("release upload: refusing to upload unless isDraft is true")
if release.get("prerelease") is not True:
    raise SystemExit("release upload: refusing to upload unless prerelease is true")
body = release.get("body")
if not isinstance(body, str):
    raise SystemExit("release upload: draft release has no source-binding body")
source_lines = [line for line in body.splitlines() if "ringout-release-source:" in line]
if source_lines != [marker]:
    raise SystemExit("release upload: draft release source marker is absent, different, or duplicated")
if type(release.get("id")) is not int or release["id"] <= 0:
    raise SystemExit("release upload: draft release has no valid id")
upload_url = release.get("upload_url")
if not isinstance(upload_url, str) or not upload_url:
    raise SystemExit("release upload: draft release has no upload_url")
PY
}

release_value() {
  python3 - "$RELEASE_JSON" "$1" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text()).get(sys.argv[2], "")
print(value if value is not None else "")
PY
}

wait_for_release() {
  local attempts=$1 delay=$2 attempt
  for ((attempt = 1; attempt <= attempts; attempt++)); do
    if get_release; then
      validate_release
      return 0
    fi
    sleep "$delay" || die "release visibility wait failed"
  done
  return 1
}

validate_source

if get_release; then
  validate_release
elif [[ "$RELEASE_ROLE" == join-release ]]; then
  echo "waiting for the Windows release-creator job to expose draft $TAG"
  wait_for_release 90 10 ||
    die "source-bound draft release was not visible for the join-release job"
else
  CREATE_JSON="$TMPDIR_RELEASE/create.json"
  python3 - "$CREATE_JSON" "$TAG" "$RELEASE_TITLE" "$RELEASE_BODY" "$SOURCE_COMMIT" <<'PY' || die "could not prepare release creation request"
import json
import pathlib
import sys

path, tag, title, body, commit = sys.argv[1:]
pathlib.Path(path).write_text(json.dumps({
    "tag_name": tag,
    "target_commitish": commit,
    "name": title,
    "body": body,
    "draft": True,
    "prerelease": True,
}) + "\n")
PY
  # Recheck both source identity and exact-tag absence after preparing the
  # request, minimizing the remaining non-atomic gap before the sole creator's
  # one-shot mutation.
  validate_source
  if get_release; then
    validate_release
  else
    api_request_once POST "$API/releases" "$CREATE_JSON"
    creation_code=$HTTP_CODE
    creation_curl_exit=$CURL_EXIT
    if ((creation_curl_exit != 0)); then
      echo "release creation transport result is ambiguous (curl exit $creation_curl_exit); reconciling without retry"
    elif [[ "$creation_code" == 201 ]]; then
      echo "created source-bound draft prerelease $TAG"
    elif [[ "$creation_code" == 422 ]]; then
      echo "release creation returned HTTP 422; reconciling the unique source-bound draft"
    elif [[ "$creation_code" == 5?? ]]; then
      echo "release creation returned ambiguous HTTP $creation_code; reconciling without retry"
    elif [[ "$creation_code" == 000 ]]; then
      echo "release creation returned no HTTP status; reconciling without retry"
    else
      die "release creation failed with HTTP $creation_code: $(api_error)"
    fi
    wait_for_release 60 2 ||
      die "draft release was not visible after creation result HTTP $creation_code curl-exit $creation_curl_exit"
  fi
fi

RELEASE_ID=$(release_value id)
UPLOAD_URL=$(release_value upload_url)
UPLOAD_URL=${UPLOAD_URL%%\{*}
[[ "$RELEASE_ID" =~ ^[0-9]+$ && -n "$UPLOAD_URL" ]] ||
  die "could not resolve release id/upload URL"

ASSETS_JSON="$TMPDIR_RELEASE/assets.json"
fetch_assets() {
  local page=1 count page_json combined
  printf '[]\n' >"$ASSETS_JSON" || die "cannot initialize asset-listing accumulator"
  while :; do
    api_request GET "$API/releases/$RELEASE_ID/assets?per_page=100&page=$page"
    [[ "$HTTP_CODE" == 200 ]] ||
      die "asset listing failed with HTTP $HTTP_CODE: $(api_error)"
    page_json=$HTTP_BODY
    count=$(python3 - "$page_json" <<'PY'
import json
import pathlib
import sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if not isinstance(value, list):
    raise SystemExit("release upload: GitHub asset listing is not an array")
print(len(value))
PY
) || die "could not parse asset listing page $page"
    combined="$TMPDIR_RELEASE/assets-combined-$page.json"
    python3 - "$ASSETS_JSON" "$page_json" "$combined" <<'PY' || die "could not accumulate asset listing page $page"
import json
import pathlib
import sys
left = json.loads(pathlib.Path(sys.argv[1]).read_text())
right = json.loads(pathlib.Path(sys.argv[2]).read_text())
pathlib.Path(sys.argv[3]).write_text(json.dumps(left + right) + "\n")
PY
    [[ -s "$combined" ]] || die "could not accumulate asset listing page $page"
    mv -- "$combined" "$ASSETS_JSON" || die "could not retain asset listing page $page"
    ((count == 100)) || break
    page=$((page + 1))
    ((page <= 100)) || die "release has too many assets to inspect safely"
  done
}

asset_record() {
  python3 - "$ASSETS_JSON" "$1" <<'PY' || die "could not inspect release asset records"
import json
import pathlib
import sys
assets = [a for a in json.loads(pathlib.Path(sys.argv[1]).read_text())
          if a.get("name") == sys.argv[2]]
if not assets:
    print("0||||")
elif len(assets) == 1:
    asset = assets[0]
    print("1|{}|{}|{}|{}".format(
        asset.get("id", ""), asset.get("digest", "") or "",
        asset.get("size", ""), asset.get("state", "")))
else:
    print(f"{len(assets)}||||")
PY
}

ASSET_RESULT=""
inspect_asset() {
  local name=$1 path=${ASSET_PATHS[$1]} record count id digest size state
  local expected_digest expected_size
  expected_digest="sha256:$(sha256sum "$path" | awk '{print $1}')" ||
    die "could not hash local asset $name"
  expected_size=$(stat -c '%s' "$path") || die "could not size local asset $name"
  record=$(asset_record "$name") || die "could not inspect release asset $name"
  IFS='|' read -r count id digest size state <<<"$record"
  case "$count" in
    0)
      ASSET_RESULT=absent
      ;;
    1)
      [[ "$id" =~ ^[0-9]+$ ]] || die "asset $name has an invalid REST id"
      if [[ -z "$digest" || "$state" != uploaded ]]; then
        # GitHub can expose a just-created asset before its digest/state has
        # converged. It is safe to poll this read-only state; a nonempty wrong
        # digest is never treated as pending.
        [[ -z "$digest" || "$digest" == "$expected_digest" ]] ||
          die "asset $name already exists with digest $digest, expected $expected_digest"
        ASSET_RESULT=pending
        return
      fi
      [[ "$digest" == "$expected_digest" ]] ||
        die "asset $name already exists with digest $digest, expected $expected_digest"
      [[ "$size" == "$expected_size" ]] || die "asset $name has size $size, expected $expected_size"
      ASSET_RESULT=equal
      ;;
    *) die "release contains $count assets named $name" ;;
  esac
}

wait_for_asset() {
  local name=$1 attempt
  for ((attempt = 1; attempt <= 60; attempt++)); do
    fetch_assets || die "could not refresh release assets while waiting for $name"
    inspect_asset "$name" || die "could not inspect release asset while waiting for $name"
    [[ "$ASSET_RESULT" == equal ]] && return 0
    sleep 2 || die "asset visibility wait failed"
  done
  return 1
}

fetch_assets
for name in "${ASSET_NAMES[@]}"; do
  inspect_asset "$name"
  if [[ "$ASSET_RESULT" == equal ]]; then
    echo "asset already verified; skipping: $name"
    continue
  elif [[ "$ASSET_RESULT" == pending ]]; then
    echo "asset is visible but its digest/state is pending; waiting: $name"
    wait_for_asset "$name" || die "existing asset did not become verifiable: $name"
    echo "asset already verified; skipping: $name"
    continue
  fi

  # Recheck the draft and its immutable source binding immediately before each
  # mutation. There is no clobber/delete path anywhere in this script.
  validate_source
  get_release || die "draft release disappeared before uploading $name"
  validate_release
  [[ "$(release_value id)" == "$RELEASE_ID" ]] || die "release id changed during upload"

  encoded_name=$(urlencode "$name")
  api_request POST "$UPLOAD_URL?name=$encoded_name" "${ASSET_PATHS[$name]}" \
    application/octet-stream
  if [[ "$HTTP_CODE" == 201 ]]; then
    echo "uploaded asset: $name"
  elif [[ "$HTTP_CODE" == 422 ]]; then
    echo "asset-name race for $name; re-querying its immutable digest"
  else
    die "asset upload failed for $name with HTTP $HTTP_CODE: $(api_error)"
  fi
  wait_for_asset "$name" || die "uploaded asset did not become verifiable: $name"
done

# One coherent postcondition: the same draft is still source-bound, every local
# argument exists remotely with the exact REST SHA-256 digest, and neither the
# local nor remote tag moved while the two platform jobs were racing.
get_release || die "draft release disappeared during post-verification"
validate_release
[[ "$(release_value id)" == "$RELEASE_ID" ]] || die "release id changed during post-verification"
fetch_assets
for name in "${ASSET_NAMES[@]}"; do
  inspect_asset "$name"
  [[ "$ASSET_RESULT" == equal ]] || die "asset missing after upload: $name"
done
validate_source
get_release || die "draft release disappeared after source post-verification"
validate_release
[[ "$(release_value id)" == "$RELEASE_ID" ]] || die "release id changed after post-verification"

echo "verified ${#ASSET_NAMES[@]} immutable asset(s) on source-bound draft $TAG"
