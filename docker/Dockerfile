FROM debian:latest AS htslib_build

ARG HTSLIB_BRANCH

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get upgrade -y && \
    apt-get install -qq -y \
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
    mkdir -p  /tmp/usr/local && autoreconf -i && ./configure --prefix=/tmp/usr/local && make && make install

FROM debian:latest as bambi_build

COPY --from=htslib_build /tmp/usr/local /usr/local
COPY . /usr/local/src/bambi

WORKDIR /usr/local/src/bambi

RUN apt-get update && \
    DEBIAN_FRONTEND=n oninteractive apt-get upgrade -y && \
    apt-get install -qq -y \
      autoconf \
      build-essential \
      bzip2 \
      git \
      libbz2-dev \
      libcurl4-openssl-dev \
      libtool \
      libgd-dev \
      liblzma-dev \
      libxml2-dev \
      libz-dev \
      libssl-dev \
    && \
    export PREFIX=/tmp/usr/local && \
    autoreconf -fi && \
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-Wl,-rpath,$PREFIX/lib -L$PREFIX/lib" \
            ./configure --prefix="$PREFIX" --with-htslib="$PREFIX" && \
    make CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" && \
    make install prefix="$PREFIX" && \
    ls -laR  /usr/local/bin

CMD ["/usr/local/bin/bambi", "--version"]

FROM debian:latest as bambi

COPY --from=htslib_build /tmp/usr/local /usr/local
COPY --from=bambi_build /tmp/usr/local /usr/local

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get upgrade -y && \
    apt-get install -qq -y \
      bzip2 \
      libbz2-dev \
      libcurl4-openssl-dev \
      libtool \
      libgd-dev \
      liblzma-dev \
      libxml2-dev \
      libz-dev \
      libssl-dev \
   && \
   apt-get clean && \
   rm -rf /var/lib/apt/lists/*

CMD ["/usr/local/bin/bambi", "--version"]
