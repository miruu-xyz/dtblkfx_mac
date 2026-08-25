#!/bin/bash
# Build a universal (arm64 + x86_64) single-precision FFTW and drop it into
# third_party/fftw-universal/, which is what CMakeLists.txt links against.
#
# Only needs running once. Replaces the vcpkg dependency the upstream repo used.
set -euo pipefail

FFTW_VERSION=3.3.10
FFTW_SHA256=56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${ROOT}/build/fftw-src"
OUT="${ROOT}/third_party/fftw-universal"

mkdir -p "${WORK}"
cd "${WORK}"

if [ ! -f "fftw-${FFTW_VERSION}.tar.gz" ]; then
  echo "==> Downloading FFTW ${FFTW_VERSION}"
  curl -fsSL -o "fftw-${FFTW_VERSION}.tar.gz" "https://www.fftw.org/fftw-${FFTW_VERSION}.tar.gz"
fi

echo "${FFTW_SHA256}  fftw-${FFTW_VERSION}.tar.gz" | shasum -a 256 -c -

[ -d "fftw-${FFTW_VERSION}" ] || tar xzf "fftw-${FFTW_VERSION}.tar.gz"

build_slice() {
  local arch="$1" simd="$2"
  echo "==> Building FFTW for ${arch}"
  rm -rf "build-${arch}"
  mkdir "build-${arch}"
  cd "build-${arch}"
  # Note: x86_64 stops at SSE2 on purpose. The x86_64 slice runs under Rosetta,
  # and SSE2 is the safe floor there.
  ../"fftw-${FFTW_VERSION}"/configure \
    --prefix="${WORK}/prefix/${arch}" \
    --enable-float "${simd}" \
    --enable-static --disable-shared --disable-fortran --disable-doc \
    $([ "${arch}" = "x86_64" ] && echo "--host=x86_64-apple-darwin") \
    CC="clang -arch ${arch}" CFLAGS="-O3" > configure.log 2>&1
  make -j"$(sysctl -n hw.ncpu)" > make.log 2>&1
  make install > install.log 2>&1
  cd ..
}

build_slice arm64  --enable-neon
build_slice x86_64 --enable-sse2

echo "==> Creating universal library"
mkdir -p "${OUT}/lib" "${OUT}/include"
lipo -create \
  "${WORK}/prefix/arm64/lib/libfftw3f.a" \
  "${WORK}/prefix/x86_64/lib/libfftw3f.a" \
  -output "${OUT}/lib/libfftw3f.a"
cp "${WORK}/prefix/arm64/include/fftw3.h" "${OUT}/include/"

lipo -info "${OUT}/lib/libfftw3f.a"
echo "==> Done: ${OUT}"
