#!/bin/bash
# Static ARM utilities must run on Linux 3.0, below Ubuntu glibc's kernel floor.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 1
ARCHIVE=/mnt/build-share/toolchains/musl-1.2.5.tar.gz
echo "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4  $ARCHIVE" | sha256sum -c -
BUILD=/home/builder/pw3-build/test-libc
mkdir -p "$BUILD"
if [[ ! -f "$BUILD/musl-1.2.5/configure" ]]; then
  tar -xzf "$ARCHIVE" -C "$BUILD"
fi
if [[ ! -f "$BUILD/musl-1.2.5/config.mak" ]] || \
   ! grep -q '^ALL_TOOLS = .*obj/musl-gcc' "$BUILD/musl-1.2.5/config.mak"; then
  cd "$BUILD/musl-1.2.5"
  CC=arm-linux-gnueabi-gcc AR=arm-linux-gnueabi-ar RANLIB=arm-linux-gnueabi-ranlib \
    ./configure --target=arm-linux-musleabi --prefix=/home/builder/toolchains/musl-arm \
    --disable-shared --enable-wrapper=gcc CFLAGS='-Os -march=armv7-a -mfloat-abi=soft'
fi
make -C "$BUILD/musl-1.2.5" -j3
make -C "$BUILD/musl-1.2.5" install
