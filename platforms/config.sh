#!/bin/bash

set -e

SDL_SHA=f6864924f76e1a0b4abaefc76ae2ed22b1a8916e
SDL_IMAGE_SHA=11154afb7855293159588b245b446a4ef09e574f
LIBOPENAL_SHA=d3875f333fb6abe2f39d82caca329414871ae53b
PINMAME_SHA=c69f68aca1fe28d5bb65ab10a17c09fb2593d57b
LIBPPUC_SHA=043239534c5d629f19f3f3b25e197a02f25e47f3
LIBDMDUTIL_SHA=b54bd68958e271961159fd3ccbd113e5c155027d


if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
