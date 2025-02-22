#!/bin/bash

set -e

LIBOPENAL_SHA=d3875f333fb6abe2f39d82caca329414871ae53b
LIBPINMAME_SHA=c69f68aca1fe28d5bb65ab10a17c09fb2593d57b
LIBPPUC_SHA=879eb844b8ba7bcbe68d07bc3f4759d2ccf5f754
LIBDMDUTIL_SHA=b54bd68958e271961159fd3ccbd113e5c155027d

echo "Building libraries..."
echo "  LIBOPENAL_SHA: ${LIBOPENAL_SHA}"
echo "  LIBPINMAME_SHA: ${LIBPINMAME_SHA}"
echo "  LIBPPUC_SHA: ${LIBPPUC_SHA}"
echo "  LIBDMDUTIL_SHA: ${LIBDMDUTIL_SHA}"
echo ""

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

if [ -z "${CACHE_DIR}" ]; then
   CACHE_DIR="external/cache/${BUILD_TYPE}"
fi

echo "Build type: ${BUILD_TYPE}"
echo "Cache dir: ${CACHE_DIR}"
echo ""

mkdir -p external ${CACHE_DIR}
cd external

#
# libdmdutil
#

CACHE_NAME="libdmdutil-${LIBDMDUTIL_SHA}"

if [ ! -f "../${CACHE_DIR}/${CACHE_NAME}.cache" ]; then
    rm -f ../${CACHE_DIR}/libdmdutil-*.cache
    rm -rf libdmdutil-*
    curl -sL https://github.com/PPUC/libdmdutil/archive/${LIBDMDUTIL_SHA}.zip -o libdmdutil.zip
    unzip libdmdutil.zip
    cd libdmdutil-${LIBDMDUTIL_SHA}
    cp -r include/DMDUtil ../../third-party/include/
    BUILD_TYPE=${BUILD_TYPE} platforms/win/x86/external.sh
    cp -a third-party/. ../../third-party
    cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=ON \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    ls -l build/${BUILD_TYPE}
    cp build/${BUILD_TYPE}/dmdutil.lib ../../third-party/build-libs/win/x86/
    cp build/${BUILD_TYPE}/dmdutil.dll ../../third-party/runtime-libs/win/x86/
    cd ..
    touch "../${CACHE_DIR}/${CACHE_NAME}.cache"
fi

#
# libpiname
#

CACHE_NAME="pinmame-${LIBPINMAME_SHA}"

if [ ! -f "../${CACHE_DIR}/${CACHE_NAME}.cache" ]; then
    rm -f ../${CACHE_DIR}/pinmame-*.cache
    rm -rf pinmame-*
    curl -sL https://github.com/vpinball/pinmame/archive/${LIBPINMAME_SHA}.zip -o pinmame.zip
    unzip pinmame.zip
    cd pinmame-${LIBPINMAME_SHA}
    cp src/libpinmame/libpinmame.h ../../third-party/include/
    cp cmake/libpinmame/CMakeLists.txt CMakeLists.txt
    cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=ON \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    cp build/${BUILD_TYPE}/pinmame.lib ../../third-party/build-libs/win/x86/
    cp build/${BUILD_TYPE}/pinamme.dll ../../third-party/runtime-libs/win/x86/
    cd ..
    touch "../${CACHE_DIR}/${CACHE_NAME}.cache"
fi

#
# libppuc
#

CACHE_NAME="libppuc-${LIBPPUC_SHA}"

if [ ! -f "../${CACHE_DIR}/${CACHE_NAME}.cache" ]; then
    rm -f ../${CACHE_DIR}/libppuc-*.cache
    rm -rf libppuc-*
    curl -sL https://github.com/PPUC/libppuc/archive/${LIBPPUC_SHA}.zip -o libppuc.zip
    unzip libppuc.zip
    cd libppuc-${LIBPPUC_SHA}
    cp src/PPUC.h ../../third-party/include/
    cp src/PPUC_structs.h ../../third-party/include/
    BUILD_TYPE=${BUILD_TYPE} platforms/win/x86/external.sh
    cp -a third-party/. ../../third-party
    cmake \
      -G "Visual Studio 17 2022" \
      -A Win32 \
      -DPLATFORM=win \
      -DARCH=x86 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=ON \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    cp build/${BUILD_TYPE}/ppuc.lib ../../third-party/build-libs/win/x86/
    cp build/${BUILD_TYPE}/ppuc.dll ../../third-party/runtime-libs/win/x86/
    cd ..
    touch "../${CACHE_DIR}/${CACHE_NAME}.cache"
fi
