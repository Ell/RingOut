#!/usr/bin/env bash
# Assemble and validate the portable Windows x86-64 package from Linux-built
# PE binaries. Staging is an allowlist: no working dist tree, disc extraction,
# generated module, save, or developer userdata is ever copied wholesale.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="v1.2.1-ell.11"
BUILD_DIR="$REPO/build-windows-cross"
OUT_DIR="$REPO/dist/out"
RUNTIME=""
LAUNCHER=""
PORT=""
DOLRECOMP=""
SYS_DIR=""
TOOLCHAIN_DIR=""
LLVM_ARCHIVE=""
CMAKE_ARCHIVE=""
NINJA_ARCHIVE=""
PYTHON_ARCHIVE=""
FORCE=0
DLL_DIRS=()

usage() {
  cat <<'EOF'
Usage: package-windows-cross.sh [options]

  --version TAG                 package/release tag (default v1.2.1-ell.11)
  --build-dir DIR               cross-build tree
  --out-dir DIR                 output directory (default dist/out)
  --runtime FILE                explicit moderngekko-run.exe
  --launcher FILE               explicit C++ RingOut.exe
  --port FILE                   explicit moderngekko-port.exe
  --dolrecomp FILE              explicit dolrecomp.exe
  --sys-dir DIR                 explicit Dolphin Sys resource directory
  --dll-dir DIR                 additional PE runtime-DLL search directory
                                (repeatable)

Supply the Windows-native first-run toolchain in exactly one form:

  --toolchain-dir DIR           already assembled toolchain directory

or all four pinned upstream archives:

  --llvm-mingw-archive FILE
  --cmake-archive FILE
  --ninja-archive FILE
  --python-archive FILE

  --force                       atomically replace an existing named artifact
  -h, --help                    show this help

Environment used in SOURCE.txt:
  SOURCE_REPOSITORY, SOURCE_COMMIT, UPSTREAM_REPOSITORY, SOURCE_DATE_EPOCH
EOF
}

