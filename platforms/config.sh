#!/bin/bash

set -e

if [ "${VERBOSE:-0}" = "1" ] || [ "${PPUC_VERBOSE:-0}" = "1" ]; then
   set -x
fi

SDL_IMAGE_SHA=bec9134a26c7d0f31b36d6083c25296e04cabff5
SDL_MIXER_SHA=72a81869b45e249e8e67102db4e98dd2441f05a1
FLITE_SHA=6c9f20dc915b17f5619340069889db0aa007fcdc
ESPEAK_NG_SHA=1.52.0
LUA_VERSION=5.4.8
PINMAME_SHA=443562a4de6f6bedaf166ca943dcfcf9221df8f8
PINMAME_NVRAM_MAPS_SHA=d8693b9ca59a1b871d2a473be3adb0392471a8e3
LIBPPUC_SHA=44515c17dbd731f5bba954e30f443ef7e6b12c75
LIBSDLDMD_SHA=2b48d459f0233caa6e0e680e6890421ccdc45396
VPINBALL_SHA=c0b065d443c4d6ca393679795f76159a0229418c
VPINBALL_SDL_SHA=f87239e71e42da91ca317a12eefb82cfbf3393eb
VPINBALL_SDL_IMAGE_SHA="${VPINBALL_SDL_IMAGE_SHA:-${SDL_IMAGE_SHA}}"
VPINBALL_SDL_TTF_SHA=a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
VPINBALL_LIBALTSOUND_SHA=f4b790a19ae45a9f93ae0051df6933800c7a6446
VPINBALL_FFMPEG_SHA=239f2c733de417201d7ad3b3b8b0d9b63285b2b1

PPUC_SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SOURCE_DIR_CACHE_BUSTER="${SOURCE_DIR_CACHE_BUSTER:-$(date +%s)}"
PPUC_DEPENDENCY_SOURCE="${PPUC_DEPENDENCY_SOURCE:-explicit}"
PPUC_LOCAL_SOURCE_ROOT="${PPUC_LOCAL_SOURCE_ROOT:-$(cd "${PPUC_SOURCE_ROOT}/.." && pwd -P)}"
PPUC_MANAGED_SOURCE_DIR_VARS=(
   LIBPPUC_SOURCE_DIR
   LIBSDLDMD_SOURCE_DIR
   VPINBALL_SOURCE_DIR
   IO_BOARDS_SOURCE_DIR
   LIBDMDUTIL_SOURCE_DIR
   LIBZEDMD_SOURCE_DIR
   LIBSERUM_SOURCE_DIR
   LIBVNI_SOURCE_DIR
   LIBFRAMEUTIL_SOURCE_DIR
)

set_dependency_source_dir_default() {
   local var_name="$1"
   local relative_dir="$2"

   if [ -z "${!var_name:-}" ]; then
      printf -v "${var_name}" '%s/%s' "${PPUC_LOCAL_SOURCE_ROOT}" "${relative_dir}"
   fi
   export "${var_name}"
}

configure_dependency_source_mode() {
   local source_var

   case "${PPUC_DEPENDENCY_SOURCE}" in
      explicit|manual|"")
         for source_var in "${PPUC_MANAGED_SOURCE_DIR_VARS[@]}"; do
            if [ -n "${!source_var:-}" ]; then
               export "${source_var}"
            fi
         done
         ;;
      local|source|sources)
         set_dependency_source_dir_default LIBPPUC_SOURCE_DIR libppuc
         set_dependency_source_dir_default LIBSDLDMD_SOURCE_DIR libsdldmd
         set_dependency_source_dir_default VPINBALL_SOURCE_DIR vpinball
         set_dependency_source_dir_default IO_BOARDS_SOURCE_DIR io-boards
         set_dependency_source_dir_default LIBDMDUTIL_SOURCE_DIR libdmdutil
         set_dependency_source_dir_default LIBZEDMD_SOURCE_DIR libzedmd
         set_dependency_source_dir_default LIBSERUM_SOURCE_DIR libserum
         set_dependency_source_dir_default LIBVNI_SOURCE_DIR libvni
         set_dependency_source_dir_default LIBFRAMEUTIL_SOURCE_DIR libframeutil
         ;;
      github|sha|archive|archives)
         for source_var in "${PPUC_MANAGED_SOURCE_DIR_VARS[@]}"; do
            unset "${source_var}"
         done
         ;;
      *)
         echo "Unsupported PPUC_DEPENDENCY_SOURCE: ${PPUC_DEPENDENCY_SOURCE}" >&2
         echo "Use explicit, local, or github." >&2
         exit 1
         ;;
   esac
}

