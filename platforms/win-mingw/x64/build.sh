#!/bin/bash

set -e

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

BUILD_TYPE=${BUILD_TYPE} ./platforms/win-mingw/x64/external.sh

cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DPLATFORM=win-mingw -DARCH=x64 -B build
cmake --build build -- -j$(nproc)

rm -rf ppuc
mkdir ppuc

cp build/ppuc-pinmame ppuc/
cp build/ppuc-menu ppuc/
cp build/ppuc-backbox ppuc/
cp -P third-party/runtime-libs/win-mingw-x64/*.dll ppuc/
cp -R third-party/pinmame-nvram-maps ppuc/