die() { printf 'package-windows-cross: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }

while (($#)); do
  case "$1" in
    --version)               (($# >= 2)) || die "$1 needs a value"; VERSION=$2; shift 2 ;;
    --build-dir)             (($# >= 2)) || die "$1 needs a value"; BUILD_DIR=$2; shift 2 ;;
    --out-dir)               (($# >= 2)) || die "$1 needs a value"; OUT_DIR=$2; shift 2 ;;
    --runtime)               (($# >= 2)) || die "$1 needs a value"; RUNTIME=$2; shift 2 ;;
    --launcher)              (($# >= 2)) || die "$1 needs a value"; LAUNCHER=$2; shift 2 ;;
    --port)                  (($# >= 2)) || die "$1 needs a value"; PORT=$2; shift 2 ;;
    --dolrecomp)             (($# >= 2)) || die "$1 needs a value"; DOLRECOMP=$2; shift 2 ;;
    --sys-dir)               (($# >= 2)) || die "$1 needs a value"; SYS_DIR=$2; shift 2 ;;
    --toolchain-dir)         (($# >= 2)) || die "$1 needs a value"; TOOLCHAIN_DIR=$2; shift 2 ;;
    --llvm-mingw-archive)    (($# >= 2)) || die "$1 needs a value"; LLVM_ARCHIVE=$2; shift 2 ;;
    --cmake-archive)         (($# >= 2)) || die "$1 needs a value"; CMAKE_ARCHIVE=$2; shift 2 ;;
    --ninja-archive)         (($# >= 2)) || die "$1 needs a value"; NINJA_ARCHIVE=$2; shift 2 ;;
    --python-archive)        (($# >= 2)) || die "$1 needs a value"; PYTHON_ARCHIVE=$2; shift 2 ;;
    --dll-dir)               (($# >= 2)) || die "$1 needs a value"; DLL_DIRS+=("$2"); shift 2 ;;
    --force)                 FORCE=1; shift ;;
    -h|--help)               usage; exit 0 ;;
    *)                       die "unknown option: $1" ;;
  esac
done

[[ "$VERSION" =~ ^v?[0-9][0-9A-Za-z._+-]*$ ]] || die "unsafe version/tag: $VERSION"
TAG_VERSION="$VERSION"
[[ "$TAG_VERSION" == v* ]] || TAG_VERSION="v$TAG_VERSION"
FILE_VERSION="${TAG_VERSION#v}"
PACKAGE="RingOut-$FILE_VERSION-windows-x86_64"
ARTIFACT="$PACKAGE.zip"

for cmd in awk cp find git grep head install mkdir mktemp mv realpath sed \
           sha256sum sort strings touch tr unzip wc xargs \
           x86_64-w64-mingw32-gcc x86_64-w64-mingw32-objdump \
           x86_64-w64-mingw32-strip zip; do
  need "$cmd"
done

BUILD_DIR="$(realpath -m "$BUILD_DIR")"
OUT_DIR="$(realpath -m "$OUT_DIR")"
[[ -d "$BUILD_DIR" ]] || die "build directory not found: $BUILD_DIR"

# GNU PE linkers honor SOURCE_DATE_EPOCH for the COFF timestamp. Resolve it
# before compiling RingOut.exe, not merely before touching the ZIP stage.
SOURCE_COMMIT="${SOURCE_COMMIT:-$(git -C "$REPO" rev-parse HEAD)}"
[[ "$SOURCE_COMMIT" =~ ^[0-9a-fA-F]{40}$ ]] || die "invalid SOURCE_COMMIT: $SOURCE_COMMIT"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO" show -s --format=%ct "$SOURCE_COMMIT")}"
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || die "invalid SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH"
((SOURCE_DATE_EPOCH >= 315532800)) || SOURCE_DATE_EPOCH=315532800
export SOURCE_DATE_EPOCH
# ZIP converts filesystem mtimes to timezone-less DOS wall clocks. Pin the
# producer timezone so identical commits package identically on CI and local
# hosts, and so the one-day safety offset below is valid for every consumer.
export TZ=UTC

resolve_project_binary() {
  local label=$1 explicit=$2
  shift 2
  local candidate root found

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
  for root in "$BUILD_DIR" "$REPO/ModernGekko/Binary"; do
    [[ -d "$root" ]] || continue
    found="$(find "$root" -type f -iname "$label" \
      ! -path '*/CMakeFiles/*' -print -quit)"
    if [[ -n "$found" ]]; then
      realpath "$found"
      return
    fi
  done
  die "could not locate $label; pass its explicit option"
}

RUNTIME="$(resolve_project_binary moderngekko-run.exe "$RUNTIME" \
  "$BUILD_DIR/Binaries/moderngekko-run.exe" \
  "$BUILD_DIR/Binary/x64/moderngekko-run.exe" \
  "$REPO/ModernGekko/Binary/x64/moderngekko-run.exe")"
LAUNCHER="$(resolve_project_binary RingOut.exe "$LAUNCHER" \
  "$BUILD_DIR/RingOut.exe" \
  "$BUILD_DIR/Binaries/RingOut.exe")"
PORT="$(resolve_project_binary moderngekko-port.exe "$PORT" \
  "$BUILD_DIR/moderngekko-port.exe" \
  "$BUILD_DIR/Binaries/moderngekko-port.exe")"
DOLRECOMP="$(resolve_project_binary dolrecomp.exe "$DOLRECOMP" \
  "$BUILD_DIR/dolrecomp-build/dolrecomp.exe" \
  "$BUILD_DIR/dolrecomp-build/src/dolrecomp.exe" \
  "$BUILD_DIR/DolRecomp/dolrecomp.exe")"

if [[ -n "$SYS_DIR" ]]; then
  [[ -d "$SYS_DIR" ]] || die "Sys directory not found: $SYS_DIR"
  SYS_DIR="$(realpath "$SYS_DIR")"
else
  for candidate in \
    "$(dirname "$RUNTIME")/Sys" \
    "$BUILD_DIR/Binaries/Sys" \
    "$BUILD_DIR/Binary/x64/Sys" \
    "$REPO/ModernGekko/vendor/dolphin/Data/Sys"; do
    if [[ -d "$candidate" ]]; then SYS_DIR="$(realpath "$candidate")"; break; fi
  done
fi
[[ -n "$SYS_DIR" && -d "$SYS_DIR" ]] || die "Dolphin Sys resources were not found"
for required in GC/dsp_rom.bin GC/font_western.bin ApprovedInis.json; do
  [[ -s "$SYS_DIR/$required" ]] || die "incomplete Sys tree: missing $required"
done
sys_files="$(find "$SYS_DIR" -type f | wc -l)"
((sys_files >= 100)) || die "incomplete Sys tree: only $sys_files files"

if [[ -n "$TOOLCHAIN_DIR" ]]; then
  [[ -z "$LLVM_ARCHIVE$CMAKE_ARCHIVE$NINJA_ARCHIVE$PYTHON_ARCHIVE" ]] || \
    die "use --toolchain-dir or archive options, not both"
  [[ -d "$TOOLCHAIN_DIR" ]] || die "toolchain directory not found: $TOOLCHAIN_DIR"
  TOOLCHAIN_DIR="$(realpath "$TOOLCHAIN_DIR")"
else
  [[ -n "$LLVM_ARCHIVE" && -n "$CMAKE_ARCHIVE" && -n "$NINJA_ARCHIVE" && -n "$PYTHON_ARCHIVE" ]] || \
    die "supply --toolchain-dir or all four pinned toolchain archives"
  for archive in "$LLVM_ARCHIVE" "$CMAKE_ARCHIVE" "$NINJA_ARCHIVE" "$PYTHON_ARCHIVE"; do
    [[ -f "$archive" ]] || die "toolchain archive not found: $archive"
  done
fi

for d in "${DLL_DIRS[@]}"; do [[ -d "$d" ]] || die "DLL directory not found: $d"; done
[[ -d /usr/x86_64-w64-mingw32/bin ]] && DLL_DIRS+=(/usr/x86_64-w64-mingw32/bin)

mkdir -p "$OUT_DIR"
FINAL_ZIP="$OUT_DIR/$ARTIFACT"
FINAL_SHA="$FINAL_ZIP.sha256"
if ((FORCE == 0)) && { [[ -e "$FINAL_ZIP" ]] || [[ -e "$FINAL_SHA" ]]; }; then
  die "artifact already exists (use --force): $FINAL_ZIP"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ringout-windows-package.XXXXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
STAGE="$WORK/$PACKAGE"
mkdir -p "$STAGE/bin" "$STAGE/tools" "$STAGE/LICENSES"

copy_tree() {
  local source=$1 destination=$2
  mkdir -p "$destination"
  cp -a "$source"/. "$destination"/
}

check_pe() {
  local file=$1 magic
  magic="$(LC_ALL=C head -c 2 "$file" 2>/dev/null || true)"
  [[ "$magic" == MZ ]] || die "not a PE executable: $file"
  x86_64-w64-mingw32-objdump -p "$file" >/dev/null 2>&1 || \
    die "objdump could not parse PE file: $file"
}

printf '==> project payload\n'
install -m 644 "$REPO/dist/windows/RingOut.cmd" "$STAGE/RingOut.cmd"
install -m 644 "$REPO/dist/windows/RingOut.ps1" "$STAGE/RingOut.ps1"
install -m 644 "$REPO/dist/windows/setup.ps1" "$STAGE/setup.ps1"
install -m 644 "$REPO/dist/windows/README.txt" "$STAGE/README.txt"
install -m 644 "$REPO/dist/windows/CREDITS.txt" "$STAGE/CREDITS.txt"
install -m 644 "$REPO/dist/windows/THIRD-PARTY-NOTICES.txt" "$STAGE/THIRD-PARTY-NOTICES.txt"

install -m 755 "$LAUNCHER" "$STAGE/RingOut.exe"
install -m 755 "$RUNTIME" "$STAGE/bin/moderngekko-run.exe"
install -m 755 "$PORT" "$STAGE/tools/moderngekko-port.exe"
install -m 755 "$DOLRECOMP" "$STAGE/tools/dolrecomp.exe"
[[ -s "$(dirname "$LAUNCHER")/fonts/DroidSans.ttf" ]] || \
  die "C++ launcher font payload is missing beside $LAUNCHER"
mkdir -p "$STAGE/fonts"
install -m 644 "$(dirname "$LAUNCHER")/fonts/DroidSans.ttf" \
  "$STAGE/fonts/DroidSans.ttf"
install -m 644 "$(dirname "$LAUNCHER")/fonts/Roboto-Medium.ttf" \
  "$STAGE/fonts/Roboto-Medium.ttf"
copy_tree "$SYS_DIR" "$STAGE/bin/Sys"

copy_tree "$REPO/dist/RingOut-1.0-dist/module-src" "$STAGE/module-src"
# This profile was produced from a private gameplay run and the Windows setup
# does not consume it. Keep the portable release strictly source/config only.
rm -f -- "$STAGE/module-src/module.profdata"
copy_tree "$REPO/dist/RingOut-1.0-dist/shaders" "$STAGE/shaders"

GAMESETTINGS="$REPO/work/mg_userdir/GameSettings/GRSEAF.ini"
[[ -s "$GAMESETTINGS" ]] || die "tracked GRSEAF.ini not found: $GAMESETTINGS"
mkdir -p "$STAGE/userdata/GameSettings"
awk '
  /^\[(ActionReplay|Gecko)_Enabled\]$/ { enabled=1; print; next }
  /^\[/ { enabled=0 }
  enabled && /^\$/ { next }
  { print }
' "$GAMESETTINGS" >"$STAGE/userdata/GameSettings/GRSEAF.ini"

printf '==> licences and provenance\n'
install -m 644 "$REPO/LICENSE" "$STAGE/LICENSES/RingOut-GPL-2.0-or-later.txt"
install -m 644 "$REPO/ModernGekko/LICENSE" "$STAGE/LICENSES/ModernGekko-GPL-3.0-or-later.txt"
install -m 644 "$REPO/DolRecomp/LICENSE" "$STAGE/LICENSES/DolRecomp-GPL-3.0-or-later.txt"
install -m 644 "$REPO/ModernGekko/PROVENANCE.md" "$STAGE/LICENSES/ModernGekko-PROVENANCE.md"
install -m 644 "$REPO/ModernGekko/vendor/dolphin/COPYING" "$STAGE/LICENSES/Dolphin-COPYING.txt"
copy_tree "$REPO/ModernGekko/vendor/dolphin/LICENSES" "$STAGE/LICENSES/Dolphin"
# Ninja's official Windows ZIP contains only ninja.exe. Supply the standard
# Apache-2.0 text from the vendored source tree rather than publishing a binary
# whose archive silently dropped its licence.
install -m 644 \
  "$REPO/ModernGekko/vendor/dolphin/Externals/spirv_cross/SPIRV-Cross/LICENSES/Apache-2.0.txt" \
  "$STAGE/LICENSES/Ninja-Apache-2.0.txt"

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

cat >"$STAGE/SOURCE.txt" <<EOF
Ring Out Windows package source provenance
==========================================

Package tag/version: $TAG_VERSION
Source repository:   $SOURCE_REPOSITORY
Exact source commit: $SOURCE_COMMIT
Tagged source tree:  $SOURCE_REPOSITORY/tree/$TAG_VERSION
Source archive:      $SOURCE_REPOSITORY/archive/$SOURCE_COMMIT.tar.gz
Upstream project:    $UPSTREAM_REPOSITORY

This binary is a modified Windows revival built from the exact commit above.
That repository revision contains the preferred source for Ring Out, the
launcher/setup/package scripts, DolRecomp, ModernGekko/RecompCore, the
Dolphin-derived runtime, and the vendored dependencies used by this build.
The release tag's automatically generated source archive is offered from the
same GitHub release page as this ZIP. Build commands and pinned compiler-tool
archives are in .github/workflows/windows-cross.yml and
.github/scripts/package-windows-cross.sh at the exact commit above.

No game image, extracted game file, save, or generated per-game recompilation
module is part of this package or source repository.
EOF

printf '==> bundled first-run toolchain\n'
mkdir -p "$STAGE/toolchain"
verify_archive() {
  local archive=$1 expected=$2 actual
  actual="$(sha256sum "$archive" | awk '{print $1}')"
  [[ "$actual" == "$expected" ]] || \
    die "checksum mismatch for $(basename "$archive"): expected $expected, got $actual"
}

if [[ -n "$TOOLCHAIN_DIR" ]]; then
  copy_tree "$TOOLCHAIN_DIR" "$STAGE/toolchain"
else
  verify_archive "$LLVM_ARCHIVE"  b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35
  verify_archive "$CMAKE_ARCHIVE" e8139d85b3813bc38833142ae1940472e9a587e9b5d2718ac1804c60f4e57a64
  verify_archive "$NINJA_ARCHIVE" 07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65
  verify_archive "$PYTHON_ARCHIVE" 009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b

  EXTRACT="$WORK/toolchain-extract"
  mkdir -p "$EXTRACT/llvm" "$EXTRACT/cmake" "$EXTRACT/ninja" "$EXTRACT/python"
  unzip -q "$LLVM_ARCHIVE" -d "$EXTRACT/llvm"
  unzip -q "$CMAKE_ARCHIVE" -d "$EXTRACT/cmake"
  unzip -q "$NINJA_ARCHIVE" -d "$EXTRACT/ninja"
  unzip -q "$PYTHON_ARCHIVE" -d "$EXTRACT/python"

  llvm_clang="$(find "$EXTRACT/llvm" -type f -path '*/bin/clang.exe' -print -quit)"
  [[ -n "$llvm_clang" ]] || die "llvm-mingw archive has no bin/clang.exe"
  llvm_root="$(dirname "$(dirname "$llvm_clang")")"
  # The archive name describes the Windows HOST, and upstream also includes
  # four unrelated target sysroots plus its own Python distribution. First-run
  # setup only targets x86_64-w64-mingw32 and carries a separately pinned
  # embeddable Python below. Select the required target instead of merging two
  # Python versions or adding hundreds of megabytes of ARM/i686 libraries.
  for entry in "$llvm_root"/*; do
    name="$(basename "$entry")"
    case "$name" in
      aarch64-w64-mingw32|arm64ec-w64-mingw32|armv7-w64-mingw32|i686-w64-mingw32|python)
        continue
        ;;
    esac
    if [[ -d "$entry" ]]; then
      copy_tree "$entry" "$STAGE/toolchain/$name"
    else
      cp -a "$entry" "$STAGE/toolchain/$name"
    fi
  done

  cmake_exe="$(find "$EXTRACT/cmake" -type f -path '*/bin/cmake.exe' -print -quit)"
  [[ -n "$cmake_exe" ]] || die "CMake archive has no bin/cmake.exe"
  cmake_root="$(dirname "$(dirname "$cmake_exe")")"
  copy_tree "$cmake_root/bin" "$STAGE/toolchain/bin"
  [[ -d "$cmake_root/share" ]] && copy_tree "$cmake_root/share" "$STAGE/toolchain/share"
  [[ -d "$cmake_root/doc" ]] && copy_tree "$cmake_root/doc" "$STAGE/toolchain/doc"

  ninja_exe="$(find "$EXTRACT/ninja" -type f -iname ninja.exe -print -quit)"
  [[ -n "$ninja_exe" ]] || die "Ninja archive has no ninja.exe"
  install -m 755 "$ninja_exe" "$STAGE/toolchain/bin/ninja.exe"

  python_exe="$(find "$EXTRACT/python" -type f -iname python.exe -print -quit)"
  [[ -n "$python_exe" ]] || die "Python archive has no python.exe"
  copy_tree "$(dirname "$python_exe")" "$STAGE/toolchain/python"
fi

for marker in bin/clang.exe bin/cmake.exe bin/ninja.exe python/python.exe; do
  [[ -s "$STAGE/toolchain/$marker" ]] || die "incomplete toolchain: missing $marker"
  check_pe "$STAGE/toolchain/$marker"
done
if [[ ! -s "$STAGE/toolchain/bin/ld.lld.exe" && ! -s "$STAGE/toolchain/bin/lld-link.exe" ]]; then
  die "incomplete toolchain: no LLVM linker"
fi
find "$STAGE/toolchain" -type f -iname 'python3*.zip' -print -quit | grep -q . || \
  die "incomplete toolchain: Python standard-library ZIP missing"
find "$STAGE/toolchain" -type f -iname windows.h -print -quit | grep -q . || \
  die "incomplete toolchain: Windows SDK headers missing"
find "$STAGE/toolchain/share" -type d -iname 'cmake-*' -print -quit 2>/dev/null | grep -q . || \
  die "incomplete toolchain: CMake modules missing"

printf '==> PE import closure\n'
pe_imports() {
  x86_64-w64-mingw32-objdump -p "$1" 2>/dev/null |
    sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' | tr -d '\r'
}

is_windows_system_dll() {
  local dll="${1,,}"
  case "$dll" in
    api-ms-win-*.dll|ext-ms-win-*.dll) return 0 ;;
    advapi32.dll|avrt.dll|bcrypt.dll|cfgmgr32.dll|comctl32.dll|comdlg32.dll|crypt32.dll|cryptui.dll|d3d11.dll|d3d12.dll|d3dcompiler_47.dll|dbghelp.dll|dnsapi.dll|dwmapi.dll|dxgi.dll|gdi32.dll|hid.dll|imm32.dll|iphlpapi.dll|kernel32.dll|ksuser.dll|mf.dll|mfplat.dll|mfreadwrite.dll|mfuuid.dll|mmdevapi.dll|mpr.dll|msvcrt.dll|netapi32.dll|normaliz.dll|ntdll.dll|ole32.dll|oleacc.dll|oleaut32.dll|opengl32.dll|powrprof.dll|propsys.dll|psapi.dll|qwave.dll|rpcrt4.dll|secur32.dll|setupapi.dll|shcore.dll|shell32.dll|shlwapi.dll|strmiids.dll|ucrtbase.dll|user32.dll|userenv.dll|usp10.dll|uxtheme.dll|version.dll|vulkan-1.dll|winhttp.dll|wininet.dll|winmm.dll|winspool.drv|wintrust.dll|ws2_32.dll|wtsapi32.dll) return 0 ;;
    *) return 1 ;;
  esac
}

find_import_source() {
  local dll=$1 origin_dir=$2 dir hit compiler
  for dir in "$origin_dir" "${DLL_DIRS[@]}" "$BUILD_DIR"; do
    [[ -d "$dir" ]] || continue
    hit="$(find "$dir" -type f -iname "$dll" ! -path '*/CMakeFiles/*' -print -quit)"
    if [[ -n "$hit" ]]; then printf '%s\n' "$hit"; return 0; fi
  done
  for compiler in x86_64-w64-mingw32-g++ x86_64-w64-mingw32-gcc; do
    command -v "$compiler" >/dev/null 2>&1 || continue
    hit="$("$compiler" -print-file-name="$dll" 2>/dev/null || true)"
    if [[ -n "$hit" && "$hit" != "$dll" && -f "$hit" ]]; then
      printf '%s\n' "$hit"
      return 0
    fi
  done
  return 1
}

copy_import_closure() {
  local initial=$1 destination=$2 current key dll existing source
  local -a queue=("$initial")
  local -A seen=()
  while ((${#queue[@]})); do
    current="${queue[0]}"
    queue=("${queue[@]:1}")
    key="$(realpath "$current")"
    [[ -z "${seen[$key]:-}" ]] || continue
    seen[$key]=1
    check_pe "$current"
    while IFS= read -r dll; do
      [[ -n "$dll" ]] || continue
      if is_windows_system_dll "$dll"; then continue; fi
      existing="$(find "$destination" -maxdepth 1 -type f -iname "$dll" -print -quit)"
      if [[ -z "$existing" ]]; then
        source="$(find_import_source "$dll" "$(dirname "$current")" || true)"
        [[ -n "$source" ]] || die "unresolved non-system import $dll required by $current"
        install -m 755 "$source" "$destination/$dll"
        existing="$destination/$dll"
        x86_64-w64-mingw32-strip --strip-unneeded "$existing" 2>/dev/null || true
        printf '    %s -> %s\n' "$dll" "${destination#$STAGE/}/$dll"
      fi
      queue+=("$existing")
    done < <(pe_imports "$current")
  done
}

for file in "$STAGE/RingOut.exe" "$STAGE/bin/moderngekko-run.exe" \
            "$STAGE/tools/moderngekko-port.exe" "$STAGE/tools/dolrecomp.exe"; do
  x86_64-w64-mingw32-strip --strip-unneeded "$file" 2>/dev/null || true
done
copy_import_closure "$STAGE/RingOut.exe" "$STAGE"
copy_import_closure "$STAGE/bin/moderngekko-run.exe" "$STAGE/bin"
copy_import_closure "$STAGE/tools/moderngekko-port.exe" "$STAGE/tools"
copy_import_closure "$STAGE/tools/dolrecomp.exe" "$STAGE/tools"

validate_import_dir() {
  local dir=$1 pe dll sibling
  while IFS= read -r -d '' pe; do
    check_pe "$pe"
    while IFS= read -r dll; do
      [[ -n "$dll" ]] || continue
      is_windows_system_dll "$dll" && continue
      sibling="$(find "$(dirname "$pe")" -maxdepth 1 -type f -iname "$dll" -print -quit)"
      [[ -n "$sibling" ]] || die "package import validation failed: $pe needs $dll beside it"
    done < <(pe_imports "$pe")
  done < <(find "$dir" -maxdepth 1 -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
}
validate_import_dir "$STAGE"
validate_import_dir "$STAGE/bin"
validate_import_dir "$STAGE/tools"

{
  printf 'Project PE import report\n========================\n'
  while IFS= read -r -d '' pe; do
    printf '\n%s\n' "${pe#$STAGE/}"
    pe_imports "$pe" | LC_ALL=C sort | sed 's/^/  /'
  done < <(find "$STAGE" -maxdepth 2 -type f \( -iname '*.exe' -o -iname '*.dll' \) \
             ! -path "$STAGE/toolchain/*" -print0 | sort -z)
} >"$STAGE/PE-IMPORTS.txt"

printf '==> privacy and package-structure validation\n'
[[ -s "$STAGE/bin/moderngekko-run.exe" ]] || die "runtime was not staged"
[[ -s "$STAGE/tools/moderngekko-port.exe" ]] || die "portable setup helper was not staged"
[[ -s "$STAGE/tools/dolrecomp.exe" ]] || die "dolrecomp was not staged"
grep -aFq 'RingOut C++ launcher self-test' "$STAGE/RingOut.exe" || \
  die "top-level RingOut.exe is not the integrated C++ launcher"
[[ -d "$STAGE/bin/Sys/GC" ]] || die "Sys must be staged specifically as bin/Sys"
[[ -s "$STAGE/module-src/CMakeLists.txt" ]] || die "module sources missing"
[[ -s "$STAGE/shaders/crt.glsl" ]] || die "shaders missing"
[[ -s "$STAGE/userdata/GameSettings/GRSEAF.ini" ]] || die "game settings missing"
if awk '
  /^\[(ActionReplay|Gecko)_Enabled\]$/ { enabled=1; next }
  /^\[/ { enabled=0 }
  enabled && /^\$/ { found=1 }
  END { exit found ? 0 : 1 }
' "$STAGE/userdata/GameSettings/GRSEAF.ini"; then
  die "package would enable cheat codes by default"
fi

if find "$STAGE" -type l -print -quit | grep -q .; then
  find "$STAGE" -type l -print >&2
  die "symbolic links are not portable in a Windows Explorer ZIP"
fi

mapfile -d '' forbidden_files < <(find "$STAGE" -type f \( \
  -iname '*.iso' -o -iname '*.wbfs' -o -iname '*.gcm' -o -iname '*.rvz' -o \
  -iname '*.gcz' -o -iname '*.dol' -o -iname '*.afs' -o -iname '*.sfd' -o \
  -iname '*.raw' -o -iname '*.gci' -o -iname '*.sav' -o -iname '*.dtm' -o \
  -iname '*.profdata' -o -iname '*_recomp.dll' -o -iname '*_recomp.so' -o \
  -iname 'boot.bin' -o -iname 'main.dol' -o -iname 'apploader.img' -o \
  -iname 'fst.bin' \) -print0)
if ((${#forbidden_files[@]})); then
  printf 'forbidden game/save/generated file: %s\n' "${forbidden_files[@]}" >&2
  die "game-derived or personal data reached the package"
fi
if find "$STAGE" -type d \( -iname game -o -iname work -o -iname private-artifacts \
     -o -iname MemoryCard -o -iname StateSaves \) -print -quit | grep -q .; then
  find "$STAGE" -type d \( -iname game -o -iname work -o -iname private-artifacts \
       -o -iname MemoryCard -o -iname StateSaves \) -print >&2
  die "private/generated directory reached the package"
fi

while IFS= read -r -d '' pe; do
  leaked="$(strings -a "$pe" | grep -E -m1 '(/home/[^/[:space:]]+/|/Users/[^/[:space:]]+/|[A-Za-z]:[/\\]Users[/\\][^/\\[:space:]]+[/\\])' || true)"
  [[ -z "$leaked" ]] || die "developer path leaked into ${pe#$STAGE/}: $leaked"
done < <(find "$STAGE" -maxdepth 2 -type f \( -iname '*.exe' -o -iname '*.dll' \) \
           ! -path "$STAGE/toolchain/*" -print0)

(
  cd "$STAGE"
  find . -type f ! -name MANIFEST.sha256 -print0 | LC_ALL=C sort -z |
    xargs -0 sha256sum >MANIFEST.sha256
)

# ZIP's DOS timestamps carry no timezone. Windows interprets them as local
# time, so storing a just-created commit's UTC wall clock puts every entry up
# to 12 hours in the future west of UTC. Ninja then reruns CMake 100 times
# because the bundled CMake modules are newer than build.ninja. Keep the PE
# reproducibility epoch above, but date archive entries one full day earlier;
# setup.ps1 also normalises future build inputs defensively for clock skew and
# third-party re-zippers.
ZIP_DATE_EPOCH=$((SOURCE_DATE_EPOCH - 86400))
((ZIP_DATE_EPOCH >= 315532800)) || ZIP_DATE_EPOCH=315532800
find "$STAGE" -depth -print0 | xargs -0 touch -h -d "@$ZIP_DATE_EPOCH"

printf '==> deterministic ZIP\n'
TMP_ZIP="$WORK/$ARTIFACT"
(
  cd "$WORK"
  find "$PACKAGE" -print | LC_ALL=C sort | zip -X -q "$TMP_ZIP" -@
)
unzip -tq "$TMP_ZIP" >/dev/null
bad_top="$(unzip -Z1 "$TMP_ZIP" | awk -F/ -v want="$PACKAGE" '$1 != want { print; exit }')"
[[ -z "$bad_top" ]] || die "ZIP contains an unexpected top-level path: $bad_top"

VERIFY="$WORK/verify"
mkdir -p "$VERIFY"
# Simulate extraction in the westernmost timezone. ZIP stores only a local DOS
# wall clock after -X removes extra timestamp fields, so this catches an entry
# that Windows would interpret as future-dated even when the UTC CI runner
# would not.
TZ=Etc/GMT+12 unzip -q "$TMP_ZIP" -d "$VERIFY"
future_entry="$(find "$VERIFY/$PACKAGE" -type f -newermt now -print -quit)"
[[ -z "$future_entry" ]] || die "ZIP has a future-dated entry in UTC-12: $future_entry"
(
  cd "$VERIFY/$PACKAGE"
  sha256sum -c MANIFEST.sha256 >/dev/null
)

TMP_SHA="$WORK/$ARTIFACT.sha256"
artifact_hash="$(sha256sum "$TMP_ZIP" | awk '{print $1}')"
printf '%s  %s\n' "$artifact_hash" "$ARTIFACT" >"$TMP_SHA"
mv -f -- "$TMP_ZIP" "$FINAL_ZIP"
mv -f -- "$TMP_SHA" "$FINAL_SHA"

printf 'OK: %s\n' "$FINAL_ZIP"
printf 'SHA-256: %s\n' "$artifact_hash"
printf 'Payload: %s files; Sys: %s files\n' \
  "$(find "$STAGE" -type f | wc -l)" "$sys_files"
