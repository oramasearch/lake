# lake — Linux/amd64 image.
#
# Builds the single `lake` binary from the local, fully-checked-out source (the HTTP stack is
# vendored under modules/ at Zetta's pins) and runs it. Same recipe as Zetta's headless image
# (zetta-bridge/build/images/zetta-headless/Dockerfile) minus the GUI and Postgres deps.
#
# Build natively on amd64 — QEMU emulation of a C++26 build on Apple Silicon is far too slow.
# From the lake repo root (context = this repo, submodules checked out):
#   docker buildx build --builder <amd64-builder> --platform linux/amd64 \
#     -t oramasearch/lake:v1-sandbox --push .
#
# The runtime needs only OpenSSL 3 + the C++ runtime.

# ---------- build stage ----------
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive

# Clang 17+ is required for C++26; install clang-19 from the official LLVM apt channel.
# libstdc++-14 is the C++ standard library (Ubuntu 24.04 defaults to 13, which lacks <print>);
# clang picks the newest libstdc++ installed. OpenSSL 3 is required by corosio's TLS layer.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg lsb-release software-properties-common \
      build-essential ninja-build git \
      libstdc++-14-dev zlib1g-dev libssl-dev libpq-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 19 \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 ships CMake 3.28, which does NOT know cxx_std_26 for Clang 19. Use a newer CMake.
ARG CMAKE_VERSION=3.31.5
RUN wget -qO /tmp/cmake.sh "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
 && sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm -f /tmp/cmake.sh && cmake --version

WORKDIR /src
# Submodules (capy, corosio, http, beast2) are already checked out locally — no git/SSH in the build.
COPY . /src

# Tests off (skips the GoogleTest fetch and the test targets). FetchContent pulls Boost 1.91 into
# build/_deps at configure time (needs network). Build uses all cores.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLAKE_BUILD_TESTS=OFF \
      -DCMAKE_C_COMPILER=clang-19 \
      -DCMAKE_CXX_COMPILER=clang++-19 \
 && cmake --build build --target lake -j "$(nproc)" \
 && { strip build/lake || true; } \
 && ./build/lake version

# ---------- runtime stage ----------
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 libstdc++6 libpq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 10001 -s /usr/sbin/nologin lake

COPY --from=build /src/build/lake /usr/local/bin/lake

USER lake
WORKDIR /home/lake
# 4900 = the lake HTTP port (web UI + /v1 REST).
EXPOSE 4900
ENTRYPOINT ["lake"]
CMD ["--listen", "0.0.0.0:4900"]
