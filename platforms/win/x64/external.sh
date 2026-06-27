#!/bin/bash

set -e

source ./platforms/config.sh

echo "Building libraries..."
echo "  SDL_IMAGE_SHA: ${SDL_IMAGE_SHA}"
echo "  SDL_MIXER_SHA: ${SDL_MIXER_SHA}"
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
echo ""

mkdir -p external ${CACHE_DIR}
cd external

#
# build libsdldmd, SDL3_image, SDL3_mixer
#

LIBSDLDMD_EXPECTED_SHA="$(ppuc_dependency_cache_key libsdldmd "${LIBSDLDMD_SHA}")"
LIBSDLDMD_FOUND_SHA="$([ -f libsdldmd/cache.txt ] && cat libsdldmd/cache.txt || echo "")"

if [ -n "${LIBSDLDMD_SHA}" ] && [ "${LIBSDLDMD_EXPECTED_SHA}" != "${LIBSDLDMD_FOUND_SHA}" ]; then
   echo "Building libsdldmd. Expected: ${LIBSDLDMD_EXPECTED_SHA}, Found: ${LIBSDLDMD_FOUND_SHA}"

   rm -rf libsdldmd
   mkdir libsdldmd
   cd libsdldmd

   ppuc_prepare_dependency_source libsdldmd "${LIBSDLDMD_SHA}" "https://github.com/PPUC/libsdldmd/archive/${LIBSDLDMD_SHA}.tar.gz"
   cd libsdldmd

   BUILD_TYPE=${BUILD_TYPE} platforms/win/x64/external.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$LIBSDLDMD_EXPECTED_SHA" > cache.txt

   cd ..
fi

SDL3_IMAGE_EXPECTED_SHA="${SDL_IMAGE_SHA}-${LIBSDLDMD_SHA}"
SDL3_IMAGE_FOUND_SHA="$([ -f SDL3_image/cache.txt ] && cat SDL3_image/cache.txt || echo "")"

