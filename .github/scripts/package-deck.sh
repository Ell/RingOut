#!/bin/bash
# Assembles the Steam Deck / SteamOS release: the runtime built against an old
# glibc, the support libraries that go with it, and the scaffolding -- zipped.
#
# This package ships NO game data, NO save data and NO recompiled module. The
# module is derived from the user's own disc and cannot be redistributed, so
# they build it once on a desktop and copy game/ and bin/g<ID>_recomp.so across
# (README.txt says so, and the launcher checks for both). That is why the stage
# is built file by file from an allowlist rather than by copying
# dist/RingOut-1.0-deck: the working copy of that directory on a developer
# machine holds ~1 GB of extracted disc, the player's memory card and the
# module, and a recursive copy would quietly ship all three.
#
#   ./.github/scripts/package-deck.sh
#
# Env:
#   RUNTIME=path  package an already-built runtime instead of building one.
#                 It must still be a Debian 12 build -- the floor is checked.
#   OUT=dir       output directory (default dist/)
#   SKIP_BUILD=1  reuse build-deck/moderngekko-run from a previous run
#   NATIVE=1      we are ALREADY on Debian 12, so run the inspection steps here
#                 instead of in a container. This is the CI path: the workflow
#                 job runs in a debian:12 container and has no podman. Do not
#                 use it on a developer machine -- ldd would then resolve the
#                 HOST's libraries, and a package of Arch libraries has a glibc
#                 floor far above SteamOS and would not start on a Deck.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="ringout-deck-build"
OUT="${OUT:-$REPO/dist}"
PKG="RingOut-1.0-deck"
SRC="$REPO/dist/$PKG"

# Staging happens in its own directory. Never stage into dist/RingOut-1.0-deck:
# that is the live working package, and this script would delete it.
WORK="$OUT/_deck-stage"
STAGE="$WORK/$PKG"
ZIP="$OUT/RingOut-1.0-steamdeck-x86_64.zip"

case "$STAGE" in
  "$SRC"|"$SRC"/*) echo "refusing to stage into the working package ($SRC)" >&2; exit 1 ;;
esac

# Run a snippet where Debian 12's linker and libraries are the ones in scope.
# The container mounts are at IDENTICAL paths, so a snippet reads the same
# either way and nothing has to translate /src-style prefixes.
box() {
  if [ "${NATIVE:-0}" = "1" ]; then
    bash -c "$1"
  else
    podman run --rm --userns=keep-id \
      -v "$REPO:$REPO:Z" -v "$WORK:$WORK:Z" -w "$REPO" "$IMAGE" bash -c "$1"
  fi
}

# --- the runtime ----------------------------------------------------------
RUNTIME="${RUNTIME:-$REPO/build-deck/moderngekko-run}"
if [ ! -f "$RUNTIME" ] && [ "${SKIP_BUILD:-0}" != "1" ]; then
  echo "==> building the runtime (no $RUNTIME yet)"
  "$REPO/.github/scripts/build-deck.sh"
fi
[ -f "$RUNTIME" ] || { echo "no runtime at $RUNTIME" >&2; exit 1; }
echo "==> runtime: $RUNTIME"

rm -rf "$WORK"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/shaders"

# --- support libraries ----------------------------------------------------
# Computed, not hand-maintained. The rule is the runtime's ldd closure minus
# the three the target OS must always provide itself: libc, libm and libpthread
# are the C library, and a Debian 12 copy of those cannot be dropped onto
# SteamOS. Everything else is fair game because the LAUNCHER decides at run time
# which of these to actually use -- it asks ldconfig what the host already has
# and links only the remainder into userdata/lib-fallback. That indirection is
# load-bearing: putting all 66 on LD_LIBRARY_PATH shadowed SteamOS's own copies
# for the whole process, including the ones Mesa dlopens, and no Vulkan driver
# would load.
echo "==> collecting support libraries"
box '
    set -e
    ldd "'"$RUNTIME"'" | awk "/=>/ && \$3 ~ /^\// {print \$1, \$3}" |
    while read -r soname path; do
      case "$soname" in
        libc.so.6|libm.so.6|libpthread.so.0) continue ;;
      esac
      # -L because the closure names sonames but the files are usually symlinks
      # to a versioned real name; the package wants the real content under the
      # soname the loader will ask for.
      cp -L "$path" "'"$STAGE"'/lib/$soname"
    done
  '
echo "    $(ls "$STAGE/lib" | wc -l) libraries"

# --- binaries and scaffolding --------------------------------------------
echo "==> scaffolding"
install -m 755 "$RUNTIME"        "$STAGE/bin/moderngekko-run"
install -m 755 "$SRC/RingOut"    "$STAGE/RingOut"
install -m 644 "$SRC/README.txt" "$STAGE/README.txt"
install -m 644 "$SRC/CREDITS.txt" "$STAGE/CREDITS.txt"
for f in "$SRC"/shaders/*.glsl; do install -m 644 "$f" "$STAGE/shaders/"; done

# --- checks ---------------------------------------------------------------
# A package that ships the disc, the save card or the module is the failure
# that matters here, so assert it rather than trusting the allowlist above.
echo "==> checks"
for forbidden in game userdata bin/gGRSEAF_recomp.so; do
  [ -e "$STAGE/$forbidden" ] && { echo "  FAIL: $forbidden is in the stage" >&2; exit 1; }
done
if find "$STAGE" -name '*_recomp.so' -o -name '*.gci' -o -name '*.iso' | grep -q .; then
  echo "  FAIL: disc-derived files in the stage" >&2; exit 1
fi

floor=$(box 'objdump -T "'"$STAGE"'/bin/moderngekko-run" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1')
echo "  glibc floor: $floor"
case "$floor" in
  GLIBC_2.3[0-6]|GLIBC_2.2*|GLIBC_2.1*) ;;
  *) echo "  FAIL: $floor is above SteamOS's ~2.37 -- this will not start on a Deck" >&2; exit 1 ;;
esac

# --- zip ------------------------------------------------------------------
# Info-ZIP stores unix modes and unzip restores them. That matters more than it
# looks: RingOut and bin/moderngekko-run arrive non-executable otherwise and the
# package fails with "permission denied" on a player's Deck, so the unpack below
# is a canary rather than a formality.
echo "==> zipping"
rm -f "$ZIP"
( cd "$WORK" && zip -qr "$ZIP" "$PKG" )

verify="$WORK/_verify"
mkdir -p "$verify"
unzip -qq "$ZIP" -d "$verify"
[ -x "$verify/$PKG/RingOut" ] || { echo "  FAIL: RingOut is not executable after unzip" >&2; exit 1; }
[ -x "$verify/$PKG/bin/moderngekko-run" ] || { echo "  FAIL: runtime is not executable after unzip" >&2; exit 1; }
rm -rf "$verify"

echo
echo "$(basename "$ZIP")  $(du -h "$ZIP" | cut -f1)"
( cd "$STAGE" && du -sh -- * | sort -h )
