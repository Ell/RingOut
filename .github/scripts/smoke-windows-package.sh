#!/usr/bin/env bash
# Exercise the exact portable ZIP under Wine: packaged DolRecomp -> packaged
# Python/CMake/Ninja/clang/lld -> generated Windows recompilation module.
set -euo pipefail

ZIP="${1:?usage: smoke-windows-package.sh <windows-package.zip> [empty-work-dir]}"
REQUESTED_WORK="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HELPER="$SCRIPT_DIR/windows-package-smoke.py"

die() {
  printf 'windows package smoke: %s\n' "$*" >&2
  exit 1
}

for command_name in find grep install nproc realpath sha256sum tee unzip wc \
                    x86_64-w64-mingw32-objdump; do
  command -v "$command_name" >/dev/null 2>&1 || die "required command missing: $command_name"
done
[[ -f "$ZIP" ]] || die "package not found: $ZIP"
[[ -f "$HELPER" ]] || die "Windows helper not found: $HELPER"
ZIP="$(realpath "$ZIP")"

if command -v wine >/dev/null 2>&1; then
  WINE=(wine)
elif command -v wine64 >/dev/null 2>&1; then
  WINE=(wine64)
elif command -v wine-stable >/dev/null 2>&1; then
  WINE=(wine-stable)
elif command -v wine64-stable >/dev/null 2>&1; then
  WINE=(wine64-stable)
else
  die "Wine is required"
fi

if [[ -n "$REQUESTED_WORK" ]]; then
  WORK="$(realpath -m "$REQUESTED_WORK")"
  mkdir -p "$WORK"
  [[ -z "$(find "$WORK" -mindepth 1 -maxdepth 1 -print -quit)" ]] || \
    die "work directory is not empty: $WORK"
else
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/ringout-windows-smoke.XXXXXXXX")"
fi

EXTRACT="$WORK/extracted"
PREFIX="$WORK/wine-prefix"
mkdir -p "$EXTRACT" "$PREFIX"

export WINEPREFIX="$PREFIX"
export WINEARCH=win64
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"

# A standard Wine prefix maps the Unix root as Z:. Avoid depending on the
# optional winepath wrapper, which Ubuntu's minimal wine64 package omits.
windows_path() {
  local absolute
  absolute="$(realpath -m "$1")"
  printf 'Z:%s\n' "${absolute//\//\\}"
}

