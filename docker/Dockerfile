FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu 20.04 = glibc 2.31 — binaries built here run on any distro >= 2020
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-gi \
    gir1.2-gtk-3.0 \
    libgtk-3-0 \
    libgtk-3-dev \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir pyinstaller

WORKDIR /build
COPY src/ src/
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
