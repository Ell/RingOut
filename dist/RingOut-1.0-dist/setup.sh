#!/bin/bash
# Ring Out - Ver 1.0 : first-time setup
#
# Builds your personal copy from a GameCube disc image you already have. Nothing
# game-derived ships with this package -- this script extracts the disc and
# recompiles its executable here, on your machine.
#
# Usage:  ./setup.sh /path/to/your/disc.iso
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
DEPS="$HERE/module-src/deps"
ISO="${1:-}"

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    echo "Usage: ./setup.sh /path/to/your/disc.iso"
    echo
    echo "Supply a GameCube disc image you already have."
    exit 1
fi

missing=""
for tool in cmake ninja python3; do
    command -v "$tool" >/dev/null || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "Missing required build tools:$missing"
    echo "Install them and re-run. On Arch:  sudo pacman -S cmake ninja clang python"
    echo "On Debian/Ubuntu:  sudo apt install cmake ninja-build clang python3"
    exit 1
fi

# A compiler being PRESENT is not the same as a compiler that WORKS. SteamOS
# ships clang but not the C library headers, so the old "command -v clang"
# check passed and the build then died on <string.h> after several minutes of
# extracting and recompiling. Actually compile something first, and pick the
# first toolchain that can.
CC=""
_probe="$(mktemp -d)"
cat > "$_probe/probe.c" <<'EOF'
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) { return (int)strlen(""); }
EOF
for candidate in clang gcc cc; do
    command -v "$candidate" >/dev/null || continue
    if "$candidate" "$_probe/probe.c" -o "$_probe/probe" >/dev/null 2>&1; then
        CC="$(command -v "$candidate")"
        break
    fi
done
rm -rf "$_probe"

if [ -z "$CC" ]; then
    echo
    echo "A C compiler is installed but cannot compile a trivial program --"
    echo "the C standard library headers (string.h, stdio.h) are missing."
    echo
    echo "This is normal on SteamOS / Steam Deck: the system image ships the"
    echo "compilers but strips the development headers, and /usr is read-only."
    echo
    echo "Options on SteamOS:"
    echo "  1. Build inside a container (recommended -- survives OS updates):"
    echo "       distrobox create -n ringout -i archlinux:latest"
    echo "       distrobox enter ringout"
    echo "       sudo pacman -S --needed base-devel cmake ninja clang python"
    echo "       cd \"$HERE\" && ./setup.sh <your-disc-image>"
    echo
    echo "  2. Or unlock the system image (undone by every SteamOS update):"
    echo "       sudo steamos-readonly disable"
    echo "       sudo pacman-key --init && sudo pacman-key --populate archlinux"
    echo "       sudo pacman -S --overwrite '*' glibc linux-api-headers"
    echo
    echo "On other distros, install the libc development package:"
    echo "  Arch: sudo pacman -S glibc   Debian/Ubuntu: sudo apt install libc6-dev"
    exit 1
fi
echo "Using C compiler: $CC"

echo "==> 1/3  Extracting disc"
rm -rf "$HERE/game"
"$HERE/tools/dolrecomp" extract "$ISO" "$HERE/game"

DISC_ID="$(head -c 6 "$HERE/game/sys/boot.bin" 2>/dev/null || true)"
if [ -z "$DISC_ID" ]; then
    echo "Could not read a disc ID -- is that a GameCube disc image?"
    exit 1
fi
echo "    disc id: $DISC_ID"

echo "==> 2/3  Recompiling the game executable (several minutes)"
rm -rf "$HERE/work"
mkdir -p "$HERE/work"
"$HERE/tools/dolrecomp" --gamecube "$HERE/game/sys/main.dol" -j"$(nproc)" "$HERE/work/out"
# gen_module_tables.py reads main.dol from alongside the generated sources.
cp "$HERE/game/sys/main.dol" "$HERE/work/out/generated/main.dol"

echo "==> 3/3  Building the module"
cmake -S "$HERE/module-src" -B "$HERE/work/build" -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_C_FLAGS="-march=native" \
      -DGAME_ID="$DISC_ID" \
      -DGENERATED_DIR="$HERE/work/out/generated" \
      -DDOLRECOMP_SRC="$DEPS/dolrecomp-src" \
      -DGXRUNTIME_INC="$DEPS/gxruntime-include" \
      -DCHASSIS_ABI_DIR="$DEPS/chassis-abi" \
      -DMODULE_TEMPLATE="$DEPS/module-template"
cmake --build "$HERE/work/build"

cp "$HERE/work/build/g${DISC_ID}_recomp.so" "$HERE/bin/"

echo
echo "Setup complete. Run ./RingOut to play."
