#!/usr/bin/env bash
# Assemble and validate a Linux x86-64 AppImage from Debian-12-built binaries.
# The AppDir is an allowlist: no extracted disc, save, generated module, build
# tree, or developer userdata is copied from a working package.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="v1.2.1-ell.7"
BUILD_DIR="$REPO/build-appimage"
DOLRECOMP_BUILD_DIR="$REPO/build-dolrecomp-appimage"
OUT_DIR="$REPO/dist/out"
RUNTIME=""
LAUNCHER=""
PORT=""
DOLRECOMP=""
SYS_DIR=""
APPIMAGETOOL=""
APPIMAGE_RUNTIME=""
TYPE2_SOURCE=""
LIBFUSE_SOURCE=""
SQUASHFUSE_SOURCE=""
FORCE=0

usage() {
  cat <<'EOF'
Usage: package-appimage.sh [options]

  --version TAG             package/release tag (default v1.2.1-ell.7)
  --build-dir DIR           ModernGekko build tree (default build-appimage)
  --dolrecomp-build-dir DIR DolRecomp build tree
  --out-dir DIR             output directory (default dist/out)
  --runtime FILE            explicit moderngekko-run executable
  --launcher FILE           explicit C++ RingOut launcher
  --port FILE               explicit moderngekko-port helper
  --dolrecomp FILE          explicit static dolrecomp executable
  --sys-dir DIR             explicit Dolphin Sys resource directory
  --appimagetool FILE       pinned appimagetool 1.9.1 executable
  --appimage-runtime FILE   pinned type-2 runtime-x86_64 file
  --type2-source FILE       pinned type2-runtime source .tar.gz
  --libfuse-source FILE     pinned fuse-3.15.0.tar.xz
  --squashfuse-source FILE  pinned squashfuse-0.5.2.tar.gz
  --force                   atomically replace an existing named artifact
  -h, --help                show this help

Environment used in SOURCE.txt:
  SOURCE_REPOSITORY, SOURCE_COMMIT, UPSTREAM_REPOSITORY, SOURCE_DATE_EPOCH
  PACKAGE_VALIDATION_ONLY=1  allow a dirty/non-tagged local validation package
EOF
}