if [ "${SDL3_IMAGE_EXPECTED_SHA}" != "${SDL3_IMAGE_FOUND_SHA}" ]; then
   echo "Building SDL3_image. Expected: ${SDL3_IMAGE_EXPECTED_SHA}, Found: ${SDL3_IMAGE_FOUND_SHA}"

   rm -rf SDL3_image
   mkdir SDL3_image
   cd SDL3_image

   curl -sL https://github.com/libsdl-org/SDL_image/archive/${SDL_IMAGE_SHA}.tar.gz -o SDL_image-${SDL_IMAGE_SHA}.tar.gz
   tar xzf SDL_image-${SDL_IMAGE_SHA}.tar.gz --exclude='*/Xcode/*'
   mv SDL_image-${SDL_IMAGE_SHA} SDL_image
   cd SDL_image
   sed -i.bak 's/OUTPUT_NAME "SDL3_image"/OUTPUT_NAME "SDL3_image64"/g' CMakeLists.txt
   ./external/download.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -DBUILD_SHARED_LIBS=ON \
      -DSDLIMAGE_SAMPLES=OFF \
      -DSDLIMAGE_DEPS_SHARED=ON \
      -DSDLIMAGE_VENDORED=ON \
      -DSDLIMAGE_AVIF=OFF \
      -DSDLIMAGE_WEBP=OFF \
      -DSDL3_DIR=../../libsdldmd/libsdldmd/external/SDL/build \
      -B build
   cmake --build build --config ${BUILD_TYPE}
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
   tar xzf SDL_mixer-${SDL_MIXER_SHA}.tar.gz --exclude='*/Xcode/*'
   mv SDL_mixer-${SDL_MIXER_SHA#release-} SDL_mixer 2>/dev/null || mv SDL_mixer-${SDL_MIXER_SHA} SDL_mixer
   cd SDL_mixer
   sed -i.bak 's/OUTPUT_NAME "SDL3_mixer"/OUTPUT_NAME "SDL3_mixer64"/g' CMakeLists.txt
   cmake \
      -G "Visual Studio 17 2022" \
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
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$SDL3_MIXER_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# build pinmame
#

PINMAME_EXPECTED_SHA="${PINMAME_SHA}"
PINMAME_FOUND_SHA="$([ -f pinmame/cache.txt ] && cat pinmame/cache.txt || echo "")"

if [ "${PINMAME_EXPECTED_SHA}" != "${PINMAME_FOUND_SHA}" ]; then
   echo "Building libpinmame. Expected: ${PINMAME_EXPECTED_SHA}, Found: ${PINMAME_FOUND_SHA}"

   rm -rf pinmame
   mkdir pinmame
   cd pinmame

   curl -sL https://github.com/vbousquet/pinmame/archive/${PINMAME_SHA}.tar.gz -o pinmame-${PINMAME_SHA}.tar.gz
   tar xzf pinmame-${PINMAME_SHA}.tar.gz
   mv pinmame-${PINMAME_SHA} pinmame
   cd pinmame
   cp cmake/libpinmame/CMakeLists.txt .
   cmake \
      -G "Visual Studio 17 2022" \
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
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

    platforms/win/x64/external.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$LIBPPUC_EXPECTED_SHA" > cache.txt

   cd ..
fi

cp libsdldmd/libsdldmd/third-party/build-libs/win/x64/SDL364.lib ../third-party/build-libs/win-x64/
cp libsdldmd/libsdldmd/third-party/runtime-libs/win/x64/SDL364.dll ../third-party/runtime-libs/win-x64/
cp -r libsdldmd/libsdldmd/third-party/include/SDL3 ../third-party/include/

cp SDL3_image/SDL_image/build/${BUILD_TYPE}/SDL3_image64.lib ../third-party/build-libs/win-x64/
cp SDL3_image/SDL_image/build/${BUILD_TYPE}/SDL3_image64.dll ../third-party/runtime-libs/win-x64/
cp -r SDL3_image/SDL_image/include/SDL3_image ../third-party/include/

cp SDL3_mixer/SDL_mixer/build/${BUILD_TYPE}/SDL3_mixer64.lib ../third-party/build-libs/win-x64/
cp SDL3_mixer/SDL_mixer/build/${BUILD_TYPE}/SDL3_mixer64.dll ../third-party/runtime-libs/win-x64/
cp -r SDL3_mixer/SDL_mixer/include/SDL3_mixer ../third-party/include/

cp pinmame/pinmame/build/${BUILD_TYPE}/pinmame64.lib ../third-party/build-libs/win-x64/
cp pinmame/pinmame/build/${BUILD_TYPE}/pinmame64.dll ../third-party/runtime-libs/win-x64/
cp pinmame/pinmame/src/libpinmame/libpinmame.h ../third-party/include/
rm -rf ../third-party/pinmame-nvram-maps
mkdir -p ../third-party/pinmame-nvram-maps
cp pinmame-nvram-maps/pinmame-nvram-maps/index.json ../third-party/pinmame-nvram-maps/
cp -R pinmame-nvram-maps/pinmame-nvram-maps/maps ../third-party/pinmame-nvram-maps/
cp -R pinmame-nvram-maps/pinmame-nvram-maps/platforms ../third-party/pinmame-nvram-maps/
#cp pinmame/pinmame/src/libpinmame/pinmamedef.h ../third-party/include/

if [ -n "${LIBSDLDMD_SHA}" ]; then
   LIBSDLDMD_DMDUTIL_THIRD_PARTY="libsdldmd/libsdldmd/external/libdmdutil/third-party"

   cp libsdldmd/libsdldmd/third-party/build-libs/win/x64/dmdutil64.lib ../third-party/build-libs/win-x64/
   cp libsdldmd/libsdldmd/third-party/runtime-libs/win/x64/dmdutil64.dll ../third-party/runtime-libs/win-x64/
   cp -r libsdldmd/libsdldmd/third-party/include/DMDUtil ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/libusb64-1.0.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/libusb64-1.0.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/vni64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/vni64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/zedmd64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/zedmd64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/ZeDMD.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/serum64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/serum64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum-decode.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/libserialport64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/libserialport64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/pupdmd64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/pupdmd64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/pupdmd.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/sockpp64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/sockpp64.dll ../third-party/runtime-libs/win-x64/
   cp -r ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/sockpp ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win/x64/cargs64.lib ../third-party/build-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win/x64/cargs64.dll ../third-party/runtime-libs/win-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/cargs.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/FrameUtil.h ../third-party/include/
   cp libsdldmd/libsdldmd/build/${BUILD_TYPE}/sdldmd64.lib ../third-party/build-libs/win-x64/
   cp libsdldmd/libsdldmd/build/${BUILD_TYPE}/sdldmd64.dll ../third-party/runtime-libs/win-x64/
   cp -r libsdldmd/libsdldmd/include/SDLDMD ../third-party/include/
fi

cp libppuc/libppuc/src/PPUC.h ../third-party/include/
cp libppuc/libppuc/src/PPUC_structs.h ../third-party/include/
cp -r libppuc/libppuc/third-party/include/yaml-cpp ../third-party/include/
cp -r libppuc/libppuc/third-party/include/io-boards ../third-party/include/
cp libppuc/libppuc/build/${BUILD_TYPE}/ppuc64.lib ../third-party/build-libs/win-x64/
cp libppuc/libppuc/build/${BUILD_TYPE}/ppuc64.dll ../third-party/runtime-libs/win-x64/
cp -a libppuc/libppuc/third-party/build-libs/win/x64/yaml-cpp.lib ../third-party/build-libs/win-x64/
cp -a libppuc/libppuc/third-party/runtime-libs/win/x64/yaml-cpp.dll ../third-party/runtime-libs/win-x64/
