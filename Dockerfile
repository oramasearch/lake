# lake — Linux/amd64 image.
#
# Builds the single `lake` binary from the local, fully-checked-out source (the HTTP stack is
# vendored under modules/ at Zetta's pins) and runs it. Same recipe as Zetta's headless image
# (zetta-bridge/build/images/zetta-headless/Dockerfile) minus the GUI and Postgres deps.
#
# Build natively on amd64 — QEMU emulation of a C++26 build on Apple Silicon is far too slow.
# From the lake repo root (context = this repo, submodules checked out):
#   docker buildx build --builder <amd64-builder> --platform linux/amd64 \
#     --build-context zetta_source=<zetta-bridge>/modules/zetta \
#     --build-context liquid_source=<liquid-bridge>/modules/liquid \
#     -t oramasearch/lake:v1-sandbox --push .
#
# The runtime needs OpenSSL 3.6.3 for Argon2id, plus the C++ runtime.

# The image workflow supplies the authorized zetta-bridge checkout as a named context. Only the
# client transport sources are compiled; no Zetta source is copied into this repository.
FROM zetta_source AS zetta-source

# liquid is a sibling module of liquid-bridge, not a submodule of lake, so it arrives as a
# named context too rather than being vendored under modules/.
FROM liquid_source AS liquid-source

# The tunnel client binaries Lake hands to Developer Access recipients (lake#745,
# docs/api/tunnel-clients.md): a private scratch image holding /clients/ (zetta-tunnel's
# docs/image.md, "Tunnel clients for developers"), pinned by digest. The build pulls it with the
# same registry credential as the base images.
FROM docker.io/oramasearch/zetta-tunnel-clients:v0.1.0@sha256:a9f609689ed5536d0dbafb1b7e165c3389e1053bf3dc2c44aa9e74a7cf2a7f00 AS tunnel-clients

# ---------- web app ----------
# The web app (frontend/, docs/ui/web-app.md) is built with Node 22 and embedded into the binary
# by the build stage below.
FROM node:22-bookworm-slim AS frontend
WORKDIR /frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY frontend/ ./
# Anything else the tests or scripts read must be copied here; `npm test` names what is missing
# (frontend/scripts/watch-copied-reads.mjs, lake#693).
# The icon test checks the website's exported copy (lake#639), which lives beside frontend/.
COPY assets/catalogue-icons/ /assets/catalogue-icons/
# The agents test checks the vendored skills against the engine's create_agent schema (lake#653).
COPY src/api/engine_openapi.json /src/api/engine_openapi.json
# The SQL keywords test checks the web app's list against the server's (lake#814).
COPY src/api/datasets.cpp /src/api/datasets.cpp
RUN npm run build

# ---------- build stage ----------
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive

