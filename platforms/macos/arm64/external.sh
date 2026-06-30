#!/bin/bash

set -e

source ./platforms/config.sh

NUM_PROCS=$(sysctl -n hw.ncpu)

echo "Building libraries..."
echo "  SDL_IMAGE_SHA: ${SDL_IMAGE_SHA}"
echo "  SDL_MIXER_SHA: ${SDL_MIXER_SHA}"
echo "  FLITE_SHA: ${FLITE_SHA}"
echo "  ESPEAK_NG_SHA: ${ESPEAK_NG_SHA}"
echo "  LUA_VERSION: ${LUA_VERSION}"
echo "  PINMAME_SHA: ${PINMAME_SHA}"
echo "  PINMAME_NVRAM_MAPS_SHA: ${PINMAME_NVRAM_MAPS_SHA}"
echo "  LIBPPUC_SHA: ${LIBPPUC_SHA}"
ppuc_print_dependency_source LIBPPUC libppuc "${LIBPPUC_SHA}"
echo "  LIBSDLDMD_SHA: ${LIBSDLDMD_SHA}"
ppuc_print_dependency_source LIBSDLDMD libsdldmd "${LIBSDLDMD_SHA}"
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

ppuc_stage_lua_source

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

LIBSDLDMD_EXPECTED_SHA="$(ppuc_dependency_cache_key libsdldmd "${LIBSDLDMD_SHA}")"
LIBSDLDMD_FOUND_SHA="$([ -f libsdldmd/cache.txt ] && cat libsdldmd/cache.txt || echo "")"

if [ -n "${LIBSDLDMD_SHA}" ] && [ "${LIBSDLDMD_EXPECTED_SHA}" != "${LIBSDLDMD_FOUND_SHA}" ]; then
   echo "Building libsdldmd. Expected: ${LIBSDLDMD_EXPECTED_SHA}, Found: ${LIBSDLDMD_FOUND_SHA}"

   rm -rf libsdldmd
   mkdir libsdldmd
   cd libsdldmd

   ppuc_prepare_dependency_source libsdldmd "${LIBSDLDMD_SHA}" "https://github.com/PPUC/libsdldmd/archive/${LIBSDLDMD_SHA}.tar.gz"
   cd libsdldmd

   BUILD_TYPE=${BUILD_TYPE} platforms/macos/arm64/external.sh

   cmake \
      -DPLATFORM=macos \
      -DARCH=arm64 \
      -DBUILD_STATIC=OFF \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$LIBSDLDMD_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# build SDL3_image and SDL3_mixer against the SDL staged by libsdldmd
#

SDL3_IMAGE_EXPECTED_SHA="${SDL_IMAGE_SHA}-${LIBSDLDMD_SHA}"
SDL3_IMAGE_FOUND_SHA="$([ -f SDL3_image/cache.txt ] && cat SDL3_image/cache.txt || echo "")"

if [ "${SDL3_IMAGE_EXPECTED_SHA}" != "${SDL3_IMAGE_FOUND_SHA}" ]; then
   echo "Building SDL3_image. Expected: ${SDL3_IMAGE_EXPECTED_SHA}, Found: ${SDL3_IMAGE_FOUND_SHA}"

   rm -rf SDL3_image
   mkdir SDL3_image
   cd SDL3_image

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
      -DSDL3_DIR=../../libsdldmd/libsdldmd/external/SDL/build \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$SDL3_IMAGE_EXPECTED_SHA" > cache.txt

   cd ..
fi

SDL3_MIXER_EXPECTED_SHA="${SDL_MIXER_SHA}-${LIBSDLDMD_SHA}-mp3only-v1"
SDL3_MIXER_FOUND_SHA="$([ -f SDL3_mixer/cache.txt ] && cat SDL3_mixer/cache.txt || echo "")"

