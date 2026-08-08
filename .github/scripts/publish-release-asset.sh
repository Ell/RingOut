#!/bin/bash
# Publish a build artifact as a GitHub RELEASE asset instead of an Actions
# artifact.
#
# Why: Actions artifact storage is a single quota'd pool shared by every workflow
# in the repo, and exceeding it does not fail the offending upload -- it fails
# EVERY upload, including small ones from unrelated jobs, with
# "Artifact storage quota has been hit". That happened here: a ~294 MB Windows
# bundle per push filled the pool and the 22 MB Deck package became collateral.
# Release assets do not count against that quota at all, and they get a stable
# download URL, which an Actions artifact never has.
#
# Uses the REST API through curl rather than a Node action or the gh CLI,
# because the Deck job runs inside a bare debian:12 container where neither is
# present and the whole point of that container is to control what is installed.
#
#   $1 = tag       rolling tag to publish under (e.g. deck-latest)
#   $2 = title     human-readable release title
#   $3 = file      path to the asset to upload
#
# Requires GITHUB_TOKEN with contents:write, plus GITHUB_REPOSITORY and
# GITHUB_SHA from the workflow environment.
set -euo pipefail

TAG="${1:?tag required}"
TITLE="${2:?title required}"
FILE="${3:?file required}"
API="https://api.github.com/repos/${GITHUB_REPOSITORY:?}"
UPLOADS="https://uploads.github.com/repos/${GITHUB_REPOSITORY}"
NAME="$(basename "$FILE")"

[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 1; }

auth=(-H "Authorization: Bearer ${GITHUB_TOKEN:?}"
      -H "Accept: application/vnd.github+json"
      -H "X-GitHub-Api-Version: 2022-11-28")

# A rolling tag is reused every push, so the release may or may not exist yet.
# Ask first: creating one that exists is a 422, and the error body is not worth
# parsing when a GET answers the question directly.
# Parsed with python3 (already a build dependency), not sed: matching on
# GitHub's JSON whitespace works right up until it doesn't.
# Tolerates empty input: the first lookup 404s when the rolling release does not
# exist yet, which is the normal first-run path, not an error worth a traceback.
jget() { python3 -c 'import json,sys
raw = sys.stdin.read().strip()
if raw:
    d = json.loads(raw)
    print(d.get(sys.argv[1], "") if isinstance(d, dict) else "")' "$1"; }

release_id="$(curl -fsSL "${auth[@]}" "$API/releases/tags/$TAG" 2>/dev/null | jget id || true)"

if [ -z "$release_id" ]; then
  echo "creating release $TAG"
  release_id="$(curl -fsSL "${auth[@]}" -X POST "$API/releases" \
      -d "{\"tag_name\":\"$TAG\",\"name\":\"$TITLE\",\"prerelease\":true,
           \"target_commitish\":\"${GITHUB_SHA:-main}\",
           \"body\":\"Rolling build. Replaced on every push to main.\"}" \
      | jget id)"
else
  # Point the existing rolling release at this commit, or the tag keeps naming
  # whatever it was first cut from and the download stops matching the notes.
  curl -fsSL "${auth[@]}" -X PATCH "$API/releases/$release_id" \
      -d "{\"name\":\"$TITLE\",\"target_commitish\":\"${GITHUB_SHA:-main}\"}" > /dev/null
  echo "updating release $TAG (id $release_id)"
fi

[ -n "$release_id" ] || { echo "could not resolve a release id for $TAG" >&2; exit 1; }

# An asset name must be unique within a release, and re-uploading does not
# replace: it 422s. Delete the previous one first.
old="$(curl -fsSL "${auth[@]}" "$API/releases/$release_id/assets" \
       | python3 -c 'import json,sys
raw = sys.stdin.read().strip()
if raw:
    print(next((str(a["id"]) for a in json.loads(raw) if a["name"] == sys.argv[1]), ""))' "$NAME" || true)"
if [ -n "$old" ]; then
  echo "removing previous asset $NAME (id $old)"
  curl -fsSL "${auth[@]}" -X DELETE "$API/releases/assets/$old" || true
fi

echo "uploading $NAME ($(du -h "$FILE" | cut -f1))"
curl -fsSL "${auth[@]}" -H "Content-Type: application/octet-stream" \
     --data-binary @"$FILE" \
     "$UPLOADS/releases/$release_id/assets?name=$NAME" > /dev/null

echo "published: https://github.com/${GITHUB_REPOSITORY}/releases/download/$TAG/$NAME"
