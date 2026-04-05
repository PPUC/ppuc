#!/bin/bash

set -e

source ./platforms/config.sh

NUM_PROCS=$(sysctl -n hw.ncpu)

echo "Building libraries..."
echo "  SDL_SHA: ${SDL_SHA}"
echo "  SDL_IMAGE_SHA: ${SDL_IMAGE_SHA}"
echo "  FLITE_SHA: ${FLITE_SHA}"
echo "  ESPEAK_NG_SHA: ${ESPEAK_NG_SHA}"
echo "  PINMAME_SHA: ${PINMAME_SHA}"
echo "  LIBPPUC_SHA: ${LIBPPUC_SHA}"
echo "  LIBDMDUTIL_SHA: ${LIBDMDUTIL_SHA}"
echo ""

if [ -z "${CACHE_DIR}" ]; then
   CACHE_DIR="external/cache/${BUILD_TYPE}"
fi

echo "Build type: ${BUILD_TYPE}"
echo "Cache dir: ${CACHE_DIR}"
echo "Procs: ${NUM_PROCS}"
echo ""

mkdir -p external ${CACHE_DIR}
cd external

#
# build SDL3, SDL3_image
#

SDL3_EXPECTED_SHA="${SDL_SHA}-${SDL_IMAGE_SHA}"
SDL3_FOUND_SHA="$([ -f SDL3/cache.txt ] && cat SDL3/cache.txt || echo "")"

if [ "${SDL3_EXPECTED_SHA}" != "${SDL3_FOUND_SHA}" ]; then
   echo "Building SDL3. Expected: ${SDL3_EXPECTED_SHA}, Found: ${SDL3_FOUND_SHA}"

   rm -rf SDL3
   mkdir SDL3
   cd SDL3

   curl -sL https://github.com/libsdl-org/SDL/archive/${SDL_SHA}.tar.gz -o SDL-${SDL_SHA}.tar.gz
   tar xzf SDL-${SDL_SHA}.tar.gz
   mv SDL-${SDL_SHA} SDL
   cd SDL
   cmake \
      -DSDL_SHARED=ON \
      -DSDL_STATIC=OFF \
      -DSDL_TEST_LIBRARY=OFF \
      -DSDL_OPENGLES=OFF \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   curl -sL https://github.com/libsdl-org/SDL_image/archive/${SDL_IMAGE_SHA}.tar.gz -o SDL_image-${SDL_IMAGE_SHA}.tar.gz
   tar xzf SDL_image-${SDL_IMAGE_SHA}.tar.gz
   mv SDL_image-${SDL_IMAGE_SHA} SDL_image
   cd SDL_image
   ./external/download.sh
   cmake \
      -DBUILD_SHARED_LIBS=ON \
      -DSDLIMAGE_SAMPLES=OFF \
      -DSDLIMAGE_DEPS_SHARED=ON \
      -DSDLIMAGE_VENDORED=ON \
      -DSDLIMAGE_AVIF=OFF \
      -DSDLIMAGE_WEBP=OFF \
      -DSDL3_DIR=../SDL/build \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$SDL3_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# flite
#

FLITE_EXPECTED_SHA="${FLITE_SHA}"
FLITE_FOUND_SHA="$([ -f flite/cache.txt ] && cat flite/cache.txt || echo "")"
FLITE_INSTALL_DIR="flite/flite/install"
FLITE_ARTIFACTS_OK=0
if [ -d "${FLITE_INSTALL_DIR}/include/flite" ] && ls "${FLITE_INSTALL_DIR}"/lib/libflite*.a >/dev/null 2>&1; then
   FLITE_ARTIFACTS_OK=1
fi

if [ "${FLITE_EXPECTED_SHA}" != "${FLITE_FOUND_SHA}" ] || [ "${FLITE_ARTIFACTS_OK}" -ne 1 ]; then
   echo "Building flite. Expected: ${FLITE_EXPECTED_SHA}, Found: ${FLITE_FOUND_SHA}"

   rm -rf flite
   mkdir flite
   cd flite

   curl -sL https://github.com/festvox/flite/archive/${FLITE_SHA}.tar.gz -o flite-${FLITE_SHA}.tar.gz
   tar xzf flite-${FLITE_SHA}.tar.gz
   mv flite-${FLITE_SHA} flite
   cd flite
   ./configure \
      --prefix="$(pwd)/install" \
      CFLAGS="-fPIC -O2"
   # We only need headers and static libraries for ppuc. Avoid Flite's CLI
   # tool build path and stage the required artifacts ourselves.
   make -j${NUM_PROCS} -C src
   make -j${NUM_PROCS} -C lang
   mkdir -p install/include install/lib
   cp -r include/* install/include/
   find build -path '*/lib/libflite*.a' -exec cp {} install/lib/ \;
   cd ..

   echo "$FLITE_EXPECTED_SHA" > cache.txt

   cd ..