if [ "${SDL3_MIXER_EXPECTED_SHA}" != "${SDL3_MIXER_FOUND_SHA}" ]; then
   echo "Building SDL3_mixer. Expected: ${SDL3_MIXER_EXPECTED_SHA}, Found: ${SDL3_MIXER_FOUND_SHA}"

   rm -rf SDL3_mixer
   mkdir SDL3_mixer
   cd SDL3_mixer

   curl -sL https://github.com/libsdl-org/SDL_mixer/archive/${SDL_MIXER_SHA}.tar.gz -o SDL_mixer-${SDL_MIXER_SHA}.tar.gz
   tar xzf SDL_mixer-${SDL_MIXER_SHA}.tar.gz
   mv SDL_mixer-${SDL_MIXER_SHA#release-} SDL_mixer 2>/dev/null || mv SDL_mixer-${SDL_MIXER_SHA} SDL_mixer
   cd SDL_mixer
   cmake \
      -DBUILD_SHARED_LIBS=ON \
      -DSDLMIXER_SAMPLES=OFF \
      -DSDLMIXER_FLAC=OFF \
      -DSDLMIXER_GME=OFF \
      -DSDLMIXER_MOD=OFF \
      -DSDLMIXER_MP3=ON \
      -DSDLMIXER_MP3_DRMP3=ON \
      -DSDLMIXER_MP3_MPG123=OFF \
      -DSDLMIXER_OPUS=OFF \
      -DSDLMIXER_VORBIS_STB=OFF \
      -DSDLMIXER_VORBIS_VORBISFILE=OFF \
      -DSDLMIXER_VORBIS_TREMOR=OFF \
      -DSDLMIXER_WAVPACK=OFF \
      -DSDL3_DIR=../../libsdldmd/libsdldmd/external/SDL/build \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$SDL3_MIXER_EXPECTED_SHA" > cache.txt

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

   curl -sL https://github.com/mkalkbrenner/pinmame/archive/${PINMAME_SHA}.tar.gz -o pinmame-${PINMAME_SHA}.tar.gz
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

PINMAME_NVRAM_MAPS_EXPECTED_SHA="${PINMAME_NVRAM_MAPS_SHA}"
PINMAME_NVRAM_MAPS_FOUND_SHA="$([ -f pinmame-nvram-maps/cache.txt ] && cat pinmame-nvram-maps/cache.txt || echo "")"

if [ "${PINMAME_NVRAM_MAPS_EXPECTED_SHA}" != "${PINMAME_NVRAM_MAPS_FOUND_SHA}" ]; then
   echo "Staging pinmame-nvram-maps. Expected: ${PINMAME_NVRAM_MAPS_EXPECTED_SHA}, Found: ${PINMAME_NVRAM_MAPS_FOUND_SHA}"

   rm -rf pinmame-nvram-maps
   mkdir pinmame-nvram-maps
   cd pinmame-nvram-maps

   curl -sL https://github.com/tomlogic/pinmame-nvram-maps/archive/${PINMAME_NVRAM_MAPS_SHA}.tar.gz -o pinmame-nvram-maps-${PINMAME_NVRAM_MAPS_SHA}.tar.gz
   tar xzf pinmame-nvram-maps-${PINMAME_NVRAM_MAPS_SHA}.tar.gz
   mv pinmame-nvram-maps-${PINMAME_NVRAM_MAPS_SHA} pinmame-nvram-maps

   echo "$PINMAME_NVRAM_MAPS_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# libppuc
#

LIBPPUC_EXPECTED_SHA="$(ppuc_dependency_cache_key libppuc "${LIBPPUC_SHA}")"
LIBPPUC_FOUND_SHA="$([ -f libppuc/cache.txt ] && cat libppuc/cache.txt || echo "")"

if [ "${LIBPPUC_EXPECTED_SHA}" != "${LIBPPUC_FOUND_SHA}" ]; then
   echo "Building libppuc. Expected: ${LIBPPUC_EXPECTED_SHA}, Found: ${LIBPPUC_FOUND_SHA}"

   rm -rf libppuc
   mkdir libppuc
   cd libppuc

   ppuc_prepare_dependency_source libppuc "${LIBPPUC_SHA}" "https://github.com/PPUC/libppuc/archive/${LIBPPUC_SHA}.tar.gz"
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

cp -a libsdldmd/libsdldmd/third-party/runtime-libs/macos/arm64/libSDL3.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r libsdldmd/libsdldmd/third-party/include/SDL3 ../third-party/include/

cp -a SDL3_image/SDL_image/build/libSDL3_image.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r SDL3_image/SDL_image/include/SDL3_image ../third-party/include/

cp -a SDL3_mixer/SDL_mixer/build/libSDL3_mixer.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -r SDL3_mixer/SDL_mixer/include/SDL3_mixer ../third-party/include/

mkdir -p ../third-party/include/flite
cp -r flite/flite/install/include/* ../third-party/include/flite/
cp -a flite/flite/install/lib/libflite*.a ../third-party/build-libs/macos-arm64/

cp -r espeak-ng/espeak-ng/install/include/espeak-ng ../third-party/include/
cp -a espeak-ng/espeak-ng/install/lib/libespeak-ng*.dylib ../third-party/runtime-libs/macos-arm64/
cp -R espeak-ng/espeak-ng/install/share/espeak-ng-data ../third-party/runtime-libs/macos-arm64/

cp -a pinmame/pinmame/build/libpinmame.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp pinmame/pinmame/src/libpinmame/libpinmame.h ../third-party/include/
rm -rf ../third-party/pinmame-nvram-maps
mkdir -p ../third-party/pinmame-nvram-maps
cp pinmame-nvram-maps/pinmame-nvram-maps/index.json ../third-party/pinmame-nvram-maps/
cp -R pinmame-nvram-maps/pinmame-nvram-maps/maps ../third-party/pinmame-nvram-maps/
cp -R pinmame-nvram-maps/pinmame-nvram-maps/platforms ../third-party/pinmame-nvram-maps/
#cp pinmame/pinmame/src/libpinmame/pinmamedef.h ../third-party/include/

if [ -n "${LIBSDLDMD_SHA}" ]; then
   LIBSDLDMD_DMDUTIL_THIRD_PARTY="libsdldmd/libsdldmd/external/libdmdutil/third-party"

   cp -a libsdldmd/libsdldmd/third-party/runtime-libs/macos/arm64/libdmdutil.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp -r libsdldmd/libsdldmd/third-party/include/DMDUtil ../third-party/include/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libusb*.dylib ../third-party/runtime-libs/macos-arm64/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libvni.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libzedmd.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/ZeDMD.h ../third-party/include/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libserum.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum-decode.h ../third-party/include/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libserialport.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libpupdmd.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/pupdmd.h ../third-party/include/
   cp -a ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libsockpp.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/macos/arm64/libcargs.dylib ../third-party/runtime-libs/macos-arm64/
   cp -r ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/sockpp ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/cargs.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/FrameUtil.h ../third-party/include/
   cp -r libsdldmd/libsdldmd/include/SDLDMD ../third-party/include/
   cp -a libsdldmd/libsdldmd/build/libsdldmd.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
fi

cp libppuc/libppuc/src/PPUC.h ../third-party/include/
cp libppuc/libppuc/src/PPUC_structs.h ../third-party/include/
cp -r libppuc/libppuc/third-party/include/yaml-cpp ../third-party/include/
cp -r libppuc/libppuc/third-party/include/io-boards ../third-party/include/
cp -a libppuc/libppuc/build/libppuc.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
cp -a libppuc/libppuc/third-party/runtime-libs/macos/arm64/libyaml-cpp.{dylib,*.dylib} ../third-party/runtime-libs/macos-arm64/