die() { printf 'package-appimage: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }

while (($#)); do
  case "$1" in
    --version)             (($# >= 2)) || die "$1 needs a value"; VERSION=$2; shift 2 ;;
    --build-dir)           (($# >= 2)) || die "$1 needs a value"; BUILD_DIR=$2; shift 2 ;;
    --dolrecomp-build-dir) (($# >= 2)) || die "$1 needs a value"; DOLRECOMP_BUILD_DIR=$2; shift 2 ;;
    --out-dir)             (($# >= 2)) || die "$1 needs a value"; OUT_DIR=$2; shift 2 ;;
    --runtime)             (($# >= 2)) || die "$1 needs a value"; RUNTIME=$2; shift 2 ;;
    --launcher)            (($# >= 2)) || die "$1 needs a value"; LAUNCHER=$2; shift 2 ;;
    --port)                (($# >= 2)) || die "$1 needs a value"; PORT=$2; shift 2 ;;
    --dolrecomp)           (($# >= 2)) || die "$1 needs a value"; DOLRECOMP=$2; shift 2 ;;
    --sys-dir)             (($# >= 2)) || die "$1 needs a value"; SYS_DIR=$2; shift 2 ;;
    --appimagetool)        (($# >= 2)) || die "$1 needs a value"; APPIMAGETOOL=$2; shift 2 ;;
    --appimage-runtime)    (($# >= 2)) || die "$1 needs a value"; APPIMAGE_RUNTIME=$2; shift 2 ;;
    --type2-source)        (($# >= 2)) || die "$1 needs a value"; TYPE2_SOURCE=$2; shift 2 ;;
    --libfuse-source)      (($# >= 2)) || die "$1 needs a value"; LIBFUSE_SOURCE=$2; shift 2 ;;
    --squashfuse-source)   (($# >= 2)) || die "$1 needs a value"; SQUASHFUSE_SOURCE=$2; shift 2 ;;
    --force)               FORCE=1; shift ;;
    -h|--help)             usage; exit 0 ;;
    *)                     die "unknown option: $1" ;;
  esac
done

[[ "$VERSION" =~ ^v?[0-9][0-9A-Za-z._+-]*$ ]] || die "unsafe version/tag: $VERSION"
TAG_VERSION=$VERSION
[[ "$TAG_VERSION" == v* ]] || TAG_VERSION="v$TAG_VERSION"
FILE_VERSION=${TAG_VERSION#v}
ARTIFACT="RingOut-$FILE_VERSION-linux-x86_64.AppImage"
SOURCE_BUNDLE="RingOut-$FILE_VERSION-appimage-runtime-sources.tar.zst"

for cmd in awk cmake cmp cp cut dpkg-query file find gcc grep head install ld ldd ln \
           mkdir mktemp mv ninja objdump python3 readelf realpath sed sha256sum sort \
           stat strings tar touch tr wc xargs zstd; do
  need "$cmd"
done

EXPECTED_BUILD_BASE='debian:12@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443'
[[ "${RINGOUT_APPIMAGE_BUILD_BASE:-}" == "$EXPECTED_BUILD_BASE" ]] || \
  die "run this packager inside the pinned AppImage Debian build image"

BUILD_DIR="$(realpath -m "$BUILD_DIR")"
DOLRECOMP_BUILD_DIR="$(realpath -m "$DOLRECOMP_BUILD_DIR")"
OUT_DIR="$(realpath -m "$OUT_DIR")"

resolve_file() {
  local explicit=$1 label=$2
  shift 2
  local candidate
  if [[ -n "$explicit" ]]; then
    [[ -f "$explicit" ]] || die "$label not found: $explicit"
    realpath "$explicit"
    return
  fi
  for candidate in "$@"; do
    if [[ -f "$candidate" ]]; then
      realpath "$candidate"
      return
    fi
  done
  die "could not locate $label; pass its explicit option"
}

RUNTIME="$(resolve_file "$RUNTIME" moderngekko-run \
  "$BUILD_DIR/moderngekko-run" "$BUILD_DIR/Binaries/moderngekko-run")"
LAUNCHER="$(resolve_file "$LAUNCHER" RingOut \
  "$BUILD_DIR/RingOut" "$BUILD_DIR/Binaries/RingOut")"
PORT="$(resolve_file "$PORT" moderngekko-port \
  "$BUILD_DIR/moderngekko-port" "$BUILD_DIR/Binaries/moderngekko-port")"
DOLRECOMP="$(resolve_file "$DOLRECOMP" dolrecomp \
  "$DOLRECOMP_BUILD_DIR/dolrecomp" "$DOLRECOMP_BUILD_DIR/Binaries/dolrecomp")"
MODULE_INFO="$(resolve_file '' moderngekko-module-info \
  "$BUILD_DIR/moderngekko-module-info" \
  "$BUILD_DIR/Binaries/moderngekko-module-info")"
[[ -n "$APPIMAGETOOL" && -f "$APPIMAGETOOL" ]] || \
  die "pass the pinned appimagetool 1.9.1 with --appimagetool"
[[ -n "$APPIMAGE_RUNTIME" && -f "$APPIMAGE_RUNTIME" ]] || \
  die "pass the pinned type-2 runtime with --appimage-runtime"
[[ -n "$TYPE2_SOURCE" && -f "$TYPE2_SOURCE" ]] || \
  die "pass the pinned type2-runtime source with --type2-source"
[[ -n "$LIBFUSE_SOURCE" && -f "$LIBFUSE_SOURCE" ]] || \
  die "pass the pinned libfuse source with --libfuse-source"
[[ -n "$SQUASHFUSE_SOURCE" && -f "$SQUASHFUSE_SOURCE" ]] || \
  die "pass the pinned squashfuse source with --squashfuse-source"
APPIMAGETOOL="$(realpath "$APPIMAGETOOL")"
APPIMAGE_RUNTIME="$(realpath "$APPIMAGE_RUNTIME")"
TYPE2_SOURCE="$(realpath "$TYPE2_SOURCE")"
LIBFUSE_SOURCE="$(realpath "$LIBFUSE_SOURCE")"
SQUASHFUSE_SOURCE="$(realpath "$SQUASHFUSE_SOURCE")"
[[ "$(sha256sum "$APPIMAGETOOL" | awk '{print $1}')" == \
   ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 ]] || \
  die "appimagetool does not match pinned 1.9.1 x86-64 digest"
[[ "$(sha256sum "$APPIMAGE_RUNTIME" | awk '{print $1}')" == \
   1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf ]] || \
  die "type-2 runtime does not match pinned x86-64 asset digest"

if [[ -n "$SYS_DIR" ]]; then
  [[ -d "$SYS_DIR" ]] || die "Sys directory not found: $SYS_DIR"
  SYS_DIR="$(realpath "$SYS_DIR")"
else
  for candidate in "$(dirname "$RUNTIME")/Sys" "$BUILD_DIR/Sys"; do
    if [[ -d "$candidate" ]]; then SYS_DIR="$(realpath "$candidate")"; break; fi
  done
fi
[[ -n "$SYS_DIR" && -d "$SYS_DIR" ]] || die "Dolphin Sys resources were not found"
for required in GC/dsp_rom.bin GC/font_western.bin ApprovedInis.json; do
  [[ -s "$SYS_DIR/$required" ]] || die "incomplete Sys tree: missing $required"
done
sys_files="$(find "$SYS_DIR" -type f | wc -l)"
((sys_files >= 100)) || die "incomplete Sys tree: only $sys_files files"

[[ "$(head -c 4 "$RUNTIME")" == $'\x7fELF' ]] || die "runtime is not ELF"
[[ "$(head -c 4 "$LAUNCHER")" == $'\x7fELF' ]] || die "launcher is not ELF"
[[ "$(head -c 4 "$PORT")" == $'\x7fELF' ]] || die "port helper is not ELF"
[[ "$(head -c 4 "$DOLRECOMP")" == $'\x7fELF' ]] || die "dolrecomp is not ELF"
file "$RUNTIME" | grep -q 'x86-64' || die "runtime is not x86-64"
file "$LAUNCHER" | grep -q 'x86-64' || die "launcher is not x86-64"
file "$PORT" | grep -q 'x86-64' || die "setup helper is not x86-64"
file "$DOLRECOMP" | grep -q 'x86-64' || die "dolrecomp is not x86-64"
if readelf -l "$DOLRECOMP" | grep -q 'INTERP'; then
  die "dolrecomp must be statically linked so first-run extraction is portable"
fi
for executable in "$RUNTIME" "$LAUNCHER" "$PORT"; do
  if ldd "$executable" 2>&1 | grep -q 'not found'; then
    ldd "$executable" >&2 || true
    die "$(basename "$executable") has unresolved shared-library dependencies"
  fi
done

glibc_floor="$({ objdump -T "$RUNTIME"; objdump -T "$LAUNCHER"; objdump -T "$PORT"; } \
  2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
[[ -n "$glibc_floor" ]] || die "could not determine the runtime glibc floor"
glibc_version=${glibc_floor#GLIBC_}
if [[ "$glibc_version" != "2.36" ]] &&
   [[ "$(printf '%s\n2.36\n' "$glibc_version" | sort -V | tail -1)" == "$glibc_version" ]]; then
  die "runtime glibc floor $glibc_floor is above the Debian 12 portability target"
fi

SOURCE_COMMIT="${SOURCE_COMMIT:-$(git -C "$REPO" rev-parse HEAD)}"
[[ "$SOURCE_COMMIT" =~ ^[0-9a-fA-F]{40}$ ]] || die "invalid SOURCE_COMMIT: $SOURCE_COMMIT"
PACKAGE_VALIDATION_ONLY="${PACKAGE_VALIDATION_ONLY:-0}"
[[ "$PACKAGE_VALIDATION_ONLY" == 0 || "$PACKAGE_VALIDATION_ONLY" == 1 ]] || \
  die "PACKAGE_VALIDATION_ONLY must be 0 or 1"
if [[ "$PACKAGE_VALIDATION_ONLY" == 0 ]]; then
  [[ -z "$(git -C "$REPO" status --porcelain=v1 --untracked-files=normal)" ]] || \
    die "release packaging requires a clean source tree (use PACKAGE_VALIDATION_ONLY=1 only for local QA)"
  tag_commit="$(git -C "$REPO" rev-parse --verify "$TAG_VERSION^{commit}" 2>/dev/null || true)"
  [[ -n "$tag_commit" ]] || die "release tag does not exist in this checkout: $TAG_VERSION"
  [[ "$tag_commit" == "$SOURCE_COMMIT" ]] || \
    die "release tag $TAG_VERSION does not resolve to SOURCE_COMMIT $SOURCE_COMMIT"
fi
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO" show -s --format=%ct "$SOURCE_COMMIT")}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || die "invalid SOURCE_DATE_EPOCH"
((SOURCE_DATE_EPOCH >= 315532800)) || SOURCE_DATE_EPOCH=315532800
export SOURCE_DATE_EPOCH TZ=UTC

mkdir -p "$OUT_DIR"
FINAL_IMAGE="$OUT_DIR/$ARTIFACT"
FINAL_SHA="$FINAL_IMAGE.sha256"
FINAL_SOURCE_BUNDLE="$OUT_DIR/$SOURCE_BUNDLE"
FINAL_SOURCE_SHA="$FINAL_SOURCE_BUNDLE.sha256"
if ((FORCE == 0)) && { [[ -e "$FINAL_IMAGE" ]] || [[ -e "$FINAL_SHA" ]] ||
                       [[ -e "$FINAL_SOURCE_BUNDLE" ]] || [[ -e "$FINAL_SOURCE_SHA" ]]; }; then
  die "an artifact already exists (use --force): $ARTIFACT or $SOURCE_BUNDLE"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ringout-appimage.XXXXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
APPDIR="$WORK/RingOut.AppDir"
PAYLOAD="$APPDIR/usr/share/ringout"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" "$PAYLOAD/LICENSES"

printf '==> exact AppImage runtime source/relink companion\n'
SOURCE_BUNDLE_OUT="$WORK/runtime-source-bundle"
mkdir -p "$SOURCE_BUNDLE_OUT"
"$REPO/.github/scripts/package-appimage-runtime-sources.sh" \
  --version "$TAG_VERSION" \
  --out-dir "$SOURCE_BUNDLE_OUT" \
  --type2-source "$TYPE2_SOURCE" \
  --libfuse-source "$LIBFUSE_SOURCE" \
  --squashfuse-source "$SQUASHFUSE_SOURCE"
TMP_SOURCE_BUNDLE="$SOURCE_BUNDLE_OUT/$SOURCE_BUNDLE"
TMP_SOURCE_SHA="$TMP_SOURCE_BUNDLE.sha256"
[[ -s "$TMP_SOURCE_BUNDLE" && -s "$TMP_SOURCE_SHA" ]] || \
  die "runtime corresponding-source bundle was not created"
source_bundle_hash=$(sha256sum "$TMP_SOURCE_BUNDLE" | awk '{print $1}')
[[ $(<"$TMP_SOURCE_SHA") == "$source_bundle_hash  $SOURCE_BUNDLE" ]] || \
  die "runtime corresponding-source checksum sidecar is malformed"

copy_tree() {
  local source=$1 destination=$2
  mkdir -p "$destination"
  cp -a "$source"/. "$destination"/
}

printf '==> AppDir entry point and metadata\n'
install -m 755 "$REPO/dist/appimage/AppRun" "$APPDIR/AppRun"
install -m 644 "$REPO/dist/appimage/ringout.desktop" "$APPDIR/ringout.desktop"
printf 'X-AppImage-Version=%s\n' "$FILE_VERSION" >>"$APPDIR/ringout.desktop"
install -m 644 "$APPDIR/ringout.desktop" \
  "$APPDIR/usr/share/applications/ringout.desktop"
install -m 644 "$REPO/dist/appimage/ringout.svg" "$APPDIR/ringout.svg"
install -m 644 "$REPO/dist/appimage/ringout.svg" "$APPDIR/.DirIcon"
install -m 644 "$REPO/dist/appimage/ringout.svg" \
  "$APPDIR/usr/share/icons/hicolor/scalable/apps/ringout.svg"

printf '==> runtime, recompiler, and Sys resources\n'
install -m 755 "$LAUNCHER" "$APPDIR/usr/bin/RingOut"
install -m 755 "$RUNTIME" "$APPDIR/usr/bin/moderngekko-run"
install -m 755 "$PORT" "$APPDIR/usr/bin/moderngekko-port"
install -m 755 "$DOLRECOMP" "$APPDIR/usr/bin/dolrecomp"
strings -a "$APPDIR/usr/bin/RingOut" | grep -Fq \
  'RingOut C++ launcher self-test' || \
  die "AppImage top-level entry is not the integrated C++ launcher"
[[ -s "$(dirname "$LAUNCHER")/fonts/DroidSans.ttf" ]] || \
  die "C++ launcher font payload is missing beside $LAUNCHER"
mkdir -p "$APPDIR/usr/bin/fonts"
install -m 644 "$(dirname "$LAUNCHER")/fonts/DroidSans.ttf" \
  "$APPDIR/usr/bin/fonts/DroidSans.ttf"
install -m 644 "$(dirname "$LAUNCHER")/fonts/Roboto-Medium.ttf" \
  "$APPDIR/usr/bin/fonts/Roboto-Medium.ttf"
copy_tree "$SYS_DIR" "$APPDIR/usr/bin/Sys"

printf '==> selective support-library payload\n'
declare -A copied_licenses=()
LIBRARY_MANIFEST="$PAYLOAD/SYSTEM-LIBRARIES.tsv"
printf 'soname\tsha256\tbinary_package\tbinary_version\tsource_package\tsource_version\tsource_url\n' \
  >"$LIBRARY_MANIFEST"
while IFS=$'\t' read -r soname path; do
  [[ -n "$soname" && -n "$path" ]] || continue
  case "$soname" in
    linux-vdso.so.*|ld-linux-*.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|\
    librt.so.*|libresolv.so.*|libanl.so.*|libutil.so.*|libBrokenLocale.so.*|\
    libnss_*.so.*|libthread_db.so.*|libnsl.so.1)
      continue
      ;;
  esac
  install -m 755 "$path" "$APPDIR/usr/lib/$soname"

  real_path="$(realpath "$path")"
  # Debian 12 usr-merge canonicalises /lib to /usr/lib, while dpkg records a
  # mixture of both forms (and may own either the SONAME link or its target).
  # Try the exact loader/canonical paths plus both sides of the usr-merge alias.
  owner_line=""
  owner_candidates=("$path" "$real_path")
  for candidate in "$path" "$real_path"; do
    case "$candidate" in
      /lib/*|/bin/*|/sbin/*) owner_candidates+=("/usr$candidate") ;;
      /usr/lib/*|/usr/bin/*|/usr/sbin/*) owner_candidates+=("${candidate#/usr}") ;;
    esac
  done
  for candidate in "${owner_candidates[@]}"; do
    owner_line="$(dpkg-query -S "$candidate" 2>/dev/null | head -1 || true)"
    [[ -n "$owner_line" ]] && break
  done
  package="${owner_line%%: /*}"
  [[ -n "$owner_line" && "$package" != "$owner_line" ]] || \
    die "bundled DSO has no Debian package owner: $soname ($real_path)"
  metadata="$(dpkg-query -W \
    -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' \
    "$package" 2>/dev/null || true)"
  IFS=$'\t' read -r binary_package binary_version source_package source_version \
    <<<"$metadata"
  [[ -n "$binary_package" && -n "$binary_version" ]] || \
    die "missing Debian binary-package metadata for $soname ($package)"
  source_package="${source_package:-${binary_package%%:*}}"
  source_version="${source_version:-$binary_version}"

  doc_package="${binary_package%%:*}"
  copyright="/usr/share/doc/$doc_package/copyright"
  [[ -s "$copyright" ]] || \
    die "bundled DSO package has no copyright file: $soname ($binary_package)"
  if [[ -z "${copied_licenses[$binary_package]:-}" ]]; then
    safe_package="${binary_package//[^A-Za-z0-9_.+-]/_}"
    install -m 644 "$copyright" \
      "$PAYLOAD/LICENSES/Debian-$safe_package.copyright"
    copied_licenses[$binary_package]=1
  fi

  library_hash="$(sha256sum "$path" | awk '{print $1}')"
  url_version="${source_version//%/%25}"
  url_version="${url_version//:/%3A}"
  url_version="${url_version//+/%2B}"
  url_version="${url_version//\~/%7E}"
  source_url="https://sources.debian.org/src/$source_package/$url_version/"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$soname" "$library_hash" "$binary_package" "$binary_version" \
    "$source_package" "$source_version" "$source_url" >>"$LIBRARY_MANIFEST"
done < <(
  { ldd "$RUNTIME"; ldd "$LAUNCHER"; ldd "$PORT"; } |
    awk '$2 == "=>" && $3 ~ /^\// { print $1 "\t" $3 }' |
    LC_ALL=C sort -u
)
printf '    %s libraries; %s Debian copyright files\n' \
  "$(find "$APPDIR/usr/lib" -maxdepth 1 -type f | wc -l)" \
  "${#copied_licenses[@]}"

# DolRecomp is deliberately static so it can survive the first-run terminal
# outliving the AppImage mount and run on distributions without our build-time
# DSOs. ModernGekko also links libstdc++ and libgcc statically. Inventory these
# embedded components separately from runtime DSOs.
TOOLCHAIN_MANIFEST="$PAYLOAD/TOOLCHAIN-COMPONENTS.tsv"
printf 'consumer\tcomponent\tbinary_package\tbinary_version\tsource_package\tsource_version\tsource_url\n' \
  >"$TOOLCHAIN_MANIFEST"
gcc_major="$(gcc -dumpversion | cut -d. -f1)"
[[ "$gcc_major" =~ ^[0-9]+$ ]] || die "could not determine GCC major version"
while IFS=$'\t' read -r consumer component package; do
  metadata="$(dpkg-query -W \
    -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' \
    "$package" 2>/dev/null || true)"
  IFS=$'\t' read -r binary_package binary_version source_package source_version \
    <<<"$metadata"
  [[ -n "$binary_package" && -n "$binary_version" ]] || \
    die "missing Debian metadata for static executable component: $package"
  source_package="${source_package:-${binary_package%%:*}}"
  source_version="${source_version:-$binary_version}"
  doc_package="${binary_package%%:*}"
  copyright="/usr/share/doc/$doc_package/copyright"
  [[ -s "$copyright" ]] || \
    die "static executable component has no copyright file: $binary_package"
  safe_package="${binary_package//[^A-Za-z0-9_.+-]/_}"
  install -m 644 "$copyright" \
    "$PAYLOAD/LICENSES/Debian-static-$safe_package.copyright"

  url_version="${source_version//%/%25}"
  url_version="${url_version//:/%3A}"
  url_version="${url_version//+/%2B}"
  url_version="${url_version//\~/%7E}"
  source_url="https://sources.debian.org/src/$source_package/$url_version/"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$consumer" "$component" "$binary_package" "$binary_version" \
    "$source_package" "$source_version" "$source_url" >>"$TOOLCHAIN_MANIFEST"
done <<EOF
dolrecomp	glibc	libc6-dev
dolrecomp	gcc-runtime	libgcc-$gcc_major-dev
dolrecomp	zlib	zlib1g-dev
moderngekko-run	gcc-runtime	libgcc-$gcc_major-dev
moderngekko-run	libstdc++	libstdc++-$gcc_major-dev
EOF

printf '==> writable-install scaffolding\n'
install -m 755 "$REPO/dist/RingOut-1.0-dist/RingOut" "$PAYLOAD/RingOut.payload"
install -m 755 "$REPO/dist/RingOut-1.0-dist/setup.sh" "$PAYLOAD/setup.sh"
install -m 755 "$REPO/dist/shared/gc-art.py" "$PAYLOAD/gc-art.py"
install -m 644 "$REPO/dist/appimage/README.txt" "$PAYLOAD/README.txt"
install -m 644 "$REPO/dist/appimage/CREDITS.txt" "$PAYLOAD/CREDITS.txt"
copy_tree "$REPO/dist/RingOut-1.0-dist/module-src" "$PAYLOAD/module-src"
rm -f -- "$PAYLOAD/module-src/module.profdata"
copy_tree "$REPO/dist/RingOut-1.0-dist/shaders" "$PAYLOAD/shaders"

GAMESETTINGS="$REPO/work/mg_userdir/GameSettings/GRSEAF.ini"
[[ -s "$GAMESETTINGS" ]] || die "tracked GRSEAF.ini not found: $GAMESETTINGS"
awk '
  /^\[(ActionReplay|Gecko)_Enabled\]$/ { enabled=1; print; next }
  /^\[/ { enabled=0 }
  enabled && /^\$/ { next }
  { print }
' "$GAMESETTINGS" >"$PAYLOAD/GRSEAF.ini"

printf '==> source, licences, and provenance\n'
install -m 644 "$REPO/LICENSE" "$PAYLOAD/LICENSES/RingOut-GPL-2.0-or-later.txt"
install -m 644 "$REPO/ModernGekko/LICENSE" \
  "$PAYLOAD/LICENSES/ModernGekko-GPL-3.0-or-later.txt"
install -m 644 "$REPO/DolRecomp/LICENSE" \
  "$PAYLOAD/LICENSES/DolRecomp-GPL-3.0-or-later.txt"
install -m 644 "$REPO/ModernGekko/PROVENANCE.md" \
  "$PAYLOAD/LICENSES/ModernGekko-PROVENANCE.md"
install -m 644 "$REPO/ModernGekko/vendor/dolphin/COPYING" \
  "$PAYLOAD/LICENSES/Dolphin-COPYING.txt"
copy_tree "$REPO/ModernGekko/vendor/dolphin/LICENSES" "$PAYLOAD/LICENSES/Dolphin"

# The type-2 prefix is itself a static executable. Keep its component/source
# inventory and exact full notices distinct from Debian DSOs and from the
# compiler components statically incorporated into Ring Out executables.
APPIMAGE_RUNTIME_COMPONENTS="$PAYLOAD/APPIMAGE-RUNTIME-COMPONENTS.tsv"
APPIMAGE_RUNTIME_LICENSES="$PAYLOAD/LICENSES/AppImage-runtime"
install -m 644 "$REPO/dist/appimage/APPIMAGE-RUNTIME-COMPONENTS.tsv" \
  "$APPIMAGE_RUNTIME_COMPONENTS"
copy_tree "$REPO/dist/appimage/runtime-licenses" "$APPIMAGE_RUNTIME_LICENSES"
[[ $(sha256sum "$APPIMAGE_RUNTIME_COMPONENTS" | awk '{print $1}') == \
   afee9393f5fef576d7e63b837aa6151b2805c9e080af03188d48761bdde0c243 ]] || \
  die "AppImage runtime component manifest differs from the audited seven-row contract"
[[ $(sha256sum "$APPIMAGE_RUNTIME_LICENSES/NOTICE-SOURCES.tsv" | awk '{print $1}') == \
   76a5ac9991ecbc527959f070e0cc158098ed45200bd0aa23949a55ffcad993cf ]] || \
  die "AppImage runtime immutable notice-source manifest changed"
[[ $(sha256sum "$APPIMAGE_RUNTIME_LICENSES/SHA256SUMS" | awk '{print $1}') == \
   b2bef013beaab73a901791711a921569d7f9b35fa6b3b339cfa30084a6bcb7f4 ]] || \
  die "AppImage runtime notice checksum manifest changed"
(
  cd "$APPIMAGE_RUNTIME_LICENSES"
  sha256sum -c SHA256SUMS >/dev/null
) || die "an AppImage runtime notice changed after the source-bundle gate"
[[ $(wc -l <"$APPIMAGE_RUNTIME_COMPONENTS") -eq 8 ]] || \
  die "AppImage runtime component manifest must contain exactly seven rows"
[[ $(awk -F '\t' 'NR == 1 { next } NF != 6 { bad=1 } { seen[$1]++ } END { for (x in seen) if (seen[x] != 1) bad=1; print length(seen), bad+0 }' "$APPIMAGE_RUNTIME_COMPONENTS") == '7 0' ]] || \
  die "AppImage runtime component manifest has malformed or duplicate rows"
while IFS=$'\t' read -r component _ _ _ _ notice_paths; do
  [[ "$component" != component ]] || continue
  IFS=';' read -ra component_notices <<<"$notice_paths"
  ((${#component_notices[@]} >= 1)) || \
    die "AppImage runtime component has no notice: $component"
  for notice in "${component_notices[@]}"; do
    [[ "$notice" == LICENSES/AppImage-runtime/* ]] || \
      die "AppImage runtime notice path escapes its license directory: $notice"
    [[ -s "$PAYLOAD/$notice" ]] || \
      die "AppImage runtime component notice is missing: $component ($notice)"
  done
done <"$APPIMAGE_RUNTIME_COMPONENTS"

# Debian package copyright files commonly refer to the canonical texts in this
# directory. AppImage users may be offline and need not run Debian, so include
# regular, hashed copies rather than leaving distro-only references or symlinks.
DEBIAN_COMMON_LICENSES="$PAYLOAD/LICENSES/Debian-common-licenses"
mkdir -p "$DEBIAN_COMMON_LICENSES"
for required in GPL-2 GPL-3 LGPL-2.1; do
  [[ -s "/usr/share/common-licenses/$required" ]] || \
    die "Debian common license text is missing: $required"
done
while IFS= read -r -d '' license; do
  install -m 644 "$license" "$DEBIAN_COMMON_LICENSES/$(basename "$license")"
done < <(find -L /usr/share/common-licenses -maxdepth 1 -type f -print0 | \
  LC_ALL=C sort -z)
for required in GPL-2 GPL-3 LGPL-2.1; do
  [[ -s "$DEBIAN_COMMON_LICENSES/$required" ]] || \
    die "failed to package Debian common license text: $required"
done

# The base image is digest-pinned, while apt intentionally follows Debian 12
# security updates. Record the exact resolved package and tool versions used by
# this artifact instead of claiming a future build environment is immutable.
BUILD_ENVIRONMENT="$PAYLOAD/BUILD-ENVIRONMENT.tsv"
printf 'kind\tname\tversion\tprovenance\n' >"$BUILD_ENVIRONMENT"
printf 'base_image\tdebian\t12\t%s\n' \
  "$EXPECTED_BUILD_BASE" \
  >>"$BUILD_ENVIRONMENT"
printf 'tool\tgcc\t%s\tDebian build container\n' \
  "$(gcc -dumpfullversion -dumpversion)" >>"$BUILD_ENVIRONMENT"
printf 'tool\tbinutils-ld\t%s\tDebian build container\n' \
  "$(ld --version | sed -n '1{s/.* //;p;}')" >>"$BUILD_ENVIRONMENT"
printf 'tool\tcmake\t%s\tDebian build container\n' \
  "$(cmake --version | sed -n '1{s/^cmake version //;p;}')" >>"$BUILD_ENVIRONMENT"
printf 'tool\tninja\t%s\tDebian build container\n' \
  "$(ninja --version)" >>"$BUILD_ENVIRONMENT"
printf 'tool\tpython\t%s\tDebian build container\n' \
  "$(python3 -c 'import platform; print(platform.python_version())')" \
  >>"$BUILD_ENVIRONMENT"
dpkg-query -W -f='package\t${binary:Package}\t${Version}\tDebian 12\n' | \
  LC_ALL=C sort >>"$BUILD_ENVIRONMENT"

SOURCE_REPOSITORY="${SOURCE_REPOSITORY:-}"
if [[ -z "$SOURCE_REPOSITORY" && -n "${GITHUB_REPOSITORY:-}" ]]; then
  SOURCE_REPOSITORY="${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY"
fi
if [[ -z "$SOURCE_REPOSITORY" ]]; then
  SOURCE_REPOSITORY="$(git -C "$REPO" config --get remote.origin.url || true)"
fi
[[ -n "$SOURCE_REPOSITORY" ]] || die "could not determine source repository URL"
if [[ "$SOURCE_REPOSITORY" =~ ^git@github\.com:(.+)$ ]]; then
  SOURCE_REPOSITORY="https://github.com/${BASH_REMATCH[1]}"
fi
SOURCE_REPOSITORY="${SOURCE_REPOSITORY%.git}"
UPSTREAM_REPOSITORY="${UPSTREAM_REPOSITORY:-https://github.com/jackpoison-prog/RingOut}"

if [[ "$PACKAGE_VALIDATION_ONLY" == 1 ]]; then
  SOURCE_SCOPE="This validation-only image was built from a local worktree. The commit above
identifies its base revision but is not claimed as complete matching source for
the uncommitted QA payload. Do not redistribute or publish this image."
else
  SOURCE_SCOPE="The repository revision above contains the complete matching source for Ring
Out, ModernGekko, DolRecomp, the Dolphin-derived runtime, the AppRun/package
scripts, and all vendored dependencies used by the build."
fi

cat >"$PAYLOAD/SOURCE.txt" <<EOF
Ring Out Linux AppImage source provenance
==========================================

Package tag/version: $TAG_VERSION
Validation only:     $([[ "$PACKAGE_VALIDATION_ONLY" == 1 ]] && echo yes || echo no)
Source repository:   $SOURCE_REPOSITORY
Exact source commit: $SOURCE_COMMIT
Source tree:         $SOURCE_REPOSITORY/tree/$SOURCE_COMMIT
Source archive:      $SOURCE_REPOSITORY/archive/$SOURCE_COMMIT.tar.gz
Upstream project:    $UPSTREAM_REPOSITORY

$SOURCE_SCOPE

Rebuild commands for publishable releases are recorded in
.github/workflows/linux-appimage.yml at the tagged commit. The Debian base image
is digest-pinned, but Debian 12 security package resolution intentionally remains
current; BUILD-ENVIRONMENT.tsv records the exact packages and tool versions used
for this artifact, so a future rebuild is not falsely claimed to be identical.

AppImage construction tool (not embedded):
  appimagetool 1.9.1 x86_64
  sha256 ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

Embedded AppImage type-2 runtime:
  AppImage/type2-runtime asset id 456065460 (runtime-x86_64)
  source commit 75849dce7cc37e4319b633df1f116ca895c71a12
  sha256 1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf
  upstream build run/job 28063784345 / 83083595830
  upstream build base alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d

AppImage runtime components and corresponding source/relink materials:
  APPIMAGE-RUNTIME-COMPONENTS.tsv records all seven components statically
  incorporated into the type-2 prefix, their exact versions, immutable source
  revisions or archive hashes, licenses, and full notice paths.

  The designated same-release source/relink companion is:
    $SOURCE_BUNDLE
    sha256 $source_bundle_hash

  It contains the exact expanded type2-runtime tree, original source archive,
  libfuse 3.15.0 and squashfuse 0.5.2 source archives, the exact libfuse mount
  patch, full component notices, and an executable rebuild/relink helper. Keep
  that checksummed asset available beside the AppImage when redistributing it.

Bundled Debian support libraries:
  SYSTEM-LIBRARIES.tsv records each DSO checksum, binary package/version,
  source package/version, and matching sources.debian.org URL. On Debian, a
  source row can also be retrieved with:
    apt-get source SOURCE_PACKAGE=SOURCE_VERSION

Statically linked executable components:
  TOOLCHAIN-COMPONENTS.tsv records the exact glibc, GCC runtime, zlib, and
  libstdc++ binary/source packages incorporated via static linkage into
  DolRecomp and moderngekko-run, with matching sources.debian.org URLs and
  Debian copyright notices. Canonical texts referenced by those notices are
  copied into LICENSES/Debian-common-licenses/ for offline use.

No game image, extracted game file, save, generated per-game recompilation
module, or profile collected from a game is included.
EOF
printf '%s-%s\n' "$FILE_VERSION" "$SOURCE_COMMIT" >"$PAYLOAD/VERSION"

printf '==> source-copy and privacy validation\n'
ABI_PKG="$PAYLOAD/module-src/deps/chassis-abi/StaticRecompABI.h"
ABI_REPO="$REPO/ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp/StaticRecompABI.h"
cmp -s "$ABI_PKG" "$ABI_REPO" || die "packaged chassis ABI header differs from the runtime source"

if grep -Eiq 'matching source (is|lives) in source/|keep source/|\b(ModernGekko|DolRecomp)-[0-9a-f]{7,}[^ ]*\.tar' \
     "$PAYLOAD/CREDITS.txt"; then
  die "AppImage credits contain a stale source/ claim or commit-named archive"
fi

# The old one-file runtime notice predated mimalloc and used mutable source
# URLs. It must never coexist with the exact seven-component closure.
if find "$APPDIR" -type f -name 'AppImage-runtime-LICENSE.txt' -o \
     -name 'AppImage-type2-runtime-MIT.txt' | grep -q .; then
  die "obsolete incomplete AppImage runtime notice reached the payload"
fi
for required in \
  'AppImage/type2-runtime asset id 456065460' \
  'source commit 75849dce7cc37e4319b633df1f116ca895c71a12' \
  'sha256 1cc49bcf1e2ccd593c379adb17c9f85a36d619088296504de95b1d06215aebbf' \
  'upstream build run/job 28063784345 / 83083595830' \
  'alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d' \
  "$SOURCE_BUNDLE" \
  "sha256 $source_bundle_hash"; do
  grep -Fq "$required" "$PAYLOAD/SOURCE.txt" || \
    die "SOURCE.txt is missing AppImage runtime provenance: $required"
done

copy_root="$PAYLOAD/module-src/deps/dolrecomp-src"
while IFS= read -r relative; do
  [[ -f "$REPO/DolRecomp/src/$relative" ]] || \
    die "module source copy has no counterpart: $relative"
  cmp -s "$copy_root/$relative" "$REPO/DolRecomp/src/$relative" || \
    die "module source copy differs from DolRecomp/src: $relative"
done < <(cd "$copy_root" && find . -type f -printf '%P\n' | sort)

if awk '
  /^\[(ActionReplay|Gecko)_Enabled\]$/ { enabled=1; next }
  /^\[/ { enabled=0 }
  enabled && /^\$/ { found=1 }
  END { exit found ? 0 : 1 }
' "$PAYLOAD/GRSEAF.ini"; then
  die "AppImage would enable cheat codes by default"
fi

mapfile -d '' forbidden_files < <(find "$APPDIR" -type f \( \
  -iname '*.iso' -o -iname '*.wbfs' -o -iname '*.gcm' -o -iname '*.rvz' -o \
  -iname '*.gcz' -o -iname '*.dol' -o -iname '*.afs' -o -iname '*.sfd' -o \
  -iname '*.raw' -o -iname '*.gci' -o -iname '*.sav' -o -iname '*.dtm' -o \
  -iname '*.profdata' -o -iname '*_recomp.dll' -o -iname '*_recomp.so' -o \
  -iname 'boot.bin' -o -iname 'main.dol' -o -iname 'apploader.img' -o \
  -iname 'fst.bin' \) -print0)
if ((${#forbidden_files[@]})); then
  printf 'forbidden game/save/generated file: %s\n' "${forbidden_files[@]}" >&2
  die "game-derived or personal data reached the AppDir"
fi
if find "$APPDIR" -type d \( -iname game -o -iname work -o -iname MemoryCard \
     -o -iname StateSaves \) -print -quit | grep -q .; then
  die "private/generated directory reached the AppDir"
fi
"$REPO/.github/scripts/privacy-scan.sh" "$APPDIR"

(
  cd "$APPDIR"
  find . -type f ! -path './usr/share/ringout/MANIFEST.sha256' -print0 |
    LC_ALL=C sort -z | xargs -0 sha256sum >"$PAYLOAD/MANIFEST.sha256"
)
for required in \
  './usr/share/ringout/APPIMAGE-RUNTIME-COMPONENTS.tsv' \
  './usr/share/ringout/LICENSES/AppImage-runtime/SHA256SUMS' \
  './usr/share/ringout/LICENSES/AppImage-runtime/NOTICE-SOURCES.tsv' \
  './usr/share/ringout/LICENSES/AppImage-runtime/type2-runtime-LICENSE' \
  './usr/share/ringout/LICENSES/AppImage-runtime/musl-1.2.5-COPYRIGHT' \
  './usr/share/ringout/LICENSES/AppImage-runtime/libfuse-3.15.0-LICENSE' \
  './usr/share/ringout/LICENSES/AppImage-runtime/libfuse-3.15.0-LGPL2.txt' \
  './usr/share/ringout/LICENSES/AppImage-runtime/squashfuse-0.5.2-LICENSE' \
  './usr/share/ringout/LICENSES/AppImage-runtime/zstd-1.5.6-LICENSE' \
  './usr/share/ringout/LICENSES/AppImage-runtime/zlib-1.3.2-LICENSE' \
  './usr/share/ringout/LICENSES/AppImage-runtime/mimalloc-2.1.7-LICENSE'; do
  grep -Fq "  $required" "$PAYLOAD/MANIFEST.sha256" || \
    die "AppDir manifest does not cover runtime legal file: $required"
done

# appimagetool forwards SOURCE_DATE_EPOCH to mksquashfs. Normalize every input
# as well so a tool regression is visible as a reproducibility mismatch rather
# than silently retaining checkout or package-manager times.
find "$APPDIR" -depth -print0 | xargs -0 touch -h -d "@$SOURCE_DATE_EPOCH"

printf '==> normalized AppImage\n'
TMP_IMAGE="$WORK/$ARTIFACT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" \
  --no-appstream --comp zstd --runtime-file "$APPIMAGE_RUNTIME" \
  "$APPDIR" "$TMP_IMAGE"
chmod 755 "$TMP_IMAGE"
file "$TMP_IMAGE" | grep -q 'x86-64' || die "output is not an x86-64 AppImage"
runtime_version=$(
  "$TMP_IMAGE" --appimage-version 2>&1
)
[[ "$runtime_version" == \
   'AppImage runtime version: https://github.com/AppImage/type2-runtime/commit/75849dc' ]] || \
  die "AppImage output reports an unexpected embedded runtime version: $runtime_version"
runtime_size=$(stat -c '%s' "$APPIMAGE_RUNTIME")
# appimagetool necessarily writes the final AppImage MD5 into the runtime's
# 16-byte .digest_md5 slot. Everything outside that designated mutable slot
# must remain byte-identical to the audited runtime input.
read -r digest_offset_hex digest_size_hex < <(
  readelf -SW "$APPIMAGE_RUNTIME" | \
    awk '$2 == ".digest_md5" { print $5, $6; found=1 } END { exit !found }'
)
[[ -n "$digest_offset_hex" && -n "$digest_size_hex" ]] || \
  die "pinned type-2 runtime has no .digest_md5 section"
digest_offset=$((16#$digest_offset_hex))
digest_size=$((16#$digest_size_hex))
[[ "$digest_size" -eq 16 && "$digest_offset" -gt 0 && \
   $((digest_offset + digest_size)) -le "$runtime_size" ]] || \
  die "pinned type-2 runtime has an unexpected mutable digest slot"
cmp -s -n "$digest_offset" "$APPIMAGE_RUNTIME" "$TMP_IMAGE" || \
  die "AppImage prefix differs from the pinned runtime before .digest_md5"
runtime_tail=$((runtime_size - digest_offset - digest_size))
cmp -s -n "$runtime_tail" \
  -i "$((digest_offset + digest_size)):$((digest_offset + digest_size))" \
  "$APPIMAGE_RUNTIME" "$TMP_IMAGE" || \
  die "AppImage prefix differs from the pinned runtime after .digest_md5"

VERIFY="$WORK/verify"
mkdir -p "$VERIFY"
(
  cd "$VERIFY"
  "$TMP_IMAGE" --appimage-extract >/dev/null
)
EXTRACTED="$VERIFY/squashfs-root"
[[ -x "$EXTRACTED/AppRun" ]] || die "AppRun is not executable after extraction"
[[ -x "$EXTRACTED/usr/bin/RingOut" ]] || die "C++ launcher missing after extraction"
[[ -x "$EXTRACTED/usr/bin/moderngekko-run" ]] || die "runtime missing after extraction"
[[ -x "$EXTRACTED/usr/bin/moderngekko-port" ]] || die "setup helper missing after extraction"
[[ -x "$EXTRACTED/usr/bin/dolrecomp" ]] || die "dolrecomp missing after extraction"
[[ -s "$EXTRACTED/usr/share/ringout/module-src/CMakeLists.txt" ]] || \
  die "setup helper module sources missing from their resolved AppImage path"
[[ -s "$EXTRACTED/usr/share/ringout/GRSEAF.ini" ]] || \
  die "setup helper game settings missing from their resolved AppImage path"
(
  cd "$EXTRACTED"
  sha256sum -c usr/share/ringout/MANIFEST.sha256 >/dev/null
)
EXTRACTED_PAYLOAD="$EXTRACTED/usr/share/ringout"
[[ $(wc -l <"$EXTRACTED_PAYLOAD/APPIMAGE-RUNTIME-COMPONENTS.tsv") -eq 8 ]] || \
  die "extracted runtime component manifest does not contain eight rows"
[[ $(sha256sum "$EXTRACTED_PAYLOAD/APPIMAGE-RUNTIME-COMPONENTS.tsv" | awk '{print $1}') == \
   afee9393f5fef576d7e63b837aa6151b2805c9e080af03188d48761bdde0c243 ]] || \
  die "extracted runtime component manifest fails its authoritative digest gate"
[[ $(sha256sum "$EXTRACTED_PAYLOAD/LICENSES/AppImage-runtime/NOTICE-SOURCES.tsv" | awk '{print $1}') == \
   76a5ac9991ecbc527959f070e0cc158098ed45200bd0aa23949a55ffcad993cf ]] || \
  die "extracted runtime notice-source manifest fails its authoritative digest gate"
[[ $(sha256sum "$EXTRACTED_PAYLOAD/LICENSES/AppImage-runtime/SHA256SUMS" | awk '{print $1}') == \
   b2bef013beaab73a901791711a921569d7f9b35fa6b3b339cfa30084a6bcb7f4 ]] || \
  die "extracted runtime notice checksum manifest fails its authoritative digest gate"
(
  cd "$EXTRACTED_PAYLOAD/LICENSES/AppImage-runtime"
  sha256sum -c SHA256SUMS >/dev/null
) || die "extracted AppImage runtime notices fail their exact digest gate"
[[ ! -e "$EXTRACTED_PAYLOAD/LICENSES/AppImage-type2-runtime-MIT.txt" ]] || \
  die "obsolete incomplete runtime notice exists after extraction"
for required in \
  '28063784345 / 83083595830' \
  'alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d' \
  "$SOURCE_BUNDLE" \
  "$source_bundle_hash"; do
  grep -Fq "$required" "$EXTRACTED_PAYLOAD/SOURCE.txt" || \
    die "extracted SOURCE.txt lost runtime provenance: $required"
done

"$REPO/.github/scripts/smoke-appimage-module.sh" \
  "$EXTRACTED" "$WORK/module-smoke" "$MODULE_INFO"

SELFTEST_DATA="$WORK/selftest-data"
RINGOUT_DATA_DIR="$SELFTEST_DATA" APPIMAGE_EXTRACT_AND_RUN=1 \
  "$TMP_IMAGE" --ringout-self-test

TMP_SHA="$WORK/$ARTIFACT.sha256"
artifact_hash="$(sha256sum "$TMP_IMAGE" | awk '{print $1}')"
printf '%s  %s\n' "$artifact_hash" "$ARTIFACT" >"$TMP_SHA"
mv -f -- "$TMP_IMAGE" "$FINAL_IMAGE"
mv -f -- "$TMP_SHA" "$FINAL_SHA"
mv -f -- "$TMP_SOURCE_BUNDLE" "$FINAL_SOURCE_BUNDLE"
mv -f -- "$TMP_SOURCE_SHA" "$FINAL_SOURCE_SHA"

printf 'OK: %s\n' "$FINAL_IMAGE"
printf 'SHA-256: %s\n' "$artifact_hash"
printf 'Runtime source/relink bundle: %s\n' "$FINAL_SOURCE_BUNDLE"
printf 'Runtime source/relink SHA-256: %s\n' "$source_bundle_hash"
printf 'Payload: %s files; Sys: %s files; glibc floor: %s\n' \
  "$(find "$APPDIR" -type f | wc -l)" "$sys_files" "$glibc_floor"
