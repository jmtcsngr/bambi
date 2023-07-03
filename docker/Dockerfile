FROM debian:latest AS build_htslib

ARG HTSLIB_BRANCH

RUN apt update && \
    DEBIAN_FRONTEND=noninteractive apt upgrade -y && \
    apt install -qq -y \
      autoconf \
      build-essential \
      git \
      libbz2-dev \
      libcurl4-openssl-dev \
      libgd-dev \
      liblzma-dev \
      libssl-dev \
    && \
    export HTSLIB=/usr/local/src/htslib && \
    git clone --branch $HTSLIB_BRANCH https://github.com/samtools/htslib.git $HTSLIB && \
    cd $HTSLIB && \
    git submodule update --init --recursive && \
    mkdir -p /tmp/usr/local && autoreconf -i && ./configure --prefix=/tmp/usr/local && make && make install && \
    ls -R /tmp/usr/local

FROM debian:latest as bambi-build

COPY --from=build_htslib /tmp/usr/local /usr/local
COPY . /usr/local/src/bambi

RUN apt update && \
    DEBIAN_FRONTEND=noninteractive apt upgrade -y && \
    ls -laR /usr/local && \
    cd /usr/local/src/bambi && \
    autoreconf -i && ./configure --with-htslib=/usr/local && make && make check && make install

CMD ["/usr/bin/echo", "Hello World"]
