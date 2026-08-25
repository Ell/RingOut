FROM debian:12@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443

COPY .github/scripts/deck-deps.txt /tmp/ringout-deck-deps.txt

RUN set -eu; \
    apt-get update; \
    deps="$(grep -v '^#' /tmp/ringout-deck-deps.txt | grep -v '^[[:space:]]*$')"; \
    apt-get install -y --no-install-recommends \
      $deps \
      binutils \
      curl \
      file \
      gh \
      squashfs-tools \
      zstd \
      zlib1g-dev; \
    rm -rf /var/lib/apt/lists/* /tmp/ringout-deck-deps.txt

# Consumed by package-appimage.sh so a direct invocation cannot accidentally
# inventory host/Ubuntu libraries while claiming this Debian base provenance.
ENV RINGOUT_APPIMAGE_BUILD_BASE="debian:12@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443"
