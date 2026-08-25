#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HELPER=$(realpath "$SCRIPT_DIR/../restore-canonical-tag-ref.sh")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ringout-tag-ref-test.XXXXXXXX")
trap 'rm -rf -- "$WORK"' EXIT

die() {
  echo "restore-canonical-tag-ref test: $*" >&2
  exit 1
}

ORIGIN="$WORK/origin.git"
SEED="$WORK/seed"
RUNNER="$WORK/runner"
ANNOTATED_TAG=v1.2.3-ell.1
LIGHTWEIGHT_TAG=v1.2.3-ell.2
MOVED_TAG=v1.2.3-ell.3

git init --bare -q "$ORIGIN"
git init -q "$SEED"
git -C "$SEED" config user.name 'RingOut tag-ref test'
git -C "$SEED" config user.email 'tag-ref-test@example.invalid'
printf 'fixture\n' >"$SEED/fixture.txt"
git -C "$SEED" add fixture.txt
git -C "$SEED" commit -qm 'fixture commit'
COMMIT=$(git -C "$SEED" rev-parse HEAD)
git -C "$SEED" tag -a "$ANNOTATED_TAG" -m 'annotated fixture'
git -C "$SEED" tag "$LIGHTWEIGHT_TAG"
ANNOTATED_OBJECT=$(git -C "$SEED" rev-parse "refs/tags/$ANNOTATED_TAG")
git -C "$SEED" remote add origin "$ORIGIN"
git -C "$SEED" push -q origin HEAD:main "refs/tags/$ANNOTATED_TAG" \
  "refs/tags/$LIGHTWEIGHT_TAG"
git --git-dir="$ORIGIN" symbolic-ref HEAD refs/heads/main

git clone -q "$ORIGIN" "$RUNNER"

# Mimic actions/checkout's tag-event refspec: it fetches the peeled event SHA
# directly into refs/tags/<name>, clobbering the annotated tag object locally.
git -C "$RUNNER" fetch -q --force --no-tags origin \
  "+$COMMIT:refs/tags/$ANNOTATED_TAG"
[[ $(git -C "$RUNNER" rev-parse "refs/tags/$ANNOTATED_TAG") == "$COMMIT" ]] ||
  die "fixture did not clobber the annotated local tag"
[[ $(git -C "$RUNNER" cat-file -t "refs/tags/$ANNOTATED_TAG") == commit ]] ||
  die "clobbered local tag is not a lightweight commit ref"

(
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME=$ANNOTATED_TAG \
  GITHUB_REF="refs/tags/$ANNOTATED_TAG" \
  GITHUB_SHA=$COMMIT \
    "$HELPER"
)
[[ $(git -C "$RUNNER" rev-parse "refs/tags/$ANNOTATED_TAG") == "$ANNOTATED_OBJECT" ]] ||
  die "annotated tag object was not restored"
[[ $(git -C "$RUNNER" cat-file -t "refs/tags/$ANNOTATED_TAG") == tag ]] ||
  die "restored annotated tag does not have type tag"
[[ $(git -C "$RUNNER" rev-parse "refs/tags/$ANNOTATED_TAG^{commit}") == "$COMMIT" ]] ||
  die "restored annotated tag does not peel to the expected commit"

# Lightweight remote tags are supported, but only when the raw ref, peel,
# GITHUB_SHA, and HEAD are all the same commit.
(
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME=$LIGHTWEIGHT_TAG \
  GITHUB_REF="refs/tags/$LIGHTWEIGHT_TAG" \
  GITHUB_SHA=$COMMIT \
    "$HELPER"
)
[[ $(git -C "$RUNNER" rev-parse "refs/tags/$LIGHTWEIGHT_TAG") == "$COMMIT" ]] ||
  die "lightweight tag identity changed"
[[ $(git -C "$RUNNER" cat-file -t "refs/tags/$LIGHTWEIGHT_TAG") == commit ]] ||
  die "lightweight tag was not retained as a commit ref"

# Manual/branch runs must not require tag context or touch refs.
before=$(git -C "$RUNNER" rev-parse "refs/tags/$ANNOTATED_TAG")
(cd "$RUNNER" && GITHUB_REF_TYPE=branch "$HELPER")
after=$(git -C "$RUNNER" rev-parse "refs/tags/$ANNOTATED_TAG")
[[ "$after" == "$before" ]] || die "non-tag run changed a tag ref"

# Reject unsafe tag names before attempting a fetch.
if (
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME='v1/bad' \
  GITHUB_REF='refs/tags/v1/bad' \
  GITHUB_SHA=$COMMIT \
    "$HELPER" >/dev/null 2>&1
); then
  die "unsafe tag name was accepted"
fi

# A valid tag name paired with any other event ref must also fail before fetch.
if (
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME=$LIGHTWEIGHT_TAG \
  GITHUB_REF="refs/tags/$ANNOTATED_TAG" \
  GITHUB_SHA=$COMMIT \
    "$HELPER" >/dev/null 2>&1
); then
  die "mismatched GITHUB_REF was accepted"
fi

# GITHUB_SHA is the peeled commit in a tag-push run. A different event commit
# must not be accepted even when the remote tag itself is otherwise valid.
WRONG_SHA=0000000000000000000000000000000000000000
if (
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME=$LIGHTWEIGHT_TAG \
  GITHUB_REF="refs/tags/$LIGHTWEIGHT_TAG" \
  GITHUB_SHA=$WRONG_SHA \
    "$HELPER" >/dev/null 2>&1
); then
  die "tag/HEAD versus GITHUB_SHA mismatch was accepted"
fi

# A remote tag that now names another commit must not be accepted for the old
# event SHA, even though the runner's detached HEAD still matches that event.
printf 'second fixture\n' >>"$SEED/fixture.txt"
git -C "$SEED" add fixture.txt
git -C "$SEED" commit -qm 'second fixture commit'
git -C "$SEED" tag -a "$MOVED_TAG" -m 'different commit fixture'
git -C "$SEED" push -q origin "refs/tags/$MOVED_TAG"
if (
  cd "$RUNNER"
  GITHUB_REF_TYPE=tag \
  GITHUB_REF_NAME=$MOVED_TAG \
  GITHUB_REF="refs/tags/$MOVED_TAG" \
  GITHUB_SHA=$COMMIT \
    "$HELPER" >/dev/null 2>&1
); then
  die "remote tag on a different commit was accepted"
fi

echo "restore-canonical-tag-ref tests passed"
