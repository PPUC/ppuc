#!/bin/bash

set -e

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

BUILD_TYPE=${BUILD_TYPE} ./platforms/linux/aarch64/external.sh

cmake -DPLATFORM=linux -DARCH=aarch64 -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -B build
cmake --build build

rm -rf ppuc
mkdir ppuc

cp build/ppuc-pinmame ppuc/
cp build/ppuc-backbox ppuc/
cp -P third-party/runtime-libs/linux-aarch64/*.so* ppuc/
