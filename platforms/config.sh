#!/bin/bash

set -e

SDL_IMAGE_SHA=bec9134a26c7d0f31b36d6083c25296e04cabff5
SDL_MIXER_SHA=72a81869b45e249e8e67102db4e98dd2441f05a1
FLITE_SHA=6c9f20dc915b17f5619340069889db0aa007fcdc
ESPEAK_NG_SHA=1.52.0
LUA_VERSION=5.4.8
PINMAME_SHA=bf74d40ef837bdfc377c0266c0ef71b3ed59a751
PINMAME_NVRAM_MAPS_SHA=fa1086d57118e12f4802f3a9683c1e6acfb6ec6d
LIBPPUC_SHA=715dd76055e2552b31dcff35508721bf0445a396
LIBSDLDMD_SHA=87c804e1ae53b86846dfb2e8a8fafb908e417e82
VPINBALL_SHA=9947367ced86164c7944236b62b3e24ce1161c58
VPINBALL_SDL_SHA=8e37db5e797b6167f3a00d697d816a684bd259c7
VPINBALL_SDL_IMAGE_SHA=96a73a551a857b7c8d0ca3cc553a266eabbab6a7
VPINBALL_SDL_TTF_SHA=a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
VPINBALL_LIBALTSOUND_SHA=f4b790a19ae45a9f93ae0051df6933800c7a6446
VPINBALL_FFMPEG_SHA=239f2c733de417201d7ad3b3b8b0d9b63285b2b1

PPUC_SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
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

ppuc_macos_deployment_target() {
   if [ -n "${PPUC_MACOS_DEPLOYMENT_TARGET}" ]; then
      echo "${PPUC_MACOS_DEPLOYMENT_TARGET}"
   elif [ -n "${MACOSX_DEPLOYMENT_TARGET}" ]; then
      echo "${MACOSX_DEPLOYMENT_TARGET}"
   elif command -v xcrun >/dev/null 2>&1; then
      xcrun --sdk macosx --show-sdk-version 2>/dev/null || echo "14.0"
   else
      echo "14.0"
   fi
}

ppuc_vpinball_root() {
   echo "${PPUC_SOURCE_ROOT}/external/vpinball/vpinball"
}

ppuc_stage_vpinball_source() {
   local expected
   local found
   local vpinball_cache_dir
   local vpinball_source_dir

   if [ "${PPUC_BUILD_VPINBALL_MEDIA_PLUGINS:-1}" = "0" ]; then
      return 0
   fi

   expected="$(ppuc_dependency_cache_key vpinball "${VPINBALL_SHA}")"
   vpinball_cache_dir="${PPUC_SOURCE_ROOT}/external/vpinball"
   vpinball_source_dir="${vpinball_cache_dir}/vpinball"
   found="$([ -f "${vpinball_cache_dir}/cache.txt" ] && cat "${vpinball_cache_dir}/cache.txt" || echo "")"

   if [ "${expected}" != "${found}" ] || [ ! -f "${vpinball_source_dir}/CMakeLists.txt" ]; then
      echo "Preparing vpinball. Expected: ${expected}, Found: ${found}"

      rm -rf "${vpinball_cache_dir}"
      mkdir -p "${vpinball_cache_dir}"
      (
         cd "${vpinball_cache_dir}"
         ppuc_prepare_dependency_source vpinball "${VPINBALL_SHA}" "https://github.com/PPUC/vpinball/archive/${VPINBALL_SHA}.tar.gz"
         echo "${expected}" > cache.txt
      )
   fi
}

ppuc_vpinball_platform_external() {
   local platform="$1"
   local arch="$2"
   local vpinball_root

   vpinball_root="$(ppuc_vpinball_root)"
   echo "${vpinball_root}/platforms/${platform}-${arch}/external.sh"
}

ppuc_vpinball_media_num_procs() {
   if command -v nproc >/dev/null 2>&1; then
      nproc
   else
      sysctl -n hw.ncpu
   fi
}

ppuc_vpinball_media_glob_copy() {
   local pattern="$1"
   local dest="$2"
   local matches

   matches=( ${pattern} )
   if [ "${#matches[@]}" -gt 0 ] && [ -e "${matches[0]}" ]; then
      cp -a "${matches[@]}" "${dest}"
   fi
}

ppuc_vpinball_media_cmake_platform_args() {
   local platform="$1"
   local arch="$2"

   if [ "${platform}" = "macos" ]; then
      local macos_deployment_target
      macos_deployment_target="$(ppuc_macos_deployment_target)"
      if [ "${arch}" = "x64" ]; then
         echo "-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=${macos_deployment_target}"
      else
         echo "-DCMAKE_OSX_ARCHITECTURES=${arch} -DCMAKE_OSX_DEPLOYMENT_TARGET=${macos_deployment_target}"
      fi
   fi
}

