FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    dpkg-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
ENTRYPOINT ["/build/packaging/build-deb.sh"]
