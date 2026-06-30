#!/bin/bash

set -e

SDL_IMAGE_SHA=bec9134a26c7d0f31b36d6083c25296e04cabff5
SDL_MIXER_SHA=72a81869b45e249e8e67102db4e98dd2441f05a1
FLITE_SHA=6c9f20dc915b17f5619340069889db0aa007fcdc
ESPEAK_NG_SHA=1.52.0
LUA_VERSION=5.4.8
PINMAME_SHA=bf74d40ef837bdfc377c0266c0ef71b3ed59a751
PINMAME_NVRAM_MAPS_SHA=fa1086d57118e12f4802f3a9683c1e6acfb6ec6d
LIBPPUC_SHA=93afce7bfcef68a7766283fb129b0d85f6838229
LIBSDLDMD_SHA=72a7e9777af59fe430c6f6ae77159e0b8c7301b4

PPUC_SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PPUC_LOCAL_DEPS_ROOT="${PPUC_LOCAL_DEPS_ROOT:-$(cd "${PPUC_SOURCE_ROOT}/.." && pwd)}"
PPUC_USE_LOCAL_DEPS="${PPUC_USE_LOCAL_DEPS:-1}"
PPUC_LOCAL_DEPS_CACHE_BUSTER="${PPUC_LOCAL_DEPS_CACHE_BUSTER:-$(date +%s)}"

ppuc_local_dependency_dir() {
   local name="$1"
   local dir="${PPUC_LOCAL_DEPS_ROOT}/${name}"

   if [ "${PPUC_USE_LOCAL_DEPS}" != "0" ] && [ -d "${dir}" ] && [ "${dir}" != "${PPUC_SOURCE_ROOT}" ]; then
      echo "${dir}"
   fi
}

ppuc_dependency_cache_key() {
   local name="$1"
   local sha="$2"
   local local_dir

   local_dir="$(ppuc_local_dependency_dir "${name}")"
   if [ -n "${local_dir}" ]; then
      echo "local:${local_dir}:${PPUC_LOCAL_DEPS_CACHE_BUSTER}"
   else
      echo "${sha}"
   fi
}

ppuc_print_dependency_source() {
   local label="$1"
   local name="$2"
   local sha="$3"
   local local_dir

   local_dir="$(ppuc_local_dependency_dir "${name}")"
   if [ -n "${local_dir}" ]; then
      echo "  ${label}_SOURCE: local ${local_dir}"
   else
      echo "  ${label}_SOURCE: archive ${sha}"
   fi
}

ppuc_prepare_dependency_source() {
   local name="$1"
   local sha="$2"
   local url="$3"
   local local_dir

   local_dir="$(ppuc_local_dependency_dir "${name}")"
   if [ -n "${local_dir}" ]; then
      echo "Using local ${name}: ${local_dir}"
      ln -s "${local_dir}" "${name}"
   else
      curl -sL "${url}" -o "${name}-${sha}.tar.gz"
      tar xzf "${name}-${sha}.tar.gz"
      mv "${name}-${sha}" "${name}"
   fi
}

ppuc_stage_lua_source() {
   local expected="${LUA_VERSION}"
   local found

   found="$([ -f lua/cache.txt ] && cat lua/cache.txt || echo "")"
   if [ "${expected}" != "${found}" ] || [ ! -f lua/lua/src/lua.h ]; then
      echo "Preparing Lua. Expected: ${expected}, Found: ${found}"

      rm -rf lua
      mkdir lua
      cd lua

      ppuc_prepare_dependency_source lua "${LUA_VERSION}" "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
      echo "${expected}" > cache.txt

      cd ..
   fi

   mkdir -p ../third-party/include/lua ../third-party/lua-src
   cp lua/lua/src/*.h ../third-party/include/lua/
   cp lua/lua/src/*.h ../third-party/lua-src/
   cp lua/lua/src/*.c ../third-party/lua-src/
}

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
