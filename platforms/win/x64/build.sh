#!/bin/bash

set -e

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

BUILD_TYPE=${BUILD_TYPE} ./platforms/win/x64/external.sh

cmake -DPLATFORM=win -DARCH=x64 -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -B build
cmake --build build

rm -rf ppuc
mkdir ppuc

cp build/ppuc_pinmame ppuc/
cp -P third-party/runtime-libs/win/x64/*.dll ppuc/
