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
    curl -sL https://github.com/vpinball/libdmdutil/archive/${LIBDMDUTIL_SHA}.tar.gz -o libdmdutil-${LIBDMDUTIL_SHA}.tar.gz
    tar xzf libdmdutil-${LIBDMDUTIL_SHA}.tar.gz
    mv libdmdutil-${LIBDMDUTIL_SHA} libdmdutil
    cd libdmdutil
    BUILD_TYPE=${BUILD_TYPE} ./platforms/win/x64/external.sh
    cmake \
      -G "Visual Studio 17 2022" \
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    cp build/${BUILD_TYPE}/dmdutil64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/dmdutil64.dll ../tmp/runtime-libs/windows-x64
    cp -r include/DMDUtil ../tmp/include/
    cp build/${BUILD_TYPE}/zedmd64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/zedmd64.dll ../tmp/runtime-libs/windows-x64
    cp third-party/include/ZeDMD.h ../tmp/include
    cp build/${BUILD_TYPE}/serum64.lib ../tmp/build-libs/windows-x64
    cp build/Rele${BUILD_TYPE}ase/serum64.dll ../tmp/runtime-libs/windows-x64
    cp third-party/include/serum.h ../tmp/include
    cp third-party/include/serum-decode.h ../tmp/include
    cp build/${BUILD_TYPE}/libserialport64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/libserialport64.dll ../tmp/runtime-libs/windows-x64
    cp build/${BUILD_TYPE}/pupdmd64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/pupdmd64.dll ../tmp/runtime-libs/windows-x64
    cp third-party/include/pupdmd.h ../tmp/include
    cp build/${BUILD_TYPE}/sockpp64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/sockpp64.dll ../tmp/runtime-libs/windows-x64
    cp build/${BUILD_TYPE}/cargs64.lib ../tmp/build-libs/windows-x64
    cp build/${BUILD_TYPE}/cargs64.dll ../tmp/runtime-libs/windows-x64
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
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    cp build/${BUILD_TYPE}/pinmame64.lib ../../third-party/build-libs/win/x64/
    cp build/${BUILD_TYPE}/pinamme64.dll ../../third-party/runtime-libs/win/x64/
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
    BUILD_TYPE=${BUILD_TYPE} platforms/win/x64/external.sh
    cmake \
      -G "Visual Studio 17 2022" \
      -DPLATFORM=win \
      -DARCH=x64 \
      -DBUILD_SHARED=ON \
      -DBUILD_STATIC=OFF \
      -B build
    cmake --build build --config ${BUILD_TYPE}
    cp -a third-party/include/yaml-cpp ../../third-party/include/
    cp build/${BUILD_TYPE}/ppuc64.lib ../../third-party/build-libs/win/x64/
    cp build/${BUILD_TYPE}/ppuc64.dll ../../third-party/runtime-libs/win/x64/
    cp third-party/build-libs/win/x64/yaml-cpp* ../../third-party/runtime-libs/win/x64/
    cp third-party/runtime-libs/win/x64/yaml-cpp* ../../third-party/runtime-libs/win/x64/
    cd ..
    touch "../${CACHE_DIR}/${CACHE_NAME}.cache"
fi
