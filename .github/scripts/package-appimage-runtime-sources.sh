#!/usr/bin/env bash
# Build the corresponding-source/relink companion for the pinned AppImage
# type-2 runtime.  This is intentionally separate from Ring Out's GPL source:
# the executable prefix is an upstream static runtime with its own build and
# LGPL relink materials, and the companion must travel beside every release.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="v1.2.1-ell.6"
OUT_DIR="$REPO/dist/out"
TYPE2_SOURCE=""
LIBFUSE_SOURCE=""
SQUASHFUSE_SOURCE=""
FORCE=0

TYPE2_COMMIT=75849dce7cc37e4319b633df1f116ca895c71a12
TYPE2_ARCHIVE_SHA=b7af4960da4b90364e935a3281d04fad6560da4813c012414fa2f738291ad443
TYPE2_RUNTIME_SHA=1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf
LIBFUSE_SHA=70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0
SQUASHFUSE_SHA=db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3
LIBFUSE_PATCH_SHA=1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44
COMPONENTS_SHA=afee9393f5fef576d7e63b837aa6151b2805c9e080af03188d48761bdde0c243
NOTICE_SOURCES_SHA=76a5ac9991ecbc527959f070e0cc158098ed45200bd0aa23949a55ffcad993cf
NOTICE_SUMS_SHA=b2bef013beaab73a901791711a921569d7f9b35fa6b3b339cfa30084a6bcb7f4
ALPINE_BASE='alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d'
UPSTREAM_RUN=28063784345
UPSTREAM_JOB=83083595830

usage() {
  cat <<'EOF'
Usage: package-appimage-runtime-sources.sh [options]

  --version TAG              package/release tag (default v1.2.1-ell.6)
  --out-dir DIR              output directory (default dist/out)
  --type2-source FILE        pinned type2-runtime codeload .tar.gz
  --libfuse-source FILE      pinned fuse-3.15.0.tar.xz
  --squashfuse-source FILE   pinned squashfuse-0.5.2.tar.gz
  --force                    atomically replace an existing named bundle
  -h, --help                 show this help

SOURCE_DATE_EPOCH controls normalized archive timestamps.
EOF
}

