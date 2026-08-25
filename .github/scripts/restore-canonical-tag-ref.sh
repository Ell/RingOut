#!/usr/bin/env bash
# actions/checkout checks out the peeled GITHUB_SHA for tag events and can
# overwrite an annotated local refs/tags/<name> with that commit. Restore the
# exact remote tag ref so release provenance retains both the tag object and
# its peeled commit. Non-tag runs deliberately do nothing.
set -euo pipefail

die() {
  echo "canonical tag ref: $*" >&2
  exit 1
}

if [[ ${GITHUB_REF_TYPE:-} != tag ]]; then
  echo "canonical tag ref: non-tag run; nothing to restore"
  exit 0
fi

TAG=${GITHUB_REF_NAME:?GITHUB_REF_NAME is required for a tag run}
REF=${GITHUB_REF:?GITHUB_REF is required for a tag run}
EXPECTED_SHA=${GITHUB_SHA:?GITHUB_SHA is required for a tag run}
REMOTE=origin

[[ "$TAG" =~ ^v[0-9][0-9A-Za-z._+-]*$ ]] || die "invalid release tag: $TAG"
[[ "$REF" == "refs/tags/$TAG" ]] || die "GITHUB_REF does not match refs/tags/$TAG"
[[ "$EXPECTED_SHA" =~ ^[0-9a-fA-F]{40}$ ]] || die "invalid GITHUB_SHA: $EXPECTED_SHA"
git check-ref-format "$REF" >/dev/null 2>&1 || die "invalid tag ref: $REF"
EXPECTED_SHA=${EXPECTED_SHA,,}

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || die "not in a Git checkout"
cd "$REPO_ROOT"
HEAD_COMMIT=$(git rev-parse --verify 'HEAD^{commit}' 2>/dev/null) || die "cannot resolve HEAD"
HEAD_COMMIT=${HEAD_COMMIT,,}
[[ "$HEAD_COMMIT" == "$EXPECTED_SHA" ]] ||
  die "HEAD $HEAD_COMMIT is not GITHUB_SHA $EXPECTED_SHA"

# --no-tags plus one fully qualified refspec prevents unrelated tag updates.
# The leading + is intentional: actions/checkout may already have replaced the
# annotated tag object with its peeled commit, which Git otherwise protects as
# a non-fast-forward tag update.
git fetch --force --no-tags --no-recurse-submodules "$REMOTE" \
  "+refs/tags/$TAG:refs/tags/$TAG"

TAG_OBJECT=$(git rev-parse --verify "$REF" 2>/dev/null) ||
  die "restored tag ref is missing: $REF"
TAG_OBJECT=${TAG_OBJECT,,}
TAG_TYPE=$(git cat-file -t "$TAG_OBJECT" 2>/dev/null) ||
  die "cannot inspect restored tag object $TAG_OBJECT"
case "$TAG_TYPE" in
  tag) ;;
  commit)
    # A lightweight tag has no separate tag object. Its ref must be the exact
    # triggering commit, never another commit that happens to peel cleanly.
    [[ "$TAG_OBJECT" == "$EXPECTED_SHA" ]] ||
      die "lightweight tag $TAG points to $TAG_OBJECT, expected $EXPECTED_SHA"
    ;;
  *) die "tag $TAG points to unsupported object type: $TAG_TYPE" ;;
esac

TAG_COMMIT=$(git rev-parse --verify "$REF^{commit}" 2>/dev/null) ||
  die "tag $TAG does not peel to a commit"
TAG_COMMIT=${TAG_COMMIT,,}
[[ "$TAG_COMMIT" == "$EXPECTED_SHA" ]] ||
  die "tag $TAG peels to $TAG_COMMIT, expected $EXPECTED_SHA"
[[ "$TAG_COMMIT" == "$HEAD_COMMIT" ]] ||
  die "tag $TAG commit $TAG_COMMIT is not HEAD $HEAD_COMMIT"

# Confirm that the ref installed by fetch still has the exact raw identity
# advertised by the remote. The publisher repeats this check through GitHub's
# refs API immediately before every release mutation.
REMOTE_RECORD=$(git ls-remote --refs "$REMOTE" "$REF") ||
  die "cannot read remote tag ref $REF"
REMOTE_COUNT=$(printf '%s\n' "$REMOTE_RECORD" | awk 'NF { count++ } END { print count + 0 }')
[[ "$REMOTE_COUNT" == 1 ]] || die "remote tag lookup returned $REMOTE_COUNT refs for $REF"
read -r REMOTE_OBJECT REMOTE_REF <<<"$REMOTE_RECORD"
REMOTE_OBJECT=${REMOTE_OBJECT,,}
[[ "$REMOTE_REF" == "$REF" ]] || die "remote returned unexpected ref: $REMOTE_REF"
[[ "$REMOTE_OBJECT" == "$TAG_OBJECT" ]] ||
  die "remote tag object $REMOTE_OBJECT does not match restored local $TAG_OBJECT"

echo "canonical tag ref: restored $TAG_TYPE object $TAG_OBJECT -> commit $TAG_COMMIT"
