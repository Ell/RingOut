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

EXTRACT="$WORK/extracted package"
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
LAUNCHER="$PACKAGE/RingOut.exe"
PORT="$PACKAGE/tools/moderngekko-port.exe"
RUNTIME="$PACKAGE/bin/moderngekko-run.exe"
CLANG="$PACKAGE/toolchain/bin/clang.exe"
LLD="$PACKAGE/toolchain/bin/ld.lld.exe"
CMAKE="$PACKAGE/toolchain/bin/cmake.exe"
NINJA="$PACKAGE/toolchain/bin/ninja.exe"
PYTHON="$PACKAGE/toolchain/python/python.exe"
MODULE_SOURCE="$PACKAGE/module-src"
for required in "$LAUNCHER" "$RUNTIME" "$PORT" "$DOLRECOMP" "$CLANG" "$LLD" "$CMAKE" "$NINJA" "$PYTHON" \
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
launcher_help="$WORK/launcher-self-test.txt"
"${WINE[@]}" "$LAUNCHER" --ringout-self-test 2>&1 | tee "$launcher_help"
grep -Fq 'RingOut C++ launcher self-test' "$launcher_help" || \
  die "top-level entry point is not the integrated C++ launcher"
grep -Fq 'runner=' "$launcher_help" || \
  die "launcher did not resolve its packaged runner"
port_selftest="$WORK/port-self-test.txt"
"${WINE[@]}" "$PORT" --ringout-self-test 2>&1 | tee "$port_selftest"
grep -Fq 'RingOut setup self-test module_source=' "$port_selftest" || \
  die "setup helper did not resolve its packaged module/config resources"
runtime_help="$WORK/runtime-help.txt"
"${WINE[@]}" "$RUNTIME" --help 2>&1 | tee "$runtime_help"
grep -Fq 'usage: moderngekko-run' "$runtime_help" || \
  die "packaged runtime did not print its usage banner"
grep -Fq -- '--netplay-host' "$runtime_help" || \
  die "packaged runtime help lacks netplay options"

GAME="$WORK/synthetic game"
PORT_OUTPUT="$WORK/port output"
PORT_LOG="$WORK/port-build.log"

printf '==> generating project-authored extracted game with bundled Python\n'
"${WINE[@]}" "$PYTHON" "$(windows_path "$HELPER")" make-game \
  "$(windows_path "$GAME")"
[[ $(wc -c <"$GAME/sys/main.dol") -eq 264 ]] || \
  die "synthetic DOL is not 0x108 bytes"

# Exercise the exact player path rather than invoking each packaged tool
# independently. The package, game, and output paths deliberately contain
# spaces. This catches Windows cmd.exe's leading-quote corruption and proves
# moderngekko-port directly launches sibling DolRecomp plus bundled
# CMake/Ninja/clang/lld/Python through native argument vectors.
printf '==> running packaged setup helper end to end\n'
"${WINE[@]}" "$PORT" build "$(windows_path "$GAME")" \
  --output "$(windows_path "$PORT_OUTPUT")" --setup-progress \
  2>&1 | tee "$PORT_LOG"
for phase in inspect translate configure compile publish; do
  grep -Fq "[ringout-setup] phase=$phase" "$PORT_LOG" || \
    die "packaged setup helper did not reach phase: $phase"
done
grep -Fq 'module: linking with lld' "$PORT_LOG" || \
  die "module configure did not select the bundled lld"

MODULE="$(find "$PORT_OUTPUT" -type f -name 'gGRSEAF_recomp.dll' -print -quit)"
[[ -n "$MODULE" && -s "$MODULE" ]] || die "gGRSEAF_recomp.dll was not produced"
ARTIFACT="$(dirname "$MODULE")"
GENERATED="$ARTIFACT/dolrecomp-output/generated"
MODULE_BUILD="$ARTIFACT/module-build"
[[ -s "$GENERATED/generated.h" ]] || die "DolRecomp did not produce generated.h"
if grep -R -q 'ppc_fallback_instruction' "$GENERATED/chunks"; then
  die "known synthetic instructions reached the fallback"
fi
grep -Fq 'MODULE_LTO:BOOL=ON' "$MODULE_BUILD/CMakeCache.txt" || \
  die "shipped setup path did not enable module LTO"
grep -Fq -- '-flto=thin' "$MODULE_BUILD/build.ninja" || \
  die "shipped setup path did not generate ThinLTO flags"
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
printf 'Windows ZIP packaged setup helper -> DolRecomp -> clang/lld module smoke passed\n'
printf 'Smoke work directory: %s\n' "$WORK"