configure_dependency_source_mode

dependency_source_dir() {
   local var_name="$1"
   local source_dir="${!var_name:-}"

   if [ -z "${source_dir}" ]; then
      return 0
   fi

   (cd "${PPUC_SOURCE_ROOT}" && cd "${source_dir}" && pwd -P)
}

dependency_cache_key() {
   local sha="$1"
   local source_var="$2"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "source:${source_dir}:${SOURCE_DIR_CACHE_BUSTER}"
   else
      echo "${sha}"
   fi
}

print_dependency_source() {
   local label="$1"
   local sha="$2"
   local source_var="$3"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "  ${label}_SOURCE_DIR: ${source_dir}"
   else
      echo "  ${label}_SOURCE: archive ${sha}"
   fi
}

prepare_dependency_source() {
   local name="$1"
   local sha="$2"
   local url="$3"
   local source_var="$4"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "Using ${source_var}: ${source_dir}"
      ln -s "${source_dir}" "${name}"
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

      prepare_dependency_source lua "${LUA_VERSION}" "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" LUA_SOURCE_DIR
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

   expected="$(dependency_cache_key "${VPINBALL_SHA}" VPINBALL_SOURCE_DIR)"
   vpinball_cache_dir="${PPUC_SOURCE_ROOT}/external/vpinball"
   vpinball_source_dir="${vpinball_cache_dir}/vpinball"
   found="$([ -f "${vpinball_cache_dir}/cache.txt" ] && cat "${vpinball_cache_dir}/cache.txt" || echo "")"

   if [ "${expected}" != "${found}" ] || [ ! -f "${vpinball_source_dir}/CMakeLists.txt" ]; then
      echo "Preparing vpinball. Expected: ${expected}, Found: ${found}"

      rm -rf "${vpinball_cache_dir}"
      mkdir -p "${vpinball_cache_dir}"
      (
         cd "${vpinball_cache_dir}"
         prepare_dependency_source vpinball "${VPINBALL_SHA}" "https://github.com/PPUC/vpinball/archive/${VPINBALL_SHA}.tar.gz" VPINBALL_SOURCE_DIR
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

ppuc_vpinball_media_required_glob_copy() {
   local pattern="$1"
   local dest="$2"
   local matches

   matches=( ${pattern} )
   if [ "${#matches[@]}" -eq 0 ] || [ ! -e "${matches[0]}" ]; then
      echo "Missing required media dependency artifact: ${pattern}" >&2
      return 1
   fi

   echo "Copying media dependency artifact(s): ${pattern} -> ${dest}"
   cp -a "${matches[@]}" "${dest}"
}

ppuc_vpinball_media_required_dir_copy() {
   local source_dir="$1"
   local dest="$2"

   if [ ! -d "${source_dir}" ]; then
      echo "Missing required media dependency include dir: ${source_dir}" >&2
      return 1
   fi

   echo "Copying media dependency include dir: ${source_dir} -> ${dest}"
   cp -r "${source_dir}" "${dest}"
}

ppuc_clean_runtime_lib_dir() {
   local dir="$1"
   local platform="$2"

   mkdir -p "${dir}"
   if [ "${platform}" = "macos" ]; then
      find "${dir}" -maxdepth 1 \( -type f -o -type l \) -name "*.dylib" -delete
   elif [[ "${platform}" = win* ]]; then
      find "${dir}" -maxdepth 1 \( -type f -o -type l \) -name "*.dll" -delete
   else
      find "${dir}" -maxdepth 1 \( -type f -o -type l \) -name "*.so*" -delete
   fi
}

ppuc_copy_dylib_link_chain() {
   local source_dir="$1"
   local dylib_name="$2"
   local dest_dir="$3"
   local current="${dylib_name}"
   local target
   local copied=0

   mkdir -p "${dest_dir}"
   while [ -n "${current}" ] && [ -e "${source_dir}/${current}" ]; do
      cp -P "${source_dir}/${current}" "${dest_dir}/"
      copied=1
      if [ ! -L "${source_dir}/${current}" ]; then
         break
      fi

      target="$(readlink "${source_dir}/${current}")"
      if [[ "${target}" = /* ]]; then
         current="$(basename "${target}")"
         source_dir="$(dirname "${target}")"
      else
         current="${target}"
      fi
   done

   if [ "${copied}" = "0" ]; then
      echo "Missing dylib chain source: ${source_dir}/${dylib_name}" >&2
      return 1
   fi
}

ppuc_relink_macos_dylib_alias() {
   local dir="$1"
   local alias="$2"
   local pattern="$3"
   local matches
   local target

   matches=( "${dir}"/${pattern} )
   if [ "${#matches[@]}" -eq 0 ] || [ ! -e "${matches[0]}" ]; then
      return 0
   fi

   target="$(basename "${matches[0]}")"
   rm -f "${dir}/${alias}"
   ln -s "${target}" "${dir}/${alias}"
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
   local ppuc_runtime_dir
   local ppuc_include_dir
   local ppuc_sdl3_cmake_dir
   local reuse_ppuc_sdl_stack
   local sdl3_cmake_dir
   local num_procs
   local cmake_platform_args

   vpinball_root="$(ppuc_vpinball_root)"
   deps_root="${PPUC_SOURCE_ROOT}/external/vpinball/media-deps/${platform_tag}/${BUILD_TYPE}"
   runtime_dir="${vpinball_root}/third-party/runtime-libs/${platform_tag}"
   include_dir="${vpinball_root}/third-party/include"
   ppuc_runtime_dir="${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}"
   ppuc_include_dir="${PPUC_SOURCE_ROOT}/third-party/include"
   ppuc_sdl3_cmake_dir="${PPUC_SOURCE_ROOT}/external/libsdldmd/libsdldmd/external/SDL/build"
   num_procs="$(ppuc_vpinball_media_num_procs)"
   cmake_platform_args="$(ppuc_vpinball_media_cmake_platform_args "${platform}" "${arch}")"
   if [ "${platform}" = "macos" ]; then
      export MACOSX_DEPLOYMENT_TARGET="$(ppuc_macos_deployment_target)"
   fi

   mkdir -p "${deps_root}" "${runtime_dir}" "${include_dir}"
   ppuc_clean_runtime_lib_dir "${runtime_dir}" "${platform}"

   reuse_ppuc_sdl_stack=0
   if [ -d "${ppuc_include_dir}/SDL3" ] && [ -d "${ppuc_include_dir}/SDL3_image" ] && \
      [ -f "${ppuc_sdl3_cmake_dir}/SDL3Config.cmake" ]; then
      if [ "${platform}" = "macos" ] && \
         ls "${ppuc_runtime_dir}"/libSDL3*.dylib >/dev/null 2>&1 && \
         ls "${ppuc_runtime_dir}"/libSDL3_image*.dylib >/dev/null 2>&1; then
         reuse_ppuc_sdl_stack=1
      elif [ "${platform}" != "macos" ] && \
         ls "${ppuc_runtime_dir}"/libSDL3.so* >/dev/null 2>&1 && \
         ls "${ppuc_runtime_dir}"/libSDL3_image.so* >/dev/null 2>&1; then
         reuse_ppuc_sdl_stack=1
      fi
   fi

   if [ "${reuse_ppuc_sdl_stack}" = "1" ]; then
      expected="reuse-ppuc-sdl-${VPINBALL_SDL_SHA}-${VPINBALL_SDL_IMAGE_SHA}-${VPINBALL_SDL_TTF_SHA}"
      sdl3_cmake_dir="${ppuc_sdl3_cmake_dir}"
   else
      expected="self-contained-sdl-${VPINBALL_SDL_SHA}-${VPINBALL_SDL_IMAGE_SHA}-${VPINBALL_SDL_TTF_SHA}"
      sdl3_cmake_dir="${deps_root}/SDL3/SDL/build"
   fi
   found="$([ -f "${deps_root}/SDL3/cache.txt" ] && cat "${deps_root}/SDL3/cache.txt" || echo "")"
   if [ "${expected}" != "${found}" ]; then
      if [ "${reuse_ppuc_sdl_stack}" = "1" ]; then
         echo "Building VPX media SDL_ttf and reusing PPUC SDL/SDL_image. Expected: ${expected}, Found: ${found}"
      else
         echo "Building VPX media SDL stack. Expected: ${expected}, Found: ${found}"
      fi
      rm -rf "${deps_root}/SDL3"
      mkdir -p "${deps_root}/SDL3"
      (
         cd "${deps_root}/SDL3"
         if [ "${reuse_ppuc_sdl_stack}" != "1" ]; then
            curl -sL "https://github.com/libsdl-org/SDL/archive/${VPINBALL_SDL_SHA}.tar.gz" -o "SDL-${VPINBALL_SDL_SHA}.tar.gz"
            tar xzf "SDL-${VPINBALL_SDL_SHA}.tar.gz"
            mv "SDL-${VPINBALL_SDL_SHA}" SDL
            cmake -S SDL -B SDL/build \
               -DSDL_SHARED=ON \
               -DSDL_STATIC=OFF \
               -DSDL_TEST_LIBRARY=OFF \
               -DSDL_OPENGLES=OFF \
               -DSDL_WAYLAND=OFF \
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
         fi

         curl -sL "https://github.com/libsdl-org/SDL_ttf/archive/${VPINBALL_SDL_TTF_SHA}.tar.gz" -o "SDL_ttf-${VPINBALL_SDL_TTF_SHA}.tar.gz"
         tar xzf "SDL_ttf-${VPINBALL_SDL_TTF_SHA}.tar.gz"
         mv "SDL_ttf-${VPINBALL_SDL_TTF_SHA}" SDL_ttf
         ( cd SDL_ttf && ./external/download.sh )
         cmake -S SDL_ttf -B SDL_ttf/build \
            -DBUILD_SHARED_LIBS=ON \
            -DSDLTTF_SAMPLES=OFF \
            -DSDLTTF_VENDORED=ON \
            -DSDLTTF_HARFBUZZ=ON \
            -DSDL3_DIR="${sdl3_cmake_dir}" \
            ${cmake_platform_args} \
            -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
         cmake --build SDL_ttf/build -- -j"${num_procs}"
         echo "${expected}" > cache.txt
      )
   fi

   if [ "${platform}" = "macos" ]; then
      if [ "${reuse_ppuc_sdl_stack}" = "1" ]; then
         ppuc_copy_dylib_link_chain "${ppuc_runtime_dir}" "libSDL3.dylib" "${runtime_dir}"
         ppuc_copy_dylib_link_chain "${ppuc_runtime_dir}" "libSDL3_image.dylib" "${runtime_dir}"
      else
         ppuc_copy_dylib_link_chain "${deps_root}/SDL3/SDL/build" "libSDL3.dylib" "${runtime_dir}"
         ppuc_copy_dylib_link_chain "${deps_root}/SDL3/SDL_image/build" "libSDL3_image.dylib" "${runtime_dir}"
      fi
      ppuc_copy_dylib_link_chain "${deps_root}/SDL3/SDL_ttf/build" "libSDL3_ttf.dylib" "${runtime_dir}"
   else
      if [ "${reuse_ppuc_sdl_stack}" = "1" ]; then
         ppuc_vpinball_media_required_glob_copy "${ppuc_runtime_dir}/libSDL3.so*" "${runtime_dir}"
         ppuc_vpinball_media_required_glob_copy "${ppuc_runtime_dir}/libSDL3_image.so*" "${runtime_dir}"
      else
         ppuc_vpinball_media_required_glob_copy "${deps_root}/SDL3/SDL/build/libSDL3.so*" "${runtime_dir}"
         ppuc_vpinball_media_required_glob_copy "${deps_root}/SDL3/SDL_image/build/libSDL3_image.so*" "${runtime_dir}"
      fi
      ppuc_vpinball_media_required_glob_copy "${deps_root}/SDL3/SDL_ttf/build/libSDL3_ttf.so*" "${runtime_dir}"
   fi
   if [ "${reuse_ppuc_sdl_stack}" = "1" ]; then
      ppuc_vpinball_media_required_dir_copy "${ppuc_include_dir}/SDL3" "${include_dir}/"
      ppuc_vpinball_media_required_dir_copy "${ppuc_include_dir}/SDL3_image" "${include_dir}/"
   else
      ppuc_vpinball_media_required_dir_copy "${deps_root}/SDL3/SDL/include/SDL3" "${include_dir}/"
      ppuc_vpinball_media_required_dir_copy "${deps_root}/SDL3/SDL_image/include/SDL3_image" "${include_dir}/"
   fi
   ppuc_vpinball_media_required_dir_copy "${deps_root}/SDL3/SDL_ttf/include/SDL3_ttf" "${include_dir}/"

   expected="${VPINBALL_LIBALTSOUND_SHA}"
   if [ "${platform}" = "macos" ]; then
      expected="${expected}-macos${MACOSX_DEPLOYMENT_TARGET}"
   fi
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
      ppuc_copy_dylib_link_chain "${deps_root}/libaltsound/libaltsound/build" "libaltsound.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${deps_root}/libaltsound/libaltsound/build/libaltsound.so*" "${runtime_dir}"
   fi
   cp "${deps_root}/libaltsound/libaltsound/src/altsound.h" "${include_dir}/"

   expected="${VPINBALL_FFMPEG_SHA}"
   if [ "${platform}" = "macos" ]; then
      expected="${expected}-macos${MACOSX_DEPLOYMENT_TARGET}"
   fi
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
         ppuc_copy_dylib_link_chain "${deps_root}/ffmpeg/ffmpeg/${lib}" "${lib}.dylib" "${runtime_dir}"
      else
         ppuc_vpinball_media_glob_copy "${deps_root}/ffmpeg/ffmpeg/${lib}/${lib}.so*" "${runtime_dir}"
      fi
      mkdir -p "${include_dir}/${lib}"
      cp "${deps_root}/ffmpeg/ffmpeg/${lib}"/*.h "${include_dir}/${lib}/"
   done

   if [ "${platform}" = "macos" ]; then
      ppuc_copy_dylib_link_chain "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}" "libpupdmd.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libpupdmd.so*" "${runtime_dir}"
   fi
   cp "${PPUC_SOURCE_ROOT}/third-party/include/pupdmd.h" "${include_dir}/"
   mkdir -p "${ppuc_include_dir}/pup"
   cp "${vpinball_root}/plugins/pup/PUPPlugin.h" "${ppuc_include_dir}/pup/"
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
   local plugin_rpath_args=()

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
   if [ "${platform}" = "linux" ]; then
      plugin_rpath_args=(
         -DCMAKE_BUILD_WITH_INSTALL_RPATH=TRUE
         "-DCMAKE_INSTALL_RPATH=\$ORIGIN;\$ORIGIN/../.."
      )
   fi

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
      "${plugin_rpath_args[@]}" \
      -DVPINBALL_PLUGIN_PACKAGE_DIR="${plugin_package_dir}"
   for plugin_target in PUPPlugin AltSoundPlugin B2SPlugin B2SLegacyPlugin; do
      cmake --build "${vpinball_build_dir}" --target "${plugin_target}"
   done

   if [ "${platform}" = "macos" ]; then
      if [ -f "${plugin_package_dir}/pup/plugin-pup.dylib" ]; then
         install_name_tool -add_rpath "@loader_path/../.." "${plugin_package_dir}/pup/plugin-pup.dylib" 2>/dev/null || true
      fi
      if [ -f "${plugin_package_dir}/b2s/plugin-b2s.dylib" ]; then
         install_name_tool -add_rpath "@loader_path/../.." "${plugin_package_dir}/b2s/plugin-b2s.dylib" 2>/dev/null || true
      fi
      if [ -f "${plugin_package_dir}/b2slegacy/plugin-b2slegacy.dylib" ]; then
         install_name_tool -add_rpath "@loader_path/../.." "${plugin_package_dir}/b2slegacy/plugin-b2slegacy.dylib" 2>/dev/null || true
      fi
      rm -f "${plugin_package_dir}"/pup/libSDL3.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3.[0-9]*.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3_image*.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3_mixer*.dylib
      rm -f "${plugin_package_dir}"/pup/libpupdmd*.dylib
      rm -f "${plugin_package_dir}"/b2slegacy/libSDL3.dylib
      rm -f "${plugin_package_dir}"/b2slegacy/libSDL3.[0-9]*.dylib

      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libSDL3_ttf.0.dylib" "libSDL3_ttf.0.*.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libSDL3_ttf.dylib" "libSDL3_ttf.0.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libavcodec.dylib" "libavcodec.[0-9]*.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libavformat.dylib" "libavformat.[0-9]*.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libavutil.dylib" "libavutil.[0-9]*.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libswresample.dylib" "libswresample.[0-9]*.dylib"
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/pup" "libswscale.dylib" "libswscale.[0-9]*.dylib"
   elif [ "${platform}" = "linux" ]; then
      rm -f "${plugin_package_dir}"/pup/libSDL3.so*
      rm -f "${plugin_package_dir}"/pup/libSDL3_image.so*
      rm -f "${plugin_package_dir}"/pup/libSDL3_mixer.so*
      rm -f "${plugin_package_dir}"/pup/libpupdmd.so*
      rm -f "${plugin_package_dir}"/b2slegacy/libSDL3.so*
   elif [ "${platform}" = "win" ] || [ "${platform}" = "win-mingw" ] || [ "${platform}" = "windows-mingw" ]; then
      rm -f "${plugin_package_dir}"/b2slegacy/SDL3*.dll
   fi
}

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
