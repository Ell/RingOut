#!/bin/bash
# Assembles the Linux DESKTOP release: the runtime, the recompiler, the module
# sources and the GPL source shipment -- zipped. No game data.
#
#   ./.github/scripts/package-dist.sh
#
# Env:
#   OUT=dir       output directory (default dist/)
#   SKIP_SOURCE=1 stage without source/ (for a local test build ONLY -- a
#                 published package without it breaks the GPL offer)
#
# WHY THIS EXISTS. The Deck and Windows releases had packaging scripts; this one
# did not, and dist/RingOut-1.0-dist was hand-assembled. That directory is the
# WORKING copy: 1.3 GB, of which 989 MB is the extracted disc, 185 MB is build
# output under work/, and userdata/ holds the developer's own Dolphin config
# (including a netplay host address on the local network), logs, and a memory
# card. Zipping it wholesale ships all three. Every other packaging script here
# stages file by file from an allowlist for exactly that reason, so this one
# does too -- and then asserts the invariants rather than trusting the list.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT:-$REPO/dist}"
PKG="RingOut-1.0-dist"
SRC="$REPO/dist/$PKG"
WORK="$OUT/_dist-stage"
STAGE="$WORK/RingOut-1.0-linux"
ZIP="$OUT/RingOut-1.0-linux-x86_64.zip"

[ -d "$SRC" ] || { echo "no working package at $SRC" >&2; exit 1; }

# Never stage inside the working package: this script deletes its stage.
case "$STAGE" in
  "$SRC"|"$SRC"/*) echo "refusing to stage into the working package ($SRC)" >&2; exit 1 ;;
esac

rm -rf "$WORK"
mkdir -p "$STAGE"

echo "==> scaffolding"
# The launcher and setup.sh between them define what has to be present:
# setup.sh calls tools/dolrecomp and builds module-src into work/; RingOut reads
# lib/, libc-fallback/, shaders/ and userdata/. Anything not on this list is
# either derived from the user's disc or is the developer's own state.
install -m 755 "$SRC/setup.sh"    "$STAGE/setup.sh"
install -m 755 "$SRC/RingOut"     "$STAGE/RingOut"
install -m 644 "$SRC/README.txt"  "$STAGE/README.txt"
install -m 644 "$SRC/CREDITS.txt" "$STAGE/CREDITS.txt"

mkdir -p "$STAGE/bin" "$STAGE/tools" "$STAGE/shaders"
install -m 755 "$SRC/bin/moderngekko-run" "$STAGE/bin/moderngekko-run"
install -m 755 "$SRC/tools/dolrecomp"     "$STAGE/tools/dolrecomp"
for f in "$SRC"/shaders/*.glsl; do install -m 644 "$f" "$STAGE/shaders/"; done

# bin/g<ID>_recomp.so is deliberately absent: it is recompiled from the user's
# own disc by setup.sh and cannot be redistributed.

echo "==> support libraries"
cp -a "$SRC/lib"           "$STAGE/lib"
cp -a "$SRC/libc-fallback" "$STAGE/libc-fallback"
echo "    $(ls "$STAGE/lib" | wc -l) libraries, $(ls "$STAGE/libc-fallback" | wc -l) fallback"

echo "==> module sources"
cp -a "$SRC/module-src" "$STAGE/module-src"

# The cheat/game-settings ini is authored by this project (USA Action Replay
# codes; Dolphin ships only the PAL ini), not disc-derived, and the Windows
# package already ships it. Take it from the same canonical location
# package-windows.ps1 uses -- the working package's own userdata/GameSettings is
# empty, so sourcing it from there would silently ship nothing and leave the
# Linux release without the CHEATS tab the Windows one has.
#
# Nothing ELSE from userdata/ goes: the rest is the developer's Dolphin config,
# cache, logs and memory card, and config.ini there names a LAN address.
GAMESETTINGS=""
for cand in "$SRC/userdata/GameSettings/GRSEAF.ini" \
            "$REPO/work/mg_userdir/GameSettings/GRSEAF.ini"; do
  [ -s "$cand" ] && { GAMESETTINGS="$cand"; break; }
done
if [ -n "$GAMESETTINGS" ]; then
  mkdir -p "$STAGE/userdata/GameSettings"
  install -m 644 "$GAMESETTINGS" "$STAGE/userdata/GameSettings/GRSEAF.ini"
  echo "==> game settings: $(basename "$(dirname "$(dirname "$GAMESETTINGS")")")/GameSettings/GRSEAF.ini"
else
  echo "==> game settings: NONE FOUND -- the CHEATS tab will be empty" >&2
fi

if [ "${SKIP_SOURCE:-0}" != "1" ]; then
  echo "==> GPL source shipment"
  [ -d "$SRC/source" ] || { echo "  FAIL: no source/ -- see CREDITS.txt" >&2; exit 1; }
  cp -a "$SRC/source" "$STAGE/source"

  # The offer is to supply the source FOR THE BINARIES SHIPPED. Source older
  # than the runtime it accompanies does not satisfy that, and it is how a
  # developer path from an old working tree survived into a release here.
  # Compare against the BUILT runtime, not the staged copy: install(1) stamps
  # the staged file with the current time, so comparing against that would make
  # this fail unconditionally -- and a check that always fails gets deleted.
  newest_src="$(find "$SRC/source" -type f -printf '%T@\n' | sort -n | tail -1)"
  runtime_ts="$(stat -c %Y "$SRC/bin/moderngekko-run")"
  if [ "${newest_src%.*}" -lt "$runtime_ts" ]; then
    echo "  FAIL: source/ is older than bin/moderngekko-run." >&2
    echo "        Regenerate the tarballs and patches from the built commits," >&2
    echo "        and update the hashes named in CREDITS.txt." >&2
    exit 1
  fi
else
  echo "==> GPL source shipment SKIPPED (SKIP_SOURCE=1) -- do not publish this"
fi

echo "==> checks"
# Assert rather than trust the allowlist: shipping the disc, the save card or
# the module is the failure that matters.
for forbidden in game work windows source/build bin/gGRSEAF_recomp.so \
                 userdata/Config userdata/GC userdata/Logs userdata/config.ini; do
  [ -e "$STAGE/$forbidden" ] && { echo "  FAIL: $forbidden is in the stage" >&2; exit 1; }
done
if find "$STAGE" -name '*_recomp.so' -o -name '*.gci' -o -name '*.iso' | grep -q .; then
  echo "  FAIL: disc-derived files in the stage" >&2; exit 1
fi
echo "  no disc-derived content"

# The privacy axis the checks above do not cover -- home paths, LAN addressing,
# credentials, author emails in the source patches.
"$REPO/.github/scripts/privacy-scan.sh" "$STAGE"

echo "==> zipping"
rm -f "$ZIP"
( cd "$WORK" && zip -qr "$ZIP" "$(basename "$STAGE")" )

echo
echo "$(basename "$ZIP")  $(du -h "$ZIP" | cut -f1)"
du -sh "$STAGE"/* | sort -rh
