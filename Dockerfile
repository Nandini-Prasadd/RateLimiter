# Multi-stage Dockerfile to build and package rate limiter components
FROM gcc:12 AS builder

# Install CMake
RUN apt-get update && apt-get install -y cmake && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy files
COPY . .

# Build components
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . --parallel $(nproc)

# Final runtime image
FROM debian:bookworm-slim

# Install standard dependencies
RUN apt-get update && apt-get install -y libstdc++6 ca-certificates && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binaries and configurations
COPY --from=builder /app/build/rate_limiter_proxy /app/
COPY --from=builder /app/build/backend_simulator /app/
COPY --from=builder /app/config /app/config

EXPOSE 8080 8081