die() { printf 'package-appimage-runtime-sources: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }

while (($#)); do
  case "$1" in
    --version)            (($# >= 2)) || die "$1 needs a value"; VERSION=$2; shift 2 ;;
    --out-dir)            (($# >= 2)) || die "$1 needs a value"; OUT_DIR=$2; shift 2 ;;
    --type2-source)       (($# >= 2)) || die "$1 needs a value"; TYPE2_SOURCE=$2; shift 2 ;;
    --libfuse-source)     (($# >= 2)) || die "$1 needs a value"; LIBFUSE_SOURCE=$2; shift 2 ;;
    --squashfuse-source)  (($# >= 2)) || die "$1 needs a value"; SQUASHFUSE_SOURCE=$2; shift 2 ;;
    --force)              FORCE=1; shift ;;
    -h|--help)            usage; exit 0 ;;
    *)                    die "unknown option: $1" ;;
  esac
done

for command in awk cp find grep install mkdir mktemp mv realpath sed sha256sum \
               sort tar touch unzstd wc xargs zstd; do
  need "$command"
done

[[ "$VERSION" =~ ^v?[0-9][0-9A-Za-z._+-]*$ ]] || die "unsafe version/tag: $VERSION"
FILE_VERSION=${VERSION#v}
BUNDLE="RingOut-$FILE_VERSION-appimage-runtime-sources.tar.zst"

for pair in \
  "type2-runtime source:$TYPE2_SOURCE" \
  "libfuse source:$LIBFUSE_SOURCE" \
  "squashfuse source:$SQUASHFUSE_SOURCE"; do
  label=${pair%%:*}
  path=${pair#*:}
  [[ -n "$path" && -f "$path" ]] || die "pass the pinned $label archive"
done
TYPE2_SOURCE="$(realpath "$TYPE2_SOURCE")"
LIBFUSE_SOURCE="$(realpath "$LIBFUSE_SOURCE")"
SQUASHFUSE_SOURCE="$(realpath "$SQUASHFUSE_SOURCE")"
OUT_DIR="$(realpath -m "$OUT_DIR")"

verify_digest() {
  local file=$1 expected=$2 label=$3 actual
  actual=$(sha256sum "$file" | awk '{print $1}')
  [[ "$actual" == "$expected" ]] || \
    die "$label digest mismatch: expected $expected, got $actual"
}
verify_digest "$TYPE2_SOURCE" "$TYPE2_ARCHIVE_SHA" 'type2-runtime codeload archive'
verify_digest "$LIBFUSE_SOURCE" "$LIBFUSE_SHA" 'libfuse 3.15.0 source archive'
verify_digest "$SQUASHFUSE_SOURCE" "$SQUASHFUSE_SHA" 'squashfuse 0.5.2 source archive'

COMPONENTS="$REPO/dist/appimage/APPIMAGE-RUNTIME-COMPONENTS.tsv"
NOTICES="$REPO/dist/appimage/runtime-licenses"
[[ -s "$COMPONENTS" ]] || die "runtime component manifest is missing"
[[ -s "$NOTICES/SHA256SUMS" && -s "$NOTICES/NOTICE-SOURCES.tsv" ]] || \
  die "runtime notice manifests are missing"
verify_digest "$COMPONENTS" "$COMPONENTS_SHA" 'AppImage runtime component manifest'
verify_digest "$NOTICES/NOTICE-SOURCES.tsv" "$NOTICE_SOURCES_SHA" \
  'AppImage runtime immutable notice-source manifest'
verify_digest "$NOTICES/SHA256SUMS" "$NOTICE_SUMS_SHA" \
  'AppImage runtime notice checksum manifest'
while read -r expected notice; do
  verify_digest "$NOTICES/$notice" "$expected" "AppImage runtime notice $notice"
done <<'EOF'
dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551  libfuse-3.15.0-LGPL2.txt
b8832d9caaa075bbbd2aef24efa09f8b7ab66a832812d88c602da0c7b4397fad  libfuse-3.15.0-LICENSE
19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d  mimalloc-2.1.7-LICENSE
f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af  musl-1.2.5-COPYRIGHT
9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13  squashfuse-0.5.2-LICENSE
aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1  type2-runtime-LICENSE
e32ff4e00d9d94930537635291da39e7e612703334bf6fde8c7f1686fe8a45a2  zlib-1.3.2-LICENSE
7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8  zstd-1.5.6-LICENSE
EOF
(
  cd "$NOTICES"
  sha256sum -c SHA256SUMS >/dev/null
) || die "a tracked AppImage runtime notice does not match its exact digest"

# This manifest is a release contract, not an advisory list.  Reject extra,
# duplicate, shortened, or structurally malformed component rows.
[[ $(wc -l <"$COMPONENTS") -eq 8 ]] || \
  die "runtime component manifest must have one header and exactly seven rows"
[[ $(awk -F '\t' 'NR == 1 { next } NF != 6 { bad=1 } { seen[$1]++ } END { for (x in seen) if (seen[x] != 1) bad=1; print length(seen), bad+0 }' "$COMPONENTS") == '7 0' ]] || \
  die "runtime component manifest has malformed or duplicate rows"
for component in type2-runtime musl libfuse squashfuse zstd zlib mimalloc; do
  [[ $(awk -F '\t' -v component="$component" '$1 == component { count++ } END { print count+0 }' "$COMPONENTS") -eq 1 ]] || \
    die "runtime component manifest is missing exact row: $component"
done
grep -Fq $'libfuse\t3.15.0\tLGPL-2.1-only\t' "$COMPONENTS" || \
  die "libfuse must be identified as LGPL-2.1-only"
grep -Fq '/community/mimalloc2' "$COMPONENTS" || \
  die "mimalloc component must point to the exact Alpine mimalloc2 aport"
grep -Fq "$TYPE2_ARCHIVE_SHA" "$COMPONENTS" || \
  die "type2-runtime archive digest is absent from the component manifest"
grep -Fq "$LIBFUSE_SHA" "$COMPONENTS" || \
  die "libfuse source digest is absent from the component manifest"
grep -Fq "$SQUASHFUSE_SHA" "$COMPONENTS" || \
  die "squashfuse source digest is absent from the component manifest"

SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-315532800}
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || die "invalid SOURCE_DATE_EPOCH"
((SOURCE_DATE_EPOCH >= 315532800)) || SOURCE_DATE_EPOCH=315532800
export SOURCE_DATE_EPOCH TZ=UTC

mkdir -p "$OUT_DIR"
FINAL="$OUT_DIR/$BUNDLE"
FINAL_SHA="$FINAL.sha256"
if ((FORCE == 0)) && { [[ -e "$FINAL" ]] || [[ -e "$FINAL_SHA" ]]; }; then
  die "source bundle already exists (use --force): $FINAL"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ringout-appimage-runtime-sources.XXXXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
ROOT="$WORK/RingOut-$FILE_VERSION-appimage-runtime-sources"
SOURCE_ROOT="$ROOT/source/type2-runtime-$TYPE2_COMMIT"
ARCHIVES="$ROOT/source-archives"
RELINK="$ROOT/relink"
mkdir -p "$ROOT/source" "$ARCHIVES" "$RELINK" "$ROOT/LICENSES/AppImage-runtime"

tar -xzf "$TYPE2_SOURCE" -C "$ROOT/source"
EXTRACTED="$ROOT/source/type2-runtime-$TYPE2_COMMIT"
[[ -d "$EXTRACTED" ]] || die "type2-runtime archive did not have its exact commit root"
[[ -s "$EXTRACTED/src/runtime/runtime.c" && \
   -s "$EXTRACTED/scripts/docker/build-with-docker.sh" && \
   -s "$EXTRACTED/scripts/common/install-dependencies.sh" && \
   -s "$EXTRACTED/patches/libfuse/mount.c.diff" ]] || \
  die "type2-runtime source archive is incomplete"
verify_digest "$EXTRACTED/patches/libfuse/mount.c.diff" "$LIBFUSE_PATCH_SHA" \
  'type2-runtime libfuse mount patch'

install -m 644 "$TYPE2_SOURCE" \
  "$ARCHIVES/type2-runtime-$TYPE2_COMMIT.tar.gz"
install -m 644 "$LIBFUSE_SOURCE" "$ARCHIVES/fuse-3.15.0.tar.xz"
install -m 644 "$SQUASHFUSE_SOURCE" "$ARCHIVES/squashfuse-0.5.2.tar.gz"
install -m 644 "$EXTRACTED/patches/libfuse/mount.c.diff" \
  "$RELINK/libfuse-3.15.0-mount.c.diff"
install -m 644 "$COMPONENTS" "$ROOT/APPIMAGE-RUNTIME-COMPONENTS.tsv"
install -m 644 "$NOTICES/NOTICE-SOURCES.tsv" \
  "$ROOT/LICENSES/AppImage-runtime/NOTICE-SOURCES.tsv"
install -m 644 "$NOTICES/SHA256SUMS" \
  "$ROOT/LICENSES/AppImage-runtime/SHA256SUMS"
while IFS= read -r notice; do
  install -m 644 "$NOTICES/$notice" "$ROOT/LICENSES/AppImage-runtime/$notice"
done < <(awk '{print $2}' "$NOTICES/SHA256SUMS")

cat >"$ROOT/MODIFICATIONS.txt" <<EOF
AppImage runtime third-party source modifications
=================================================

libfuse 3.15.0 file changed: lib/mount.c
Applied patch: relink/libfuse-3.15.0-mount.c.diff
Patch SHA-256: $LIBFUSE_PATCH_SHA

The patch was introduced in AppImage/type2-runtime by:
  commit f5636118e0d8645e7f572771b5ac98b6da931f95
  author TheAssassin
  author date 2024-11-24T17:28:34+01:00
  commit date 2024-11-24T19:09:04+01:00
  subject Make FUSE require \$FUSERMOUNT_PROG in env to be set
  https://github.com/AppImage/type2-runtime/commit/f5636118e0d8645e7f572771b5ac98b6da931f95

The modification makes libfuse require the FUSERMOUNT_PROG environment
variable rather than locating the fusermount executable itself. The exact
patch is also present in the untouched type2-runtime source tree at
patches/libfuse/mount.c.diff and is applied by its dependency build script.
EOF

cat >"$ROOT/README.txt" <<EOF
Ring Out AppImage type-2 runtime corresponding source and relink materials
=========================================================================

Release companion: RingOut-$FILE_VERSION-linux-x86_64.AppImage
Embedded runtime SHA-256: $TYPE2_RUNTIME_SHA

Keep this checksummed bundle available beside the AppImage. It is the
designated same-release source/relink download for the static type-2 runtime
prefix, including the libfuse LGPL-2.1-only component. It is separate from the
Ring Out repository source named by SOURCE.txt inside the AppImage.

Exact upstream provenance
-------------------------

type2-runtime commit: $TYPE2_COMMIT
type2-runtime archive: source-archives/type2-runtime-$TYPE2_COMMIT.tar.gz
type2-runtime archive SHA-256: $TYPE2_ARCHIVE_SHA
Upstream GitHub Actions run/job: $UPSTREAM_RUN / $UPSTREAM_JOB
Upstream build base: $ALPINE_BASE

Resolved static components in that upstream x86_64 build:
  musl 1.2.5-r11
  libfuse 3.15.0 plus patches/libfuse/mount.c.diff
  squashfuse 0.5.2
  zstd 1.5.6-r2
  zlib 1.3.2-r0
  mimalloc2 2.1.7-r0

APPIMAGE-RUNTIME-COMPONENTS.tsv records immutable source locations, revisions
or source hashes, licenses, and notice files for all seven runtime components.
LICENSES/AppImage-runtime contains the exact full notices and their hashes.

Included source/relink inputs
-----------------------------

  source/type2-runtime-$TYPE2_COMMIT/
    Exact expanded type2-runtime source and build scripts.
  source-archives/fuse-3.15.0.tar.xz
    SHA-256 $LIBFUSE_SHA
  source-archives/squashfuse-0.5.2.tar.gz
    SHA-256 $SQUASHFUSE_SHA
  relink/libfuse-3.15.0-mount.c.diff
    SHA-256 $LIBFUSE_PATCH_SHA
  MODIFICATIONS.txt
    Identifies the modified libfuse file and the exact patch commit, date, and
    purpose required to distinguish it from the unmodified 3.15.0 source.

The original type2 source archive is retained under source-archives as an
independent exact-input check. SHA256SUMS covers every other regular file here.

Rebuild/relink x86_64
---------------------

From the extracted root, with Docker available, run:

  ./relink/rebuild-x86_64.sh

The helper copies the exact source tree to a fresh work directory, pins the
recorded Alpine base digest, substitutes the included libfuse and squashfuse
archives for the upstream downloads, and invokes the upstream build command:

  ARCH=x86_64 scripts/docker/build-with-docker.sh

The untouched codeload tree says UNSUPPORTED_LOCAL_DEVELOPER_BUILD in
src/runtime/version because GitHub codeload does not run the release workflow's
version injection. Before compiling, the helper writes the exact released
input used by upstream run $UPSTREAM_RUN:

  https://github.com/AppImage/type2-runtime/commit/75849dc

The resulting files are written below the helper's work directory. Alpine APK
repositories are still needed for the compiler and the recorded static musl,
zstd, zlib, and mimalloc2 packages. Their exact resolved versions and immutable
aport revisions are recorded above and in APPIMAGE-RUNTIME-COMPONENTS.tsv.
Future repository updates can change binary reproducibility; the source and
relink materials in this archive remain exact for the released runtime.
EOF

cat >"$RELINK/rebuild-x86_64.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TYPE2_COMMIT=75849dce7cc37e4319b633df1f116ca895c71a12
ALPINE_BASE='alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d'
RUNTIME_VERSION='https://github.com/AppImage/type2-runtime/commit/75849dc'
WORK=${1:-"$PWD/type2-runtime-x86_64-rebuild"}

[[ ! -e "$WORK" ]] || {
  printf 'rebuild directory already exists: %s\n' "$WORK" >&2
  exit 1
}
mkdir -p "$WORK"
cp -a "$HERE/source/type2-runtime-$TYPE2_COMMIT/." "$WORK/"
printf '%s\n' "$RUNTIME_VERSION" >"$WORK/src/runtime/version"
grep -Fxq "$RUNTIME_VERSION" "$WORK/src/runtime/version"
mkdir -p "$WORK/vendor-sources"
install -m 644 "$HERE/source-archives/fuse-3.15.0.tar.xz" \
  "$WORK/vendor-sources/fuse-3.15.0.tar.xz"
install -m 644 "$HERE/source-archives/squashfuse-0.5.2.tar.gz" \
  "$WORK/vendor-sources/squashfuse-0.5.2.tar.gz"

echo '70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0  '"$WORK/vendor-sources/fuse-3.15.0.tar.xz" | sha256sum -c -
echo 'db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3  '"$WORK/vendor-sources/squashfuse-0.5.2.tar.gz" | sha256sum -c -
echo '1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44  '"$WORK/patches/libfuse/mount.c.diff" | sha256sum -c -

# Pin the base image used by the recorded upstream build and arrange for the
# upstream dependency installer to consume this bundle's exact source tars.
sed -i "1cFROM $ALPINE_BASE" "$WORK/scripts/docker/Dockerfile"
sed -i '/^COPY patches\/ /a COPY vendor-sources/ /tmp/vendor-sources/' \
  "$WORK/scripts/docker/Dockerfile"
sed -i \
  -e 's|^wget https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz$|cp /tmp/vendor-sources/fuse-3.15.0.tar.xz ./|' \
  -e 's|^wget "https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz"$|cp /tmp/vendor-sources/squashfuse-0.5.2.tar.gz ./0.5.2.tar.gz|' \
  "$WORK/scripts/common/install-dependencies.sh"
grep -Fq 'cp /tmp/vendor-sources/fuse-3.15.0.tar.xz ./' \
  "$WORK/scripts/common/install-dependencies.sh"
grep -Fq 'cp /tmp/vendor-sources/squashfuse-0.5.2.tar.gz ./0.5.2.tar.gz' \
  "$WORK/scripts/common/install-dependencies.sh"

cd "$WORK"
ARCH=x86_64 bash scripts/docker/build-with-docker.sh
printf '\nBuilt runtime: %s/out/runtime-x86_64\n' "$WORK"
sha256sum "$WORK/out/runtime-x86_64"
printf 'Released runtime reference SHA-256: %s\n' \
  1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf
EOF
chmod 755 "$RELINK/rebuild-x86_64.sh"

(
  cd "$ROOT"
  find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | \
    xargs -0 sha256sum >SHA256SUMS
  sha256sum -c SHA256SUMS >/dev/null
)

# Normalize source-tree and generated metadata times before a deterministic,
# single-threaded compression pass.  Single-threaded zstd avoids host-CPU-count
# differences changing frame layout for this small legal-source bundle.
find "$ROOT" -depth -print0 | xargs -0 touch -h -d "@$SOURCE_DATE_EPOCH"
TMP_BUNDLE="$WORK/$BUNDLE"
tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner \
  --mtime="@$SOURCE_DATE_EPOCH" -C "$WORK" -cf - "$(basename "$ROOT")" | \
  zstd -q -19 --threads=1 -c >"$TMP_BUNDLE"

VERIFY="$WORK/verify"
mkdir -p "$VERIFY"
tar --use-compress-program=unzstd -xf "$TMP_BUNDLE" -C "$VERIFY"
VERIFY_ROOT="$VERIFY/$(basename "$ROOT")"
[[ -x "$VERIFY_ROOT/relink/rebuild-x86_64.sh" ]] || \
  die "rebuild helper lost its executable mode"
(
  cd "$VERIFY_ROOT"
  sha256sum -c SHA256SUMS >/dev/null
  cd LICENSES/AppImage-runtime
  sha256sum -c SHA256SUMS >/dev/null
)
grep -Fq "$TYPE2_RUNTIME_SHA" "$VERIFY_ROOT/README.txt" || \
  die "runtime digest is absent from source-bundle README"
grep -Fq "$ALPINE_BASE" "$VERIFY_ROOT/README.txt" || \
  die "exact upstream Alpine base is absent from source-bundle README"
grep -Fq "$UPSTREAM_RUN / $UPSTREAM_JOB" "$VERIFY_ROOT/README.txt" || \
  die "upstream build run/job is absent from source-bundle README"
grep -Fq 'commit f5636118e0d8645e7f572771b5ac98b6da931f95' \
  "$VERIFY_ROOT/MODIFICATIONS.txt" || \
  die "libfuse modification notice lacks its exact type2-runtime commit"
grep -Fq 'commit date 2024-11-24T19:09:04+01:00' \
  "$VERIFY_ROOT/MODIFICATIONS.txt" || \
  die "libfuse modification notice lacks its exact commit date"
grep -Fq "Patch SHA-256: $LIBFUSE_PATCH_SHA" \
  "$VERIFY_ROOT/MODIFICATIONS.txt" || \
  die "libfuse modification notice lacks its exact patch digest"
grep -Fq "RUNTIME_VERSION='https://github.com/AppImage/type2-runtime/commit/75849dc'" \
  "$VERIFY_ROOT/relink/rebuild-x86_64.sh" || \
  die "rebuild helper does not recreate the exact embedded runtime version"

TMP_SHA="$WORK/$BUNDLE.sha256"
bundle_hash=$(sha256sum "$TMP_BUNDLE" | awk '{print $1}')
printf '%s  %s\n' "$bundle_hash" "$BUNDLE" >"$TMP_SHA"
mv -f -- "$TMP_BUNDLE" "$FINAL"
mv -f -- "$TMP_SHA" "$FINAL_SHA"

printf 'OK: %s\n' "$FINAL"
printf 'SHA-256: %s\n' "$bundle_hash"