ppuc_prepare_vpinball_media_dependencies() {
   local platform="$1"
   local arch="$2"
   local platform_tag="${platform}-${arch}"
   local expected
   local found
   local deps_root
   local vpinball_root
   local runtime_dir
   local include_dir
   local num_procs
   local cmake_platform_args

   vpinball_root="$(ppuc_vpinball_root)"
   deps_root="${PPUC_SOURCE_ROOT}/external/vpinball/media-deps/${platform_tag}/${BUILD_TYPE}"
   runtime_dir="${vpinball_root}/third-party/runtime-libs/${platform_tag}"
   include_dir="${vpinball_root}/third-party/include"
   num_procs="$(ppuc_vpinball_media_num_procs)"
   cmake_platform_args="$(ppuc_vpinball_media_cmake_platform_args "${platform}" "${arch}")"
   if [ "${platform}" = "macos" ]; then
      export MACOSX_DEPLOYMENT_TARGET="$(ppuc_macos_deployment_target)"
   fi

   mkdir -p "${deps_root}" "${runtime_dir}" "${include_dir}"

   expected="${VPINBALL_SDL_SHA}-${VPINBALL_SDL_IMAGE_SHA}-${VPINBALL_SDL_TTF_SHA}"
   found="$([ -f "${deps_root}/SDL3/cache.txt" ] && cat "${deps_root}/SDL3/cache.txt" || echo "")"
   if [ "${expected}" != "${found}" ]; then
      echo "Building VPX media SDL stack. Expected: ${expected}, Found: ${found}"
      rm -rf "${deps_root}/SDL3"
      mkdir -p "${deps_root}/SDL3"
      (
         cd "${deps_root}/SDL3"
         curl -sL "https://github.com/libsdl-org/SDL/archive/${VPINBALL_SDL_SHA}.tar.gz" -o "SDL-${VPINBALL_SDL_SHA}.tar.gz"
         tar xzf "SDL-${VPINBALL_SDL_SHA}.tar.gz"
         mv "SDL-${VPINBALL_SDL_SHA}" SDL
         cmake -S SDL -B SDL/build \
            -DSDL_SHARED=ON \
            -DSDL_STATIC=OFF \
            -DSDL_TEST_LIBRARY=OFF \
            -DSDL_OPENGLES=OFF \
            ${cmake_platform_args} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
         cmake --build SDL/build -- -j"${num_procs}"

         curl -sL "https://github.com/libsdl-org/SDL_image/archive/${VPINBALL_SDL_IMAGE_SHA}.tar.gz" -o "SDL_image-${VPINBALL_SDL_IMAGE_SHA}.tar.gz"
         tar xzf "SDL_image-${VPINBALL_SDL_IMAGE_SHA}.tar.gz"
         mv "SDL_image-${VPINBALL_SDL_IMAGE_SHA}" SDL_image
         ( cd SDL_image && ./external/download.sh )
         cmake -S SDL_image -B SDL_image/build \
            -DBUILD_SHARED_LIBS=ON \
            -DSDLIMAGE_SAMPLES=OFF \
            -DSDLIMAGE_DEPS_SHARED=ON \
            -DSDLIMAGE_VENDORED=ON \
            -DSDLIMAGE_AVIF=OFF \
            -DSDLIMAGE_WEBP=OFF \
            -DSDL3_DIR="${deps_root}/SDL3/SDL/build" \
            ${cmake_platform_args} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
         cmake --build SDL_image/build -- -j"${num_procs}"

         curl -sL "https://github.com/libsdl-org/SDL_ttf/archive/${VPINBALL_SDL_TTF_SHA}.tar.gz" -o "SDL_ttf-${VPINBALL_SDL_TTF_SHA}.tar.gz"
         tar xzf "SDL_ttf-${VPINBALL_SDL_TTF_SHA}.tar.gz"
         mv "SDL_ttf-${VPINBALL_SDL_TTF_SHA}" SDL_ttf
         ( cd SDL_ttf && ./external/download.sh )
         cmake -S SDL_ttf -B SDL_ttf/build \
            -DBUILD_SHARED_LIBS=ON \
            -DSDLTTF_SAMPLES=OFF \
            -DSDLTTF_VENDORED=ON \
            -DSDLTTF_HARFBUZZ=ON \
            -DSDL3_DIR="${deps_root}/SDL3/SDL/build" \
            ${cmake_platform_args} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
         cmake --build SDL_ttf/build -- -j"${num_procs}"
         echo "${expected}" > cache.txt
      )
   fi

   if [ "${platform}" = "macos" ]; then
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL/build/libSDL3*.dylib" "${runtime_dir}"
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL_image/build/libSDL3_image*.dylib" "${runtime_dir}"
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL_ttf/build/libSDL3_ttf*.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL/build/libSDL3.so*" "${runtime_dir}"
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL_image/build/libSDL3_image.so*" "${runtime_dir}"
      ppuc_vpinball_media_glob_copy "${deps_root}/SDL3/SDL_ttf/build/libSDL3_ttf.so*" "${runtime_dir}"
   fi
   cp -r "${deps_root}/SDL3/SDL/include/SDL3" "${include_dir}/"
   cp -r "${deps_root}/SDL3/SDL_image/include/SDL3_image" "${include_dir}/"
   cp -r "${deps_root}/SDL3/SDL_ttf/include/SDL3_ttf" "${include_dir}/"

   expected="${VPINBALL_LIBALTSOUND_SHA}$([ "${platform}" = "macos" ] && echo "-macos${MACOSX_DEPLOYMENT_TARGET}")"
   found="$([ -f "${deps_root}/libaltsound/cache.txt" ] && cat "${deps_root}/libaltsound/cache.txt" || echo "")"
   if [ "${expected}" != "${found}" ]; then
      echo "Building VPX media libaltsound. Expected: ${expected}, Found: ${found}"
      rm -rf "${deps_root}/libaltsound"
      mkdir -p "${deps_root}/libaltsound"
      (
         cd "${deps_root}/libaltsound"
         curl -sL "https://github.com/vpinball/libaltsound/archive/${VPINBALL_LIBALTSOUND_SHA}.tar.gz" -o "libaltsound-${VPINBALL_LIBALTSOUND_SHA}.tar.gz"
         tar xzf "libaltsound-${VPINBALL_LIBALTSOUND_SHA}.tar.gz"
         mv "libaltsound-${VPINBALL_LIBALTSOUND_SHA}" libaltsound
         cmake -S libaltsound -B libaltsound/build \
            -DPLATFORM="${platform}" \
            -DARCH="${arch}" \
            -DBUILD_STATIC=OFF \
            ${cmake_platform_args} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
         cmake --build libaltsound/build -- -j"${num_procs}"
         echo "${expected}" > cache.txt
      )
   fi

   if [ "${platform}" = "macos" ]; then
      ppuc_vpinball_media_glob_copy "${deps_root}/libaltsound/libaltsound/build/libaltsound*.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${deps_root}/libaltsound/libaltsound/build/libaltsound.so*" "${runtime_dir}"
   fi
   cp "${deps_root}/libaltsound/libaltsound/src/altsound.h" "${include_dir}/"

   expected="${VPINBALL_FFMPEG_SHA}$([ "${platform}" = "macos" ] && echo "-macos${MACOSX_DEPLOYMENT_TARGET}")"
   found="$([ -f "${deps_root}/ffmpeg/cache.txt" ] && cat "${deps_root}/ffmpeg/cache.txt" || echo "")"
   if [ "${expected}" != "${found}" ]; then
      echo "Building VPX media ffmpeg. Expected: ${expected}, Found: ${found}"
      rm -rf "${deps_root}/ffmpeg"
      mkdir -p "${deps_root}/ffmpeg"
      (
         cd "${deps_root}/ffmpeg"
         curl -sL "https://github.com/FFmpeg/FFmpeg/archive/${VPINBALL_FFMPEG_SHA}.tar.gz" -o "FFmpeg-${VPINBALL_FFMPEG_SHA}.tar.gz"
         tar xzf "FFmpeg-${VPINBALL_FFMPEG_SHA}.tar.gz"
         mv "FFmpeg-${VPINBALL_FFMPEG_SHA}" ffmpeg
         cd ffmpeg
         if [ "${platform}" = "macos" ]; then
            local ffmpeg_arch="${arch}"
            local extra_ldflags="-mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
            if [ "${arch}" = "x64" ]; then
               ffmpeg_arch="x86_64"
               extra_ldflags="-Wl,-no_fixup_chains -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}"
            fi
            ./configure --enable-cross-compile \
               --enable-shared \
               --disable-static \
               --disable-programs \
               --disable-doc \
               --disable-xlib \
               --disable-libxcb \
               --enable-rpath \
               --prefix=. \
               --libdir=@rpath \
               --arch="${ffmpeg_arch}" \
               --cc="clang -arch ${ffmpeg_arch}" \
               --extra-cflags="-mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET}" \
               --extra-ldflags="${extra_ldflags}"
         else
            LDFLAGS=-Wl,-rpath,'$$ORIGIN' ./configure \
               --enable-shared \
               --disable-static \
               --disable-programs \
               --disable-doc
         fi
         make -j"${num_procs}"
         cd ..
         echo "${expected}" > cache.txt
      )
   fi

   for lib in libavcodec libavformat libavutil libswresample libswscale; do
      if [ "${platform}" = "macos" ]; then
         ppuc_vpinball_media_glob_copy "${deps_root}/ffmpeg/ffmpeg/${lib}/${lib}*.dylib" "${runtime_dir}"
      else
         ppuc_vpinball_media_glob_copy "${deps_root}/ffmpeg/ffmpeg/${lib}/${lib}.so*" "${runtime_dir}"
      fi
      mkdir -p "${include_dir}/${lib}"
      cp "${deps_root}/ffmpeg/ffmpeg/${lib}"/*.h "${include_dir}/${lib}/"
   done

   if [ "${platform}" = "macos" ]; then
      ppuc_vpinball_media_glob_copy "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libpupdmd*.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libpupdmd.so*" "${runtime_dir}"
   fi
   cp "${PPUC_SOURCE_ROOT}/third-party/include/pupdmd.h" "${include_dir}/"
}

ppuc_prepare_vpinball_media_plugins() {
   local platform="$1"
   local arch="$2"
   local vpinball_root
   local vpinball_external

   if [ "${PPUC_BUILD_VPINBALL_MEDIA_PLUGINS:-1}" = "0" ]; then
      return 0
   fi

   ppuc_stage_vpinball_source

   vpinball_root="$(ppuc_vpinball_root)"
   if [ ! -f "${vpinball_root}/CMakeLists.txt" ]; then
      echo "Skipping VPX media plugin externals: vpinball source not found at ${vpinball_root}" >&2
      return 0
   fi

   vpinball_external="$(ppuc_vpinball_platform_external "${platform}" "${arch}")"
   if [ ! -f "${vpinball_external}" ]; then
      echo "Skipping VPX media plugin externals: unsupported vpinball platform ${platform}-${arch}" >&2
      return 0
   fi

   echo "Preparing VPX media plugin dependencies for PPUC: ${platform}-${arch}"
   ppuc_prepare_vpinball_media_dependencies "${platform}" "${arch}"
}

ppuc_build_vpinball_media_plugins() {
   local platform="$1"
   local arch="$2"
   local vpinball_root
   local vpinball_external
   local plugin_package_dir
   local vpinball_build_dir
   local cmake_platform_args

   if [ "${PPUC_BUILD_VPINBALL_MEDIA_PLUGINS:-1}" = "0" ]; then
      return 0
   fi

   vpinball_root="$(ppuc_vpinball_root)"
   if [ ! -f "${vpinball_root}/CMakeLists.txt" ]; then
      echo "Skipping VPX media plugins: vpinball source not found at ${vpinball_root}" >&2
      return 0
   fi

   vpinball_external="$(ppuc_vpinball_platform_external "${platform}" "${arch}")"
   if [ ! -f "${vpinball_external}" ]; then
      echo "Skipping VPX media plugins: unsupported vpinball platform ${platform}-${arch}" >&2
      return 0
   fi

   plugin_package_dir="${PPUC_SOURCE_ROOT}/ppuc/plugins"
   vpinball_build_dir="${PPUC_SOURCE_ROOT}/external/vpinball/build-${platform}-${arch}"
   cmake_platform_args="$(ppuc_vpinball_media_cmake_platform_args "${platform}" "${arch}")"

   echo "Building VPX media plugins for PPUC: ${platform}-${arch}"
   mkdir -p "${plugin_package_dir}"
   if [ "${platform}" = "macos" ]; then
      export MACOSX_DEPLOYMENT_TARGET="$(ppuc_macos_deployment_target)"
   fi
   cmake \
      -S "${vpinball_root}" \
      -B "${vpinball_build_dir}" \
      -DPLATFORM="${platform}" \
      -DARCH="${arch}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DPOST_BUILD_COPY_EXT_LIBS=ON \
      ${cmake_platform_args} \
      -DVPINBALL_PPUC_PLUGIN_PACKAGE_DIR="${plugin_package_dir}"
   cmake --build "${vpinball_build_dir}" --target PPUCMediaPluginBundle
}

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
