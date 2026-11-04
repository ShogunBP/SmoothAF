FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       gcc-arm-none-eabi \
       binutils-arm-none-eabi \
       libnewlib-arm-none-eabi \
       libc6-dev \
       file \
       mercurial \
       python3 \
       ca-certificates \
       bzip2 \
       gzip \
       zip \
       xz-utils \
       git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

CMD ["/bin/sh"]
