FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171 AS debian-snapshot

# The base image records these snapshot timestamps as comments while using the
# mutable Debian mirrors. Make the timestamps authoritative. Snapshot metadata
# and packages remain authenticated by Debian's archive keyring over HTTP.
RUN printf '%s\n' \
      'Types: deb' \
      'URIs: http://snapshot.debian.org/archive/debian/20260824T000000Z' \
      'Suites: bookworm bookworm-updates' \
      'Components: main' \
      'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
      'Check-Valid-Until: no' \
      '' \
      'Types: deb' \
      'URIs: http://snapshot.debian.org/archive/debian-security/20260824T000000Z' \
      'Suites: bookworm-security' \
      'Components: main' \
      'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
      'Check-Valid-Until: no' \
      > /etc/apt/sources.list.d/debian.sources

FROM debian-snapshot AS build-base

# Defaults keep the development Compose path usable without claiming a release
# identity. Release automation supplies the exact VERSION/revision/epoch.
ARG PLINTH_VERSION=0.6.5
ARG PLINTH_REVISION=0000000000000000000000000000000000000000
ARG PLINTH_RELEASE=0
ARG SOURCE_DATE_EPOCH=1787529600
ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential=12.9 \
      ca-certificates=20250419~deb12u1 \
      cmake=3.25.1-1 \
      git=1:2.39.5-0+deb12u3 \
      libargon2-dev=0~20171227-0.3+deb12u1 \
      libjsoncpp-dev=1.9.5-4 \
      libpq-dev=15.19-0+deb12u1 \
      libssl-dev=3.0.20-1~deb12u2 \
      pkg-config=1.8.1-1 \
      postgresql-client-15=15.19-0+deb12u1 \
      python3=3.11.2-1+b1 \
      uuid-dev=2.38.1-5+deb12u3 \
      zlib1g-dev=1:1.2.13.dfsg-1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN test "$(cat VERSION)" = "${PLINTH_VERSION}" \
    && printf '%s\n' "${PLINTH_VERSION}" \
       | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    && printf '%s\n' "${PLINTH_REVISION}" \
       | grep -Eq '^[0-9a-f]{40}$' \
    && case "${PLINTH_RELEASE}" in 0|1) ;; *) exit 1 ;; esac \
    && case "${SOURCE_DATE_EPOCH}" in ''|*[!0-9]*) exit 1 ;; esac \
    && test "${SOURCE_DATE_EPOCH}" -gt 0

FROM build-base AS builder

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_CXX_FLAGS=-ffile-prefix-map=/src=. \
      -DPLINTH_BUILD_REVISION="${PLINTH_REVISION}" \
      -DPLINTH_BUILD_EXACT_TAG="${PLINTH_RELEASE}" \
    && cmake --build build --target plinth --parallel 2 \
    && DESTDIR=/opt/plinth-root cmake --install build --component Runtime

# The complete server suite builds in the same pinned Release environment and
# from the same source as the runtime artifact, but none of its tools enter
# `runtime`.
FROM builder AS server-tests
RUN cmake --build build --target plinth_tests --parallel 2
ENTRYPOINT ["ctest", "--test-dir", "build", "--output-on-failure", "--parallel", "1"]

FROM debian-snapshot AS runtime

ARG PLINTH_VERSION=0.6.5
ARG PLINTH_REVISION=0000000000000000000000000000000000000000
ARG PLINTH_RELEASE=0
ARG SOURCE_DATE_EPOCH=1787529600

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates=20250419~deb12u1 \
      libargon2-1=0~20171227-0.3+deb12u1 \
      libgcc-s1=12.2.0-14+deb12u1 \
      libjsoncpp25=1.9.5-4 \
      libpq5=15.19-0+deb12u1 \
      libssl3=3.0.20-1~deb12u2 \
      libstdc++6=12.2.0-14+deb12u1 \
      libuuid1=2.38.1-5+deb12u3 \
      zlib1g=1:1.2.13.dfsg-1 \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 10001 plinth \
    && useradd --uid 10001 --gid 10001 --home-dir /var/lib/plinth \
       --no-create-home --shell /usr/sbin/nologin plinth \
    && install -d -o 10001 -g 10001 \
       /var/lib/plinth /var/lib/plinth/data /var/lib/plinth/data/staging \
       /var/lib/plinth/logs

COPY --from=builder /opt/plinth-root/ /

RUN test -x /usr/local/bin/plinth \
    && test -f /usr/local/share/plinth/bundled/shell.zip \
    && test -f /usr/local/share/plinth/migrations/schema.sql \
    && test -f /usr/local/share/plinth/migrations/extension_database.sql \
    && test -f /usr/local/share/doc/plinth/LICENSE \
    && test -f /usr/local/share/doc/plinth/THIRD_PARTY_NOTICES.md \
    && test -f /usr/local/share/doc/plinth/sbom.cdx.json

LABEL org.opencontainers.image.title="Plinth" \
      org.opencontainers.image.description="Plinth self-hosted application kernel" \
      org.opencontainers.image.source="https://github.com/gobha-me/plinth" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.version="${PLINTH_VERSION}" \
      org.opencontainers.image.revision="${PLINTH_REVISION}"

ENV HOME=/var/lib/plinth \
    PLINTH_MIGRATIONS_DIR=/usr/local/share/plinth/migrations

WORKDIR /var/lib/plinth
VOLUME ["/var/lib/plinth/data", "/var/lib/plinth/logs"]
USER 10001:10001

EXPOSE 8080
STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/local/bin/plinth"]
CMD ["serve", "--host", "0.0.0.0"]
