#!/usr/bin/env bash
# Prove that the exact AppImage payload can turn a project-authored synthetic
# DOL into a native module and that the result is loadable through its ABI.
set -euo pipefail

APPDIR="${1:?usage: smoke-appimage-module.sh <extracted-AppDir> <work-dir> <module-info>}"
WORK="${2:?usage: smoke-appimage-module.sh <extracted-AppDir> <work-dir> <module-info>}"
MODULE_INFO="${3:?usage: smoke-appimage-module.sh <extracted-AppDir> <work-dir> <module-info>}"
APPDIR="$(realpath "$APPDIR")"
WORK="$(realpath -m "$WORK")"
MODULE_INFO="$(realpath "$MODULE_INFO")"
PAYLOAD="$APPDIR/usr/share/ringout"

for command_name in cmake ninja python3; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "module smoke: required command missing: $command_name" >&2
    exit 1
  }
done
[[ -x "$APPDIR/usr/bin/dolrecomp" ]] || {
  echo "module smoke: packaged dolrecomp is missing" >&2
  exit 1
}
[[ -s "$PAYLOAD/module-src/CMakeLists.txt" ]] || {
  echo "module smoke: packaged module sources are missing" >&2
  exit 1
}
[[ -x "$MODULE_INFO" ]] || {
  echo "module smoke: moderngekko-module-info is missing" >&2
  exit 1
}

mkdir -p "$WORK"
DOL="$WORK/synthetic-legal.dol"
python3 - "$DOL" <<'PY'
import struct
import sys

# Project-authored two-instruction GameCube DOL: li r3,1; blr. This is the same
# minimal legal shape used by DolRecomp's frontend unit test and contains no
# Nintendo or third-party game material.
data = bytearray(0x108)
struct.pack_into(">I", data, 0x00, 0x100)       # text[0] file offset
struct.pack_into(">I", data, 0x48, 0x80003100)  # text[0] load address
struct.pack_into(">I", data, 0x90, 8)           # text[0] byte size
struct.pack_into(">I", data, 0xE0, 0x80003100)  # entry point
struct.pack_into(">II", data, 0x100, 0x38600001, 0x4E800020)
with open(sys.argv[1], "wb") as output:
    output.write(data)
PY

GENERATED_ROOT="$WORK/generated-root"
"$APPDIR/usr/bin/dolrecomp" --gamecube "$DOL" "$GENERATED_ROOT"
GENERATED="$GENERATED_ROOT/generated"
[[ -s "$GENERATED/generated.h" ]] || {
  echo "module smoke: recompiler did not produce generated.h" >&2
  exit 1
}
if grep -R -q 'ppc_fallback_instruction' "$GENERATED/chunks"; then
  echo "module smoke: known synthetic instructions reached fallback" >&2
  exit 1
fi
install -m 600 /dev/null "$GENERATED/generated_smc.txt"
install -m 600 "$DOL" "$GENERATED/main.dol"

MODULE_BUILD="$WORK/module-build"
cmake -S "$PAYLOAD/module-src" -B "$MODULE_BUILD" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=$WORK=." \
  -DGAME_ID=TST001 \
  -DGENERATED_DIR="$GENERATED" \
  -DDOLRECOMP_SRC="$PAYLOAD/module-src/deps/dolrecomp-src" \
  -DGXRUNTIME_INC="$PAYLOAD/module-src/deps/gxruntime-include" \
  -DCHASSIS_ABI_DIR="$PAYLOAD/module-src/deps/chassis-abi" \
  -DMODULE_TEMPLATE="$PAYLOAD/module-src/deps/module-template"
grep -Fxq 'MODULE_LTO:BOOL=ON' "$MODULE_BUILD/CMakeCache.txt" || {
  echo "module smoke: shipped default LTO path was not enabled" >&2
  exit 1
}
grep -Fq -- '-flto=auto' "$MODULE_BUILD/build.ninja" || {
  echo "module smoke: Debian GCC default LTO flag was not generated" >&2
  exit 1
}
cmake --build "$MODULE_BUILD" --parallel "$(nproc)"

MODULE="$MODULE_BUILD/gTST001_recomp.so"
[[ -s "$MODULE" ]] || {
  echo "module smoke: native module was not produced" >&2
  exit 1
}
file "$MODULE" | grep -q 'ELF 64-bit.*x86-64' || {
  echo "module smoke: native module is not an x86-64 ELF shared object" >&2
  exit 1
}
if ldd "$MODULE" 2>&1 | grep -q 'not found'; then
  ldd "$MODULE" >&2 || true
  echo "module smoke: generated module has unresolved dependencies" >&2
  exit 1
fi

python3 - "$MODULE" <<'PY'
import ctypes
import sys

class DescriptorPrefix(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("cpu_abi_version", ctypes.c_uint32),
        ("cpu_state_size", ctypes.c_uint32),
        ("game_id", ctypes.c_char * 8),
        ("entry_point", ctypes.c_uint32),
    ]

module = ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)
entry = module.staticrecomp_get_module
entry.restype = ctypes.POINTER(DescriptorPrefix)
descriptor = entry().contents
assert descriptor.abi_version == 3, descriptor.abi_version
assert descriptor.cpu_abi_version > 0, descriptor.cpu_abi_version
assert descriptor.cpu_state_size > 0, descriptor.cpu_state_size
assert descriptor.game_id.rstrip(b"\0") == b"TST001", descriptor.game_id
assert descriptor.entry_point == 0x80003100, hex(descriptor.entry_point)
PY

"$MODULE_INFO" "$MODULE" TST001 | tee "$WORK/module-info.txt"
for expected in \
  'game_id=TST001' \
  'module_abi=3' \
  'cpu_abi=3' \
  'entry_point=0x80003100' \
  'code_ranges=1' \
  'smc_ranges=0' \
  'chunk_ranges=1'; do
  grep -Fxq "$expected" "$WORK/module-info.txt" || {
    echo "module smoke: descriptor is missing: $expected" >&2
    exit 1
  }
done

echo "AppImage synthetic DOL -> native module -> dlopen/ABI smoke passed"
