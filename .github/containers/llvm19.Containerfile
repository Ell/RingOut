# Toolchain for DolRecomp's LLVM backend.
#
# The backend requires LLVM 19 or 20 and hard-fails outside that window, which
# nothing packages conveniently: Debian 12 ships 14, this Arch host has 22, and
# Arch's own repo offers 18. So the version comes from apt.llvm.org rather than
# from whatever the build machine happens to have.
#
# Based on debian:12 to match the Deck build container (glibc 2.36), so a
# recompiler built here behaves like the one used for the shipped module.
FROM debian:12

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release software-properties-common \
        build-essential cmake ninja-build git python3 zlib1g-dev libzstd-dev \
    && wget -qO /usr/share/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.asc] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main" \
       > /etc/apt/sources.list.d/llvm19.list \
    && apt-get update && apt-get install -y --no-install-recommends llvm-19-dev \
    && rm -rf /var/lib/apt/lists/*

ENV LLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm
