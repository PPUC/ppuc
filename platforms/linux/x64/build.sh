#!/bin/bash

set -e

source ./platforms/config.sh

BUILD_TYPE=${BUILD_TYPE} ./platforms/linux/x64/external.sh

cmake -DPLATFORM=linux -DARCH=x64 -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -B build
cmake --build build

rm -rf ppuc
mkdir ppuc

cp build/ppuc-pinmame ppuc/
cp build/ppuc-menu ppuc/
cp build/ppuc-backbox ppuc/
cp -P third-party/runtime-libs/linux-x64/*.so* ppuc/
cp -R third-party/pinmame-nvram-maps ppuc/

ppuc_build_vpinball_media_plugins linux x64
