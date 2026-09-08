FROM ubuntu:26.04@sha256:2260313b31c8c011cd2eebe728008efac1b3982be73eb71348ea2648d2c0e09b
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git ca-certificates \
      libpq-dev libjemalloc-dev \
      libjsoncpp-dev libssl-dev zlib1g-dev \
      uuid-dev libargon2-dev \
      clang-21 llvm-21 clang-format-21 clang-tidy-21 clang-tools-21 libclang-rt-21-dev \
      python3 postgresql-client \
      nodejs npm \
    && rm -rf /var/lib/apt/lists/*

# Project CMake builds its pinned, locally patched Drogon through FetchContent.
# A preinstalled unpatched library is unused and cannot validate that build.

LABEL org.opencontainers.image.description="Plinth CI builder image (Ubuntu 26.04 + GCC 15 + LLVM/Clang 21 + build dependencies)"