fi

LIBDMDUTIL_EXPECTED_SHA="${LIBDMDUTIL_SHA}"
LIBDMDUTIL_FOUND_SHA="$([ -f libdmdutil/cache.txt ] && cat libdmdutil/cache.txt || echo "")"

if [ "${LIBDMDUTIL_EXPECTED_SHA}" != "${LIBDMDUTIL_FOUND_SHA}" ]; then
   echo "Building libdmdutil. Expected: ${LIBDMDUTIL_EXPECTED_SHA}, Found: ${LIBDMDUTIL_FOUND_SHA}"

   rm -rf libdmdutil
   mkdir libdmdutil
   cd libdmdutil

   curl -sL https://github.com/vpinball/libdmdutil/archive/${LIBDMDUTIL_SHA}.tar.gz -o libdmdutil-${LIBDMDUTIL_SHA}.tar.gz
   tar xzf libdmdutil-${LIBDMDUTIL_SHA}.tar.gz
   mv libdmdutil-${LIBDMDUTIL_SHA} libdmdutil
   cd libdmdutil
   ./platforms/macos/arm64/external.sh
   cmake \
      -DPLATFORM=macos \
      -DARCH=arm64 \
      -DBUILD_STATIC=OFF \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$LIBDMDUTIL_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# espeak-ng
#

ESPEAK_NG_EXPECTED_SHA="${ESPEAK_NG_SHA}"
ESPEAK_NG_FOUND_SHA="$([ -f espeak-ng/cache.txt ] && cat espeak-ng/cache.txt || echo "")"
ESPEAK_NG_INSTALL_DIR="espeak-ng/espeak-ng/install"
ESPEAK_NG_ARTIFACTS_OK=0
if [ -d "${ESPEAK_NG_INSTALL_DIR}/include/espeak-ng" ] && \
   ls "${ESPEAK_NG_INSTALL_DIR}"/lib/libespeak-ng*.dylib >/dev/null 2>&1 && \
   [ -d "${ESPEAK_NG_INSTALL_DIR}/share/espeak-ng-data" ]; then
   ESPEAK_NG_ARTIFACTS_OK=1
fi

if [ "${ESPEAK_NG_EXPECTED_SHA}" != "${ESPEAK_NG_FOUND_SHA}" ] || [ "${ESPEAK_NG_ARTIFACTS_OK}" -ne 1 ]; then
   echo "Building espeak-ng. Expected: ${ESPEAK_NG_EXPECTED_SHA}, Found: ${ESPEAK_NG_FOUND_SHA}"

   rm -rf espeak-ng
   mkdir espeak-ng
   cd espeak-ng

   curl -sL https://github.com/espeak-ng/espeak-ng/archive/refs/tags/${ESPEAK_NG_SHA}.tar.gz -o espeak-ng-${ESPEAK_NG_SHA}.tar.gz
   tar xzf espeak-ng-${ESPEAK_NG_SHA}.tar.gz
   mv espeak-ng-${ESPEAK_NG_SHA} espeak-ng
   cd espeak-ng
   cmake \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_TESTING=OFF \
      -DUSE_MBROLA=OFF \
      -DUSE_LIBSONIC=OFF \
      -DUSE_LIBPCAUDIO=OFF \
      -DUSE_SPEECHPLAYER=OFF \
      -DUSE_ASYNC=OFF \
      -DCMAKE_INSTALL_PREFIX="$(pwd)/install" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cmake --install build
   cd ..

   echo "$ESPEAK_NG_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# pinamame
#

PINMAME_EXPECTED_SHA="${PINMAME_SHA}"
PINMAME_FOUND_SHA="$([ -f pinmame/cache.txt ] && cat pinmame/cache.txt || echo "")"

