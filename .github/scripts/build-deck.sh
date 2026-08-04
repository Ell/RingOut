#!/bin/bash
# Builds the Linux runtime against an OLD glibc, inside a container, so the
# result runs on SteamOS / Steam Deck.
#
# Binaries run FORWARD across glibc versions but never backward. This machine
# is glibc 2.44 and SteamOS is ~2.37, so anything built natively simply will
# not start on the Deck. Debian 12 (glibc 2.36) is below the Deck and below
# essentially every current distro, so one build covers all of them -- and
# replaces the libc-fallback bundling hack, which was never verified on real
# SteamOS and whose predicted failure mode is NSS.
#
# Steam Runtime sniper would be the more obvious base (glibc 2.31) but ships
# gcc 10 / clang 11, far too old for this codebase.
#
# Run from the repo root:  .github/scripts/build-deck.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="ringout-deck-build"

echo "==> building the toolchain image"
podman build -t "$IMAGE" -f - "$REPO" <<'DOCKERFILE'
FROM debian:12

# Dolphin vendors most of its dependencies under Externals/ and falls back to
# them when a system copy is absent, so this list is deliberately the minimum:
# the things that must come from the host because they talk to the display,
# audio or input stack.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        python3 \
        libx11-dev libxrandr-dev libxi-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols \
        libgl1-mesa-dev libegl1-mesa-dev \
        libasound2-dev libpulse-dev \
        libudev-dev libevdev-dev libusb-1.0-0-dev \
        libsystemd-dev libbluetooth-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE

echo
echo "==> configuring"
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" bash -c '
  set -e
  cmake -S ModernGekko -B build-deck -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QT=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_ANALYTICS=OFF \
    -DENABLE_AUTOUPDATE=OFF
'

echo
echo "==> building moderngekko-run"
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" \
  cmake --build build-deck --target moderngekko-run

echo
echo "==> result"
podman run --rm --userns=keep-id -v "$REPO:/src:Z" -w /src "$IMAGE" bash -c '
  echo "glibc required:  $(objdump -T build-deck/moderngekko-run | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
  echo "GLIBCXX required: $(objdump -T build-deck/moderngekko-run | grep -o "GLIBCXX_[0-9.]*" | sort -uV | tail -1)"
  ls -la build-deck/moderngekko-run
'
