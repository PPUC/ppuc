#!/bin/bash

set -e

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

BUILD_TYPE=${BUILD_TYPE} ./platforms/macos/x64/external.sh

cmake -DPLATFORM=macos -DARCH=x64 -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -B build
cmake --build build

rm -rf ppuc
mkdir ppuc

cp build/ppuc-pinmame ppuc/
cp build/ppuc-backbox ppuc/
cp -P third-party/runtime-libs/macos-x64/*.dylib ppuc/