printf '==> extracting and verifying exact ZIP\n'
unzip -q "$ZIP" -d "$EXTRACT"
shopt -s nullglob
package_roots=("$EXTRACT"/RingOut-*-windows-x86_64)
shopt -u nullglob
[[ ${#package_roots[@]} -eq 1 && -d "${package_roots[0]}" ]] || \
  die "expected one RingOut Windows package root"
PACKAGE="$(realpath "${package_roots[0]}")"
(
  cd "$PACKAGE"
  sha256sum -c MANIFEST.sha256 >/dev/null
)

DOLRECOMP="$PACKAGE/tools/dolrecomp.exe"
RUNTIME="$PACKAGE/bin/moderngekko-run.exe"
CLANG="$PACKAGE/toolchain/bin/clang.exe"
LLD="$PACKAGE/toolchain/bin/ld.lld.exe"
CMAKE="$PACKAGE/toolchain/bin/cmake.exe"
NINJA="$PACKAGE/toolchain/bin/ninja.exe"
PYTHON="$PACKAGE/toolchain/python/python.exe"
MODULE_SOURCE="$PACKAGE/module-src"
for required in "$RUNTIME" "$DOLRECOMP" "$CLANG" "$LLD" "$CMAKE" "$NINJA" "$PYTHON" \
                "$MODULE_SOURCE/CMakeLists.txt"; do
  [[ -s "$required" ]] || die "packaged input missing: ${required#$PACKAGE/}"
done

printf '==> starting packaged Windows tools under Wine\n'
"${WINE[@]}" "$PYTHON" --version
"${WINE[@]}" "$CMAKE" --version
"${WINE[@]}" "$NINJA" --version
"${WINE[@]}" "$CLANG" --version
"${WINE[@]}" "$LLD" --version

# This enters the shipped frontend far enough for the Windows loader to resolve
# its complete ordinary DLL import table (including the ENet/WinSock path), but
# exits before any game data or graphics device is needed. MinGW builds use the
# deliberate no-QWave QoS stub, so qwave.dll is neither imported nor shipped.
printf '==> loading packaged runtime and checking frontend help\n'
runtime_help="$WORK/runtime-help.txt"
"${WINE[@]}" "$RUNTIME" --help 2>&1 | tee "$runtime_help"
grep -Fq 'usage: moderngekko-run' "$runtime_help" || \
  die "packaged runtime did not print its usage banner"
grep -Fq -- '--netplay-host' "$runtime_help" || \
  die "packaged runtime help lacks netplay options"

DOL="$WORK/synthetic-legal.dol"
GENERATED_ROOT="$WORK/generated-root"
GENERATED="$GENERATED_ROOT/generated"
MODULE_BUILD="$WORK/module-build"
LINK_CACHE="$WORK/thinlto-cache"

printf '==> generating project-authored 0x108-byte DOL with bundled Python\n'
"${WINE[@]}" "$PYTHON" "$(windows_path "$HELPER")" make-dol "$(windows_path "$DOL")"
[[ $(wc -c <"$DOL") -eq 264 ]] || die "synthetic DOL is not 0x108 bytes"

printf '==> running bundled Windows DolRecomp\n'
"${WINE[@]}" "$DOLRECOMP" --gamecube "$(windows_path "$DOL")" \
  "-j$(nproc)" "$(windows_path "$GENERATED_ROOT")"
[[ -s "$GENERATED/generated.h" ]] || die "DolRecomp did not produce generated.h"
if grep -R -q 'ppc_fallback_instruction' "$GENERATED/chunks"; then
  die "known synthetic instructions reached the fallback"
fi
install -m 600 /dev/null "$GENERATED/generated_smc.txt"
install -m 600 "$DOL" "$GENERATED/main.dol"

printf '==> configuring module with packaged CMake/clang/lld/Ninja/Python\n'
configure_log="$WORK/configure.log"
"${WINE[@]}" "$CMAKE" \
  -S "$(windows_path "$MODULE_SOURCE")" \
  -B "$(windows_path "$MODULE_BUILD")" \
  -GNinja \
  "-DCMAKE_MAKE_PROGRAM=$(windows_path "$NINJA")" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_COMPILER=$(windows_path "$CLANG")" \
  "-DPython3_EXECUTABLE=$(windows_path "$PYTHON")" \
  -DMODULE_LTO=ON \
  -DMODULE_LLD=ON \
  "-DMODULE_LINK_CACHE=$(windows_path "$LINK_CACHE")" \
  -DGAME_ID=TST001 \
  "-DGENERATED_DIR=$(windows_path "$GENERATED")" \
  "-DDOLRECOMP_SRC=$(windows_path "$MODULE_SOURCE/deps/dolrecomp-src")" \
  "-DGXRUNTIME_INC=$(windows_path "$MODULE_SOURCE/deps/gxruntime-include")" \
  "-DCHASSIS_ABI_DIR=$(windows_path "$MODULE_SOURCE/deps/chassis-abi")" \
  "-DMODULE_TEMPLATE=$(windows_path "$MODULE_SOURCE/deps/module-template")" \
  2>&1 | tee "$configure_log"
grep -Fq 'module: linking with lld' "$configure_log" || \
  die "module configure did not select the bundled lld"
grep -Eq 'module: ThinLTO cache at .*thinlto-cache' "$configure_log" || \
  die "module configure did not select the temporary ThinLTO cache"

printf '==> building generated Windows module\n'
build_log="$WORK/build.log"
"${WINE[@]}" "$CMAKE" --build "$(windows_path "$MODULE_BUILD")" \
  --parallel "$(nproc)" --verbose 2>&1 | tee "$build_log"
grep -Eq -- '-fuse-ld=lld|ld\.lld' "$build_log" || \
  die "module link command did not exercise lld"
[[ -n "$(find "$LINK_CACHE" -mindepth 1 -type f -print -quit)" ]] || \
  die "lld did not populate the temporary ThinLTO cache"

MODULE="$MODULE_BUILD/gTST001_recomp.dll"
[[ -s "$MODULE" ]] || die "gTST001_recomp.dll was not produced"
pe_report="$WORK/module-pe.txt"
x86_64-w64-mingw32-objdump -p "$MODULE" | tee "$pe_report" >/dev/null
grep -Fq 'staticrecomp_get_module' "$pe_report" || \
  die "module is missing staticrecomp_get_module export"
grep -Fq 'ppc_set_gather_pipe' "$pe_report" || \
  die "module is missing ppc_set_gather_pipe export"

printf '==> loading module and checking ABI through bundled Windows Python\n'
"${WINE[@]}" "$PYTHON" "$(windows_path "$HELPER")" check-module \
  "$(windows_path "$MODULE")"

if command -v wineserver >/dev/null 2>&1; then
  wineserver -w
elif command -v wineserver-stable >/dev/null 2>&1; then
  wineserver-stable -w
fi
printf 'Windows ZIP synthetic DOL -> DolRecomp -> clang/lld module smoke passed\n'
printf 'Smoke work directory: %s\n' "$WORK"