if [ "${PINMAME_EXPECTED_SHA}" != "${PINMAME_FOUND_SHA}" ]; then
   echo "Building libpinmame. Expected: ${PINMAME_EXPECTED_SHA}, Found: ${PINMAME_FOUND_SHA}"

   rm -rf pinmame
   mkdir pinmame
   cd pinmame

   curl -sL https://github.com/vpinball/pinmame/archive/${PINMAME_SHA}.tar.gz -o pinmame-${PINMAME_SHA}.tar.gz
   tar xzf pinmame-${PINMAME_SHA}.tar.gz
   mv pinmame-${PINMAME_SHA} pinmame
   cd pinmame
   cp cmake/libpinmame/CMakeLists.txt .
   cmake \
      -DPLATFORM=macos \
      -DARCH=arm64 \
      -DBUILD_STATIC=OFF \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$PINMAME_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# libppuc
#

LIBPPUC_EXPECTED_SHA="${LIBPPUC_SHA}"
LIBPPUC_FOUND_SHA="$([ -f libppuc/cache.txt ] && cat libppuc/cache.txt || echo "")"

if [ "${LIBPPUC_EXPECTED_SHA}" != "${LIBPPUC_FOUND_SHA}" ]; then
   echo "Building libppuc. Expected: ${LIBPPUC_EXPECTED_SHA}, Found: ${LIBPPUC_FOUND_SHA}"

   rm -rf libppuc
   mkdir libppuc
   cd libppuc

   curl -sL https://github.com/PPUC/libppuc/archive/${LIBPPUC_SHA}.tar.gz -o libppuc-${LIBPPUC_SHA}.tar.gz
   tar xzf libppuc-${LIBPPUC_SHA}.tar.gz
   mv libppuc-${LIBPPUC_SHA} libppuc
   cd libppuc

   BUILD_TYPE=${BUILD_TYPE} platforms/macos/arm64/external.sh

   cmake \
      -DPLATFORM=macos \
      -DARCH=arm64 \
      -DBUILD_STATIC=OFF \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$LIBPPUC_EXPECTED_SHA" > cache.txt

   cd ..
fi

cp -a SDL3/SDL/build/libSDL3.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r SDL3/SDL/include/SDL3 ../third-party/include/

cp -a SDL3/SDL_image/build/libSDL3_image.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r SDL3/SDL_image/include/SDL3_image ../third-party/include/

mkdir -p ../third-party/include/flite
cp -r flite/flite/install/include/* ../third-party/include/flite/
cp -a flite/flite/install/lib/libflite*.a ../third-party/build-libs/macos-arm64/

cp -r espeak-ng/espeak-ng/install/include/espeak-ng ../third-party/include/
cp -a espeak-ng/espeak-ng/install/lib/libespeak-ng*.dylib ../third-party/runtime-libs/macos-arm64/
cp -R espeak-ng/espeak-ng/install/share/espeak-ng-data ../third-party/runtime-libs/macos-arm64/

cp -a pinmame/pinmame/build/libpinmame.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp pinmame/pinmame/src/libpinmame/libpinmame.h ../third-party/include/
#cp pinmame/pinmame/src/libpinmame/pinmamedef.h ../third-party/include/

cp -a libdmdutil/libdmdutil/build/libdmdutil.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r libdmdutil/libdmdutil/include/DMDUtil ../third-party/include/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libusb*.dylib ../third-party/runtime-libs/macos-arm64/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libvni.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libzedmd.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp libdmdutil/libdmdutil/third-party/include/ZeDMD.h ../third-party/include/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libserum.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp libdmdutil/libdmdutil/third-party/include/serum.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/include/serum-decode.h ../third-party/include/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libserialport.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libpupdmd.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp libdmdutil/libdmdutil/third-party/include/pupdmd.h ../third-party/include/
cp -a libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libsockpp.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp libdmdutil/libdmdutil/third-party/runtime-libs/macos/arm64/libcargs.dylib ../third-party/runtime-libs/macos-arm64/
cp -r libdmdutil/libdmdutil/third-party/include/sockpp ../third-party/include/
cp libdmdutil/libdmdutil/third-party/include/cargs.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/include/FrameUtil.h ../third-party/include/

cp libppuc/libppuc/src/PPUC.h ../third-party/include/
cp libppuc/libppuc/src/PPUC_structs.h ../third-party/include/
cp -r libppuc/libppuc/third-party/include/yaml-cpp ../third-party/include/
cp -r libppuc/libppuc/third-party/include/io-boards ../third-party/include/
cp -a libppuc/libppuc/build/libppuc.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -a libppuc/libppuc/third-party/runtime-libs/macos/arm64/libyaml-cpp.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
