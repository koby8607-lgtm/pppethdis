#!/usr/bin/env bash
set -euo pipefail

VERSION="2.5.4"
TAG="v${VERSION}"
WORK="${RUNNER_TEMP:-/tmp}/usb0-ppp-${VERSION}"
SRC="${WORK}/ppp"
OUT="app/src/main/assets/tools/arm64-v8a"

NDK="${ANDROID_NDK_ROOT:-${ANDROID_HOME}/ndk/27.2.12479018}"
TOOLCHAIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="${TOOLCHAIN}/aarch64-linux-android30-clang"
AR="${TOOLCHAIN}/llvm-ar"
RANLIB="${TOOLCHAIN}/llvm-ranlib"
STRIP="${TOOLCHAIN}/llvm-strip"

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"

test -x "$CC"

tarball="${WORK}/ppp-${VERSION}.tar.gz"
url="https://github.com/ppp-project/ppp/archive/refs/tags/${TAG}.tar.gz"

echo "Downloading official PPP ${VERSION} source"
curl --fail --show-error --location --retry 10 --retry-all-errors --retry-delay 3 "$url" -o "$tarball"
test -s "$tarball"

tar -xzf "$tarball" -C "$WORK"
mv "${WORK}/ppp-${TAG#v}" "$SRC"
cd "$SRC"

# GitHub source archives may not include a generated configure script.
# Generate it with the project's own autotools bootstrap when necessary.
if [ ! -x ./configure ]; then
  echo "configure not present; running PPP autotools bootstrap"
  test -x ./autogen.sh
  ./autogen.sh
fi

test -x ./configure

export CC
export AR
export RANLIB
export CFLAGS="-O2 -fPIE -fstack-protector-strong -DANDROID -D__ANDROID_API__=30"
export CPPFLAGS="$CFLAGS"
export LDFLAGS="-pie"
export LIBS=""

./configure \
  --build="$(gcc -dumpmachine)" \
  --host=aarch64-linux-gnu \
  --prefix=/data/local/tmp/usb0-ppp \
  --sysconfdir=/data/local/tmp/usb0-ppp/etc \
  --disable-plugins \
  --disable-eaptls \
  --disable-peap \
  --disable-multilink \
  --disable-systemd \
  --without-openssl \
  --without-pam \
  --without-pcap \
  --without-atm

make -j"$(nproc)" CFLAGS="$CFLAGS" CPPFLAGS="$CPPFLAGS" LDFLAGS="$LDFLAGS"

test -s pppd/pppd
test -s chat/chat

cp pppd/pppd "$OUT/pppd"
cp chat/chat "$OUT/chat"

"$STRIP" "$OUT/pppd" || true
"$STRIP" "$OUT/chat" || true

chmod 755 "$OUT/pppd" "$OUT/chat"

file "$OUT/pppd"
file "$OUT/chat"

file "$OUT/pppd" | grep -Eiq 'ELF 64-bit.*ARM aarch64'
file "$OUT/chat" | grep -Eiq 'ELF 64-bit.*ARM aarch64'


