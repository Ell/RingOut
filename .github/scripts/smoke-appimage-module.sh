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
GAME="$WORK/game"
mkdir -p "$GAME/sys" "$GAME/files"
python3 - "$DOL" "$GAME/sys/boot.bin" <<'PY'
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

boot = bytearray(0x60)
boot[0:6] = b"GRSEAF"
struct.pack_into(">I", boot, 0x1C, 0xC2339F3D)
boot[0x20:0x20 + len(b"RingOut synthetic module smoke")] = b"RingOut synthetic module smoke"
with open(sys.argv[2], "wb") as output:
    output.write(boot)
PY
install -m 600 "$DOL" "$GAME/sys/main.dol"

# The helper itself starts with the AppImage DSO closure, exactly like a real
# launcher child. Every host tool is wrapped and refuses that contaminated
# library path; moderngekko-port must restore the caller's original value before
# compiler discovery, CMake configure, Ninja, compiler, and Python subprocesses.
HOST_TOOLS="$WORK/host-tools"
HOST_LIBRARY_PATH="$WORK/host-library-path"
mkdir -p "$HOST_TOOLS" "$HOST_LIBRARY_PATH"
export RINGOUT_REAL_CMAKE="$(command -v cmake)"
export RINGOUT_REAL_NINJA="$(command -v ninja)"
export RINGOUT_REAL_CLANG="$(command -v clang || true)"
export RINGOUT_REAL_GCC="$(command -v gcc || true)"
export RINGOUT_REAL_PYTHON3="$(command -v python3)"
cat >"$HOST_TOOLS/host-tool-wrapper" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${LD_LIBRARY_PATH+x}:${LD_LIBRARY_PATH-}" != \
      "x:${RINGOUT_EXPECTED_HOST_LD_LIBRARY_PATH}" ]]; then
  echo "host tool inherited AppImage LD_LIBRARY_PATH: $(basename "$0")" >&2
  exit 86
fi
case "$(basename "$0")" in
  cmake) real=$RINGOUT_REAL_CMAKE ;;
  ninja) real=$RINGOUT_REAL_NINJA ;;
  clang)
    if [[ "${RINGOUT_FORCE_BROKEN_CLANG:-0}" == 1 ]]; then
      echo "synthetic broken clang" >&2
      exit 89
    fi
    real=$RINGOUT_REAL_CLANG
    ;;
  gcc) real=$RINGOUT_REAL_GCC ;;
  python3) real=$RINGOUT_REAL_PYTHON3 ;;
  *) exit 87 ;;
esac
[[ -n "$real" ]] || exit 88
exec "$real" "$@"
SH
chmod 755 "$HOST_TOOLS/host-tool-wrapper"
for tool in cmake ninja clang gcc python3; do
  ln -s host-tool-wrapper "$HOST_TOOLS/$tool"
done

PORT_OUTPUT="$WORK/port-output"
env \
  PATH="$HOST_TOOLS:$PATH" \
  LD_LIBRARY_PATH="$APPDIR/usr/lib" \
  RINGOUT_HOST_LD_LIBRARY_PATH_SET=1 \
  RINGOUT_HOST_LD_LIBRARY_PATH="$HOST_LIBRARY_PATH" \
  RINGOUT_EXPECTED_HOST_LD_LIBRARY_PATH="$HOST_LIBRARY_PATH" \
  RINGOUT_FORCE_BROKEN_CLANG=1 \
  "$APPDIR/usr/bin/moderngekko-port" build "$GAME" \
    --output "$PORT_OUTPUT" --setup-progress

MODULE="$(sed -n '1p' "$PORT_OUTPUT/GRSEAF/active-module.txt")"
[[ -s "$MODULE" ]] || {
  echo "module smoke: setup helper did not publish a native module" >&2
  exit 1
}
ARTIFACT="$(dirname "$MODULE")"
GENERATED="$ARTIFACT/dolrecomp-output/generated"
MODULE_BUILD="$ARTIFACT/module-build"
[[ -s "$GENERATED/generated.h" ]] || {
  echo "module smoke: recompiler did not produce generated.h" >&2
  exit 1
}
if grep -R -q 'ppc_fallback_instruction' "$GENERATED/chunks"; then
  echo "module smoke: known synthetic instructions reached fallback" >&2
  exit 1
fi
grep -Fxq 'MODULE_LTO:BOOL=ON' "$MODULE_BUILD/CMakeCache.txt" || {
  echo "module smoke: shipped default LTO path was not enabled" >&2
  exit 1
}
grep -Eq -- '-flto=(auto|thin)' "$MODULE_BUILD/build.ninja" || {
  echo "module smoke: default LTO flag was not generated" >&2
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
assert descriptor.game_id.rstrip(b"\0") == b"GRSEAF", descriptor.game_id
assert descriptor.entry_point == 0x80003100, hex(descriptor.entry_point)
PY

"$MODULE_INFO" "$MODULE" GRSEAF | tee "$WORK/module-info.txt"
for expected in \
  'game_id=GRSEAF' \
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
