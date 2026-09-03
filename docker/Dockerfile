# Build stage
FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libpq-dev libjemalloc-dev \
    libjsoncpp-dev libssl-dev zlib1g-dev \
    uuid-dev libargon2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-O2" && \
    cmake --build build --target plinth --parallel 2

# Runtime stage
FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171

RUN apt-get update && apt-get install -y \
    libpq5 libjemalloc2 libssl3 libargon2-1 libjsoncpp25 \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --system plinth && \
    useradd --system --gid plinth --home-dir /opt/plinth \
            --shell /usr/sbin/nologin plinth

# Use jemalloc as default allocator
ENV LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2

COPY --from=builder /src/build/plinth /usr/local/bin/plinth
COPY --from=builder /src/client /opt/plinth/client
COPY --from=builder /src/migrations /opt/plinth/migrations
COPY --from=builder /src/build/share/plinth /usr/local/share/plinth

RUN mkdir -p /opt/plinth/data/staging /opt/plinth/logs \
             /opt/plinth/uploads/tmp && \
    chown -R plinth:plinth /opt/plinth/data /opt/plinth/logs \
                           /opt/plinth/uploads

WORKDIR /opt/plinth
VOLUME ["/opt/plinth/data"]
USER plinth:plinth

EXPOSE 8080

ENTRYPOINT ["plinth"]
CMD ["serve", "--host", "0.0.0.0"]
