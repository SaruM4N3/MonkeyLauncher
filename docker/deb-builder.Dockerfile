FROM debian:bookworm-slim

# dpkg-dev for dpkg-deb; the rest is what the C++ (gtkmm-3.0) build needs.
RUN apt-get update && apt-get install -y --no-install-recommends \
    dpkg-dev \
    g++ \
    make \
    pkg-config \
    libgtkmm-3.0-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
ENTRYPOINT ["/build/packaging/build-deb.sh"]