# Clang 17+ is required for C++26; install clang-19 from the official LLVM apt channel.
# libstdc++-14 is the C++ standard library (Ubuntu 24.04 defaults to 13, which lacks <print>);
# clang picks the newest libstdc++ installed. OpenSSL 3.6.3 supplies both Corosio TLS and the
# Argon2id EVP_KDF used by password credentials.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg lsb-release software-properties-common \
      build-essential ninja-build git pkg-config libfuse3-dev fuse3 nodejs ccache \
      libstdc++-14-dev zlib1g-dev libssl-dev libpq-dev \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 19 \
 && rm -rf /var/lib/apt/lists/*

ARG OPENSSL_VERSION=3.6.3
ARG OPENSSL_SHA256=243a86649cf6f23eeb6a2ff2456e09e5d77dd9018a54d3d96b0c6bdd6ba6c7f1
ENV LD_LIBRARY_PATH=/opt/openssl/lib64
ENV OPENSSL_MODULES=/opt/openssl/lib64/ossl-modules
RUN wget -qO /tmp/openssl.tar.gz \
      "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" \
 && echo "${OPENSSL_SHA256}  /tmp/openssl.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/openssl.tar.gz -C /tmp \
 && cd "/tmp/openssl-${OPENSSL_VERSION}" \
 && ./Configure linux-x86_64 --prefix=/opt/openssl --libdir=lib64 \
      --openssldir=/etc/ssl shared no-tests \
 && make -j "$(nproc)" \
 && make install_sw \
 && /opt/openssl/bin/openssl version \
 && rm -rf /tmp/openssl.tar.gz "/tmp/openssl-${OPENSSL_VERSION}"

# Ubuntu 24.04 ships CMake 3.28, which does NOT know cxx_std_26 for Clang 19. Use a newer CMake.
ARG CMAKE_VERSION=3.31.5
RUN wget -qO /tmp/cmake.sh "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
 && sh /tmp/cmake.sh --skip-license --prefix=/usr/local && rm -f /tmp/cmake.sh && cmake --version

WORKDIR /src
# Submodules (capy, corosio, http, beast2) are already checked out locally — no git/SSH in the build.
COPY . /src
COPY --from=zetta-source . /opt/zetta
COPY --from=liquid-source . /opt/liquid
COPY --from=frontend /frontend/dist /opt/frontend-dist

# Tests off (skips the GoogleTest fetch and the test targets). FetchContent pulls Boost 1.91 into
# build/_deps at configure time (needs network). Build uses all cores.
ARG LAKE_ZETTA_SOURCE_DIR=/opt/zetta
ARG LAKE_LIQUID_SOURCE_DIR=/opt/liquid
ARG LAKE_BUILD_REVISION=unknown
# ccache keeps every object file in a BuildKit cache mount that outlives the image, so a later
# build recompiles only the sources that changed instead of all of lake, libliquid and the vendored
# stack (docs/build/container-image.md). The source always sits at /src, so cache keys match.
ENV CCACHE_DIR=/root/.cache/ccache CCACHE_MAXSIZE=8G CCACHE_COMPILERCHECK=content
RUN --mount=type=cache,target=/root/.cache/ccache,id=lake-ccache \
    cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DLAKE_BUILD_TESTS=OFF \
      -DLAKE_ZETTA_SOURCE_DIR="${LAKE_ZETTA_SOURCE_DIR}" \
      -DLAKE_LIQUID_SOURCE_DIR="${LAKE_LIQUID_SOURCE_DIR}" \
      -DLAKE_BUILD_REVISION="${LAKE_BUILD_REVISION}" \
      -DLAKE_FRONTEND_DIST=/opt/frontend-dist \
      -DCMAKE_C_COMPILER=clang-19 \
      -DCMAKE_CXX_COMPILER=clang++-19 \
      -DOPENSSL_ROOT_DIR=/opt/openssl \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 && cmake --build build --target lake -j "$(nproc)" \
 && ccache --show-stats \
 && { strip build/lake || true; } \
 && ./build/lake version

# ---------- runtime stage ----------
FROM ubuntu:24.04 AS runtime
ARG LAKE_BUILD_REVISION=unknown
ARG LAKE_BUILD_CREATED=unknown
LABEL org.opencontainers.image.revision="${LAKE_BUILD_REVISION}" \
      org.opencontainers.image.created="${LAKE_BUILD_CREATED}"
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 libstdc++6 libpq5 libfuse3-3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m -u 10001 -s /usr/sbin/nologin lake

COPY --from=build /opt/openssl/lib64/libcrypto.so.3 /opt/openssl/lib64/
COPY --from=build /opt/openssl/lib64/libssl.so.3 /opt/openssl/lib64/
COPY --from=build /opt/openssl/lib64/ossl-modules /opt/openssl/lib64/ossl-modules
COPY --from=build /src/build/lake /usr/local/bin/lake
# Served by GET /tunnel-clients/<platform> (LAKE_TUNNEL_CLIENTS_DIR's default).
COPY --from=tunnel-clients /clients/ /usr/share/lake/tunnel-clients/
ENV LD_LIBRARY_PATH=/opt/openssl/lib64
ENV OPENSSL_MODULES=/opt/openssl/lib64/ossl-modules

# Numeric so a runAsNonRoot pod security context can verify it (a named USER cannot be).
USER 10001:10001
WORKDIR /home/lake
# 4900 = the lake HTTP port (web UI + /v1 REST).
EXPOSE 4900
ENTRYPOINT ["lake"]
CMD ["--listen", "0.0.0.0:4900"]
