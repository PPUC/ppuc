#!/bin/bash

set -e

source ./platforms/config.sh

NUM_PROCS=$(nproc)

echo "Building libraries..."
echo "  SDL_IMAGE_SHA: ${SDL_IMAGE_SHA}"
echo "  PINMAME_SHA: ${PINMAME_SHA}"
echo "  LIBPPUC_SHA: ${LIBPPUC_SHA}"
echo "  LIBSDLDMD_SHA: ${LIBSDLDMD_SHA}"
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

LIBSDLDMD_EXPECTED_SHA="${LIBSDLDMD_SHA}"
LIBSDLDMD_FOUND_SHA="$([ -f libsdldmd/cache.txt ] && cat libsdldmd/cache.txt || echo "")"

if [ -n "${LIBSDLDMD_SHA}" ] && [ "${LIBSDLDMD_EXPECTED_SHA}" != "${LIBSDLDMD_FOUND_SHA}" ]; then
   echo "Building libsdldmd. Expected: ${LIBSDLDMD_EXPECTED_SHA}, Found: ${LIBSDLDMD_FOUND_SHA}"

   rm -rf libsdldmd
   mkdir libsdldmd
   cd libsdldmd

   curl -sL https://github.com/PPUC/libsdldmd/archive/${LIBSDLDMD_SHA}.tar.gz -o libsdldmd-${LIBSDLDMD_SHA}.tar.gz
   tar xzf libsdldmd-${LIBSDLDMD_SHA}.tar.gz
   mv libsdldmd-${LIBSDLDMD_SHA} libsdldmd
   cd libsdldmd

   BUILD_TYPE=${BUILD_TYPE} platforms/win-mingw/x64/external.sh
   cmake \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DPLATFORM=win-mingw \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build -- -j${NUM_PROCS}
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
   ./external/download.sh
   sed -i.bak 's/OUTPUT_NAME "SDL3_image"/OUTPUT_NAME "SDL3_image64"/g' CMakeLists.txt
   cmake \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DBUILD_SHARED_LIBS=ON \
      -DSDLIMAGE_SAMPLES=OFF \
      -DSDLIMAGE_DEPS_SHARED=ON \
      -DSDLIMAGE_VENDORED=ON \
      -DSDLIMAGE_AVIF=OFF \
      -DSDLIMAGE_WEBP=OFF \
      -DSDL3_DIR=../../libsdldmd/libsdldmd/external/SDL/build \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$SDL3_IMAGE_EXPECTED_SHA" > cache.txt

   cd ..
fi

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
   cp CMakeLists.txt CMakeLists.txt.orig 2>/dev/null || true
   cp cmake/libpinmame/CMakeLists.txt .
   cmake \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DPLATFORM=win-mingw \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$PINMAME_EXPECTED_SHA" > cache.txt

   cd ..
fi

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

   BUILD_TYPE=${BUILD_TYPE} platforms/win-mingw/x64/external.sh
   cmake \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DPLATFORM=win-mingw \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
   cmake --build build -- -j${NUM_PROCS}
   cd ..

   echo "$LIBPPUC_EXPECTED_SHA" > cache.txt

   cd ..
fi

cp libsdldmd/libsdldmd/third-party/build-libs/win-mingw/x64/SDL364.dll.a ../third-party/build-libs/win-mingw-x64/
cp libsdldmd/libsdldmd/third-party/runtime-libs/win-mingw/x64/SDL364.dll ../third-party/runtime-libs/win-mingw-x64/
cp -r libsdldmd/libsdldmd/third-party/include/SDL3 ../third-party/include/

cp SDL3_image/SDL_image/build/libSDL3_image64.dll.a ../third-party/build-libs/win-mingw-x64/
cp SDL3_image/SDL_image/build/SDL3_image64.dll ../third-party/runtime-libs/win-mingw-x64/
cp -r SDL3_image/SDL_image/include/SDL3_image ../third-party/include/

cp pinmame/pinmame/build/libpinmame.dll.a ../third-party/build-libs/win-mingw-x64/pinmame64.dll.a
cp pinmame/pinmame/build/libpinmame.dll ../third-party/runtime-libs/win-mingw-x64/pinmame64.dll
cp pinmame/pinmame/src/libpinmame/libpinmame.h ../third-party/include/

if [ -n "${LIBSDLDMD_SHA}" ]; then
   LIBSDLDMD_DMDUTIL_THIRD_PARTY="libsdldmd/libsdldmd/external/libdmdutil/third-party"

   cp libsdldmd/libsdldmd/third-party/build-libs/win-mingw/x64/dmdutil64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp libsdldmd/libsdldmd/third-party/runtime-libs/win-mingw/x64/dmdutil64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp -r libsdldmd/libsdldmd/third-party/include/DMDUtil ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/libusb64-1.0.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/libusb64-1.0.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/vni64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/vni64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/zedmd64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/zedmd64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/ZeDMD.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/serum64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/serum64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/serum-decode.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/libserialport64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/libserialport64-0.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/pupdmd64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/pupdmd64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/pupdmd.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/libsockpp64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/libsockpp64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp -r ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/sockpp ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/build-libs/win-mingw/x64/libcargs64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/runtime-libs/win-mingw/x64/libcargs64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/cargs.h ../third-party/include/
   cp ${LIBSDLDMD_DMDUTIL_THIRD_PARTY}/include/FrameUtil.h ../third-party/include/
   cp libsdldmd/libsdldmd/build/sdldmd64.dll.a ../third-party/build-libs/win-mingw-x64/
   cp libsdldmd/libsdldmd/build/sdldmd64.dll ../third-party/runtime-libs/win-mingw-x64/
   cp -r libsdldmd/libsdldmd/include/SDLDMD ../third-party/include/
fi

cp libppuc/libppuc/src/PPUC.h ../third-party/include/
cp libppuc/libppuc/src/PPUC_structs.h ../third-party/include/
cp -r libppuc/libppuc/third-party/include/yaml-cpp ../third-party/include/
cp -r libppuc/libppuc/third-party/include/io-boards ../third-party/include/
cp libppuc/libppuc/build/ppuc64.dll.a ../third-party/build-libs/win-mingw-x64/
cp libppuc/libppuc/build/ppuc64.dll ../third-party/runtime-libs/win-mingw-x64/
cp -a libppuc/libppuc/third-party/build-libs/win-mingw/x64/libyaml-cpp.dll.a ../third-party/build-libs/win-mingw-x64/
cp -a libppuc/libppuc/third-party/runtime-libs/win-mingw/x64/libyaml-cpp.dll ../third-party/runtime-libs/win-mingw-x64/

UCRT64_BIN="${MINGW_PREFIX}/bin"
cp "${UCRT64_BIN}/libgcc_s_seh-1.dll" ../third-party/runtime-libs/win-mingw-x64/
cp "${UCRT64_BIN}/libstdc++-6.dll" ../third-party/runtime-libs/win-mingw-x64/
cp "${UCRT64_BIN}/libwinpthread-1.dll" ../third-party/runtime-libs/win-mingw-x64/
