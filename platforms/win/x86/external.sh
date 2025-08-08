#!/bin/bash

set -e

source ./platforms/config.sh

echo "Building libraries..."
echo "  SDL_SHA: ${SDL_SHA}"
echo "  SDL_IMAGE_SHA: ${SDL_IMAGE_SHA}"
echo "  PINMAME_SHA: ${PINMAME_SHA}"
echo "  LIBPPUC_SHA: ${LIBPPUC_SHA}"
echo "  LIBDMDUTIL_SHA: ${LIBDMDUTIL_SHA}"
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
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DSDL_SHARED=ON \
      -DSDL_STATIC=OFF \
      -DSDL_TEST_LIBRARY=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   curl -sL https://github.com/libsdl-org/SDL_image/archive/${SDL_IMAGE_SHA}.tar.gz -o SDL_image-${SDL_IMAGE_SHA}.tar.gz
   tar xzf SDL_image-${SDL_IMAGE_SHA}.tar.gz --exclude='*/Xcode/*'
   mv SDL_image-${SDL_IMAGE_SHA} SDL_image
   cd SDL_image
   ./external/download.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DBUILD_SHARED_LIBS=ON \
      -DSDLIMAGE_SAMPLES=OFF \
      -DSDLIMAGE_DEPS_SHARED=ON \
      -DSDLIMAGE_VENDORED=ON \
      -DSDLIMAGE_AVIF=OFF \
      -DSDLIMAGE_WEBP=OFF \
      -DSDL3_DIR=../SDL/build \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$SDL3_EXPECTED_SHA" > cache.txt

   cd ..
fi

#
# libdmdutil
#

CACHE_NAME="libdmdutil-${LIBDMDUTIL_SHA}"

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
   ./platforms/win/x86/external.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$LIBDMDUTIL_EXPECTED_SHA" > cache.txt

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
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
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

   platforms/win/x86/external.sh
   cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build --config ${BUILD_TYPE}
   cd ..

   echo "$LIBPPUC_EXPECTED_SHA" > cache.txt

   cd ..
fi

cp SDL3/SDL/build/${BUILD_TYPE}/SDL3.lib ../third-party/build-libs/win-x86/
cp SDL3/SDL/build/${BUILD_TYPE}/SDL3.dll ../third-party/runtime-libs/win-x86/
cp -r SDL3/SDL/include/SDL3 ../third-party/include/

cp SDL3/SDL_image/build/${BUILD_TYPE}/SDL3_image.lib ../third-party/build-libs/win-x86/
cp SDL3/SDL_image/build/${BUILD_TYPE}/SDL3_image.dll ../third-party/runtime-libs/win-x86/
cp -r SDL3/SDL_image/include/SDL3_image ../third-party/include/

cp pinmame/pinmame/build/${BUILD_TYPE}/pinmame.lib ../third-party/build-libs/win-x86/
cp pinmame/pinmame/build/${BUILD_TYPE}/pinmame.dll ../third-party/runtime-libs/win-x86/
cp pinmame/pinmame/src/libpinmame/libpinmame.h ../third-party/include/
#cp pinmame/pinmame/src/libpinmame/pinmamedef.h ../third-party/include/

cp libdmdutil/libdmdutil/build/${BUILD_TYPE}/dmdutil.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/build/${BUILD_TYPE}/dmdutil.dll ../third-party/runtime-libs/win-x86/
cp -r libdmdutil/libdmdutil/include/DMDUtil ../third-party/include/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/zedmd.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/zedmd.dll ../third-party/runtime-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/include/ZeDMD.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/serum.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/serum.dll ../third-party/runtime-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/include/serum.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/include/serum-decode.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/libserialport.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/libserialport.dll ../third-party/runtime-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/pupdmd.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/pupdmd.dll ../third-party/runtime-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/include/pupdmd.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/sockpp.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/sockpp.dll ../third-party/runtime-libs/win-x86/
cp -r libdmdutil/libdmdutil/third-party/include/sockpp ../third-party/include/
cp libdmdutil/libdmdutil/third-party/build-libs/win/x86/cargs.lib ../third-party/build-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/runtime-libs/win/x86/cargs.dll ../third-party/runtime-libs/win-x86/
cp libdmdutil/libdmdutil/third-party/include/cargs.h ../third-party/include/
cp libdmdutil/libdmdutil/third-party/include/FrameUtil.h ../third-party/include/

cp libppuc/libppuc/src/PPUC.h ../third-party/include/
cp libppuc/libppuc/src/PPUC_structs.h ../third-party/include/
cp -r libppuc/libppuc/third-party/include/yaml-cpp ../third-party/include/
cp -r libppuc/libppuc/third-party/include/io-boards ../third-party/include/
cp libppuc/libppuc/build/${BUILD_TYPE}/ppuc.lib ../third-party/build-libs/win-x86/
cp libppuc/libppuc/build/${BUILD_TYPE}/ppuc.dll ../third-party/runtime-libs/win-x86/
cp -a libppuc/libppuc/third-party/build-libs/win/x86/yaml-cpp.lib ../third-party/build-libs/win-x86/
cp -a libppuc/libppuc/third-party/runtime-libs/win/x86/yaml-cpp.dll ../third-party/runtime-libs/win-x86/
