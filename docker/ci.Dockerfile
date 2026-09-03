FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates \
      libpq-dev libjemalloc-dev \
      libjsoncpp-dev libssl-dev zlib1g-dev \
      uuid-dev libargon2-dev \
      clang-20 llvm-20 clang-tidy clang-tools libclang-rt-20-dev \
      python3 \
      nodejs \
    && rm -rf /var/lib/apt/lists/*

# Prebuild Drogon into /usr/local so find_package(Drogon) in CMakeLists.txt
# picks it up and skips the FetchContent path. Keep DROGON_VERSION in sync
# with CMakeLists.txt (GIT_TAG). When bumping, edit this file first and let
# build-ci-image.yml push a new :latest before bumping CMakeLists.txt.
ARG DROGON_VERSION=v1.9.12
RUN git clone --depth 1 --branch ${DROGON_VERSION} --recurse-submodules \
      https://github.com/drogonframework/drogon.git /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build \
         -DCMAKE_BUILD_TYPE=Release \
         -DBUILD_CTL=OFF -DBUILD_EXAMPLES=OFF \
         -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/drogon/build -j"$(nproc)" \
    && cmake --install /tmp/drogon/build \
    && rm -rf /tmp/drogon

LABEL org.opencontainers.image.description="Plinth CI builder image (Ubuntu 25.10 + gcc 15 + clang/clang-tidy/clang-tools/libclang-rt 20 + deps + Drogon ${DROGON_VERSION})"
