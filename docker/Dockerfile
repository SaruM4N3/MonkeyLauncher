FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu 20.04 = glibc 2.31 — binaries built here run on any distro >= 2020
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    pkg-config \
    libgtkmm-3.0-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY Makefile VERSION ./
COPY cpp/ cpp/
COPY src/ src/
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
