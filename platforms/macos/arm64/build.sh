#!/bin/bash

set -e

source ./platforms/config.sh

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(ppuc_macos_deployment_target)}"

BUILD_TYPE=${BUILD_TYPE} ./platforms/macos/arm64/external.sh

cmake \
   -DPLATFORM=macos \
   -DARCH=arm64 \
   -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build

rm -rf ppuc
mkdir ppuc

cp build/ppuc-pinmame ppuc/
cp build/ppuc-menu ppuc/
cp build/ppuc-backbox ppuc/
cp -P third-party/runtime-libs/macos-arm64/*.dylib ppuc/
cp -R third-party/pinmame-nvram-maps ppuc/

ppuc_build_vpinball_media_plugins macos arm64
