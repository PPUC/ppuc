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
PINMAME_SHA=65c3a8de4a3059e73894466a6376f056065073fa
PINMAME_NVRAM_MAPS_SHA=d8693b9ca59a1b871d2a473be3adb0392471a8e3
LIBPPUC_SHA=b514db24d7867b5ad4bbb565dfb382b0f483d5fb
DOCTEST_VERSION=2.4.11
LIBSDLDMD_SHA=66bbd33c25a5352fc05d0c696deeacf59a15f430
VPINBALL_SHA=2466399d6125981dd1f2d3a9c692179549d74d36
VPINBALL_SDL_SHA=f87239e71e42da91ca317a12eefb82cfbf3393eb
VPINBALL_SDL_IMAGE_SHA="${VPINBALL_SDL_IMAGE_SHA:-${SDL_IMAGE_SHA}}"
VPINBALL_SDL_TTF_SHA=a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
VPINBALL_LIBALTSOUND_SHA=f4b790a19ae45a9f93ae0051df6933800c7a6446
VPINBALL_FFMPEG_SHA=239f2c733de417201d7ad3b3b8b0d9b63285b2b1

PPUC_SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SOURCE_DIR_CACHE_BUSTER="${SOURCE_DIR_CACHE_BUSTER:-$(date +%s)}"
# Local source overrides: one variable per dependency, named explicitly.
#
# This follows the VPX ecosystem's convention -- LIBDMDUTIL_SOURCE_DIR and
# friends -- so a checkout is built only when it is named. PPUC previously had a
# PPUC_DEPENDENCY_SOURCE=local mode that derived every path from a single
# workspace root, which is convenient right up until the repositories are not
# all siblings. In practice they are not: libdmdutil, libserum and libzedmd live
# beside vpinball rather than beside ppuc. The mode then resolved some
# dependencies to local checkouts and left the rest on their pins, so a build
# could quietly use a different libserum than platforms/config.sh named, with
# nothing in the output to say so.
#
# Exported rather than merely read, because nested builds resolve the same
# variables -- libsdldmd stages its own libdmdutil, for one.
PPUC_SOURCE_DIR_VARS=(
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

for ppuc_source_var in "${PPUC_SOURCE_DIR_VARS[@]}"; do
   if [ -n "${!ppuc_source_var:-}" ]; then
      export "${ppuc_source_var}"
   fi
done
unset ppuc_source_var

dependency_source_dir() {
   local var_name="$1"
   local source_dir="${!var_name:-}"

   if [ -z "${source_dir}" ]; then
      return 0
   fi

   (cd "${PPUC_SOURCE_ROOT}" && cd "${source_dir}" && pwd -P)
}

ppuc_source_dir_fingerprint() {
   # A stable fingerprint of a local dependency checkout: its commit plus any
   # uncommitted work. Used so that building from a source directory rebuilds
   # when the sources actually changed, rather than on every invocation.
   #
   # Falls back to the timestamp for anything that is not a git checkout, which
   # restores the old always-rebuild behaviour for that case.
   local dir="$1"
   local head
   local dirty

   if ! head="$(git -C "${dir}" rev-parse HEAD 2>/dev/null)"; then
      echo "${SOURCE_DIR_CACHE_BUSTER}"
      return 0
   fi

   # Hash with git rather than shasum/sha1sum: git is definitionally available
   # here (the branch above already used it), whereas shasum is a Perl script
   # that is not guaranteed on every build host. A missing hasher would have
   # silently degraded the fingerprint to commit-only rather than failing.
   dirty="$( {
      # Content of tracked modifications.
      git -C "${dir}" diff HEAD
      # Content of untracked files, so a new or edited untracked source file
      # (a test suite, for instance) still triggers a rebuild.
      git -C "${dir}" ls-files --others --exclude-standard | while read -r f; do
         git hash-object "${dir}/${f}" 2>/dev/null
      done
   } 2>/dev/null | git hash-object --stdin | cut -c1-12 )"

   echo "${head:0:12}-${dirty}"
}

dependency_cache_key() {
   local sha="$1"
   local source_var="$2"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "source:${source_dir}:$(ppuc_source_dir_fingerprint "${source_dir}")"
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
   # -a preserves timestamps. A plain cp would restamp all of Lua on every run
   # and force the ~33 Lua translation units to recompile each build.
   cp -a lua/lua/src/*.h ../third-party/include/lua/
   cp -a lua/lua/src/*.h ../third-party/lua-src/
   cp -a lua/lua/src/*.c ../third-party/lua-src/
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

# libpinmame vendors its own copies of the plugin API headers, and PPUC builds it
# from source against a vpinball checkout that may be ahead of PINMAME_SHA. A
# difference in DisplaySrcId's layout between the two does not fail loudly: the
# function pointers still line up and the geometry does not, so a DMD arrives
# with nonsense dimensions instead of not arriving. Staging vpinball's copies
# over pinmame's makes one header authoritative and takes the two SHAs out of
# lockstep. If the headers ever become genuinely incompatible the libpinmame
# build fails, which is the failure worth having.
ppuc_stage_plugin_api_headers_into_pinmame() {
   local pinmame_plugins_dir="${PPUC_SOURCE_ROOT}/external/pinmame/pinmame/src/libpinmame/plugins"
   local vpinball_root
   local header

   if [ "${PPUC_BUILD_VPINBALL_MEDIA_PLUGINS:-1}" = "0" ]; then
      return 0
   fi
   if [ ! -d "${pinmame_plugins_dir}" ]; then
      return 0
   fi

   ppuc_stage_vpinball_source
   vpinball_root="$(ppuc_vpinball_root)"

   for header in ControllerPlugin.h MsgPlugin.h; do
      if [ -f "${vpinball_root}/plugins/plugins/${header}" ]; then
         cp -a "${vpinball_root}/plugins/plugins/${header}" "${pinmame_plugins_dir}/${header}"
      fi
   done
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
   # -a preserves timestamps: a plain cp would make every staged header look
   # newer than the objects that include it and force a full plugin rebuild.
   cp -a "${source_dir}" "${dest}"
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
   cp -a "${deps_root}/libaltsound/libaltsound/src/altsound.h" "${include_dir}/"

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
      cp -a "${deps_root}/ffmpeg/ffmpeg/${lib}"/*.h "${include_dir}/${lib}/"
   done

   if [ "${platform}" = "macos" ]; then
      ppuc_copy_dylib_link_chain "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}" "libpupdmd.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libpupdmd.so*" "${runtime_dir}"
   fi
   cp -a "${PPUC_SOURCE_ROOT}/third-party/include/pupdmd.h" "${include_dir}/"
   # Six plugins include pinmame/PinMAMEPlugin.h -- b2s, dof, altsound, pinmame,
   # b2slegacy and the shared B2SPluginEventStream -- for PMPI_GAMEID_PREFIX and
   # the PinMAME event messages. vpinball's own build gets it by staging pinmame;
   # PPUC builds the plugins against its own pinmame checkout instead, so it has
   # to put the header where they expect it.
   # PinMAMEPlugin links libpinmame out of vpinball's own runtime-libs. PPUC
   # already built it in external.sh, so reuse that rather than build it twice.
   if [ "${platform}" = "macos" ]; then
      ppuc_copy_dylib_link_chain "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}" \
         "libpinmame.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy \
         "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libpinmame.so*" "${runtime_dir}"
   fi
   mkdir -p "${include_dir}/pinmame"
   # Under pinmame/, not at the include root: the plugin sources include
   # "pinmame/libpinmame.h", matching how vpinball stages its own pinmame.
   cp -a "${PPUC_SOURCE_ROOT}/third-party/include/libpinmame.h" "${include_dir}/pinmame/"
   cp -a "${PPUC_SOURCE_ROOT}/external/pinmame/pinmame/src/libpinmame/PinMAMEPlugin.h" \
      "${include_dir}/pinmame/"
   # PUPPlugin.h is not staged: PUPPI_MSG_QUEUE_EVENT was a PPUC-only addition
   # to the fork and no longer exists. PUP discovers controller state from the
   # bus now, so nothing here includes that header.
   # PinMAMEPlugin.h comes from the pinmame tree and carries PMPI_GAMEID_PREFIX,
   # PMPI_EVT_ON_AUDIO_CMD and PinMAMEChildBoardEventMsg, which the media host
   # needs to publish its controller and its sound commands.
   mkdir -p "${ppuc_include_dir}/pinmame"
   cp -a "${PPUC_SOURCE_ROOT}/external/pinmame/pinmame/src/libpinmame/PinMAMEPlugin.h" \
      "${ppuc_include_dir}/pinmame/"

   # SerumPlugin links libserum. PPUC already stages it for libdmdutil's own
   # colorizer, and both come from PPUC/libserum, so reuse that build rather
   # than fetching a second copy that could drift to a different version.
   if [ "${platform}" = "macos" ]; then
      ppuc_copy_dylib_link_chain "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}" \
         "libserum.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy \
         "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libserum.so*" "${runtime_dir}"
   fi
   cp -a "${PPUC_SOURCE_ROOT}/third-party/include/serum-decode.h" "${include_dir}/"
   cp -a "${PPUC_SOURCE_ROOT}/third-party/include/serum.h" "${include_dir}/"

   # VNIPlugin links libvni, for the .vni/.pal colorizations Serum does not
   # cover. Same reasoning as libserum: PPUC already stages it for libdmdutil,
   # and one copy cannot drift from another.
   if [ "${platform}" = "macos" ]; then
      ppuc_copy_dylib_link_chain "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}" \
         "libvni.dylib" "${runtime_dir}"
   else
      ppuc_vpinball_media_glob_copy \
         "${PPUC_SOURCE_ROOT}/third-party/runtime-libs/${platform_tag}/libvni.so*" "${runtime_dir}"
   fi
   cp -a "${PPUC_SOURCE_ROOT}/third-party/include/vni.h" "${include_dir}/"
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
   ppuc_reset_stale_cmake_cache "${vpinball_build_dir}" "${vpinball_root}"
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
   # Upstream derives the plugin folder from the app bundle, which creates it as
   # a side effect. Building the plugins without the app means nothing does, and
   # the plugin.cfg copy is the first thing to notice.
   mkdir -p "${plugin_package_dir}/pup" "${plugin_package_dir}/altsound" "${plugin_package_dir}/b2s" \
      "${plugin_package_dir}/pinmame" "${plugin_package_dir}/alphadmd" "${plugin_package_dir}/serum" "${plugin_package_dir}/vni"

   # B2SLegacyPlugin is deliberately not built: MediaPluginHost only ever
   # loads "PUP", "AltSound" and "B2S". --b2s uses the modern B2S plugin.
   # PinMAMEPlugin first: it is what actually runs the ROM, so a link failure
   # there should surface before the long PUP/ffmpeg build.
   # AlphaDMDPlugin renders segment displays as a 128x32 DMD. libpinmame used to
   # synthesize that frame itself; its plugin path does not, so without this an
   # alphanumeric game has no DMD at all -- no ZeDMD, no virtual DMD, and no
   # Serum, which for Time Warp is the whole point of its colorization.
   for plugin_target in PinMAMEPlugin PUPPlugin AltSoundPlugin B2SPlugin AlphaDMDPlugin SerumPlugin VNIPlugin; do
      cmake --build "${vpinball_build_dir}" --target "${plugin_target}"
   done

   # Libraries that have no shared home. Everything a plugin needs is either in
   # ppuc/ next to the executables, reached through the @loader_path/../.. rpath
   # added below, or nowhere -- in which case it has to sit beside the plugin.
   # Which libraries fall on which side is a fact about PPUC's layout, not about
   # the plugins, so it is decided here rather than upstream.
   if [ "${platform}" = "macos" ]; then
      ppuc_vpinball_media_glob_copy \
         "${vpinball_root}/third-party/runtime-libs/${platform}-${arch}/libaltsound*.dylib" \
         "${plugin_package_dir}/altsound"
      for lib in libSDL3_ttf libavcodec libavformat libavutil libswresample libswscale; do
         ppuc_vpinball_media_glob_copy \
            "${vpinball_root}/third-party/runtime-libs/${platform}-${arch}/${lib}*.dylib" \
            "${plugin_package_dir}/pup"
      done
   fi

   if [ "${platform}" = "macos" ]; then
      if [ -f "${plugin_package_dir}/pup/plugin-pup.dylib" ]; then
         install_name_tool -add_rpath "@loader_path/../.." "${plugin_package_dir}/pup/plugin-pup.dylib" 2>/dev/null || true
      fi
      # Reaches the shared dylibs in ppuc/, two levels up from plugins/<name>/.
      for plugin_using_shared_libs in b2s pinmame serum vni; do
         if [ -f "${plugin_package_dir}/${plugin_using_shared_libs}/plugin-${plugin_using_shared_libs}.dylib" ]; then
            install_name_tool -add_rpath "@loader_path/../.." \
               "${plugin_package_dir}/${plugin_using_shared_libs}/plugin-${plugin_using_shared_libs}.dylib" 2>/dev/null || true
         fi
      done
      # Added rather than set through CMAKE_INSTALL_RPATH: setting it would
      # replace the rpaths upstream gives every plugin, which the app-bundle
      # layout depends on.
      for plugin_with_local_libs in altsound pup; do
         if [ -f "${plugin_package_dir}/${plugin_with_local_libs}/plugin-${plugin_with_local_libs}.dylib" ]; then
            install_name_tool -add_rpath "@loader_path" \
               "${plugin_package_dir}/${plugin_with_local_libs}/plugin-${plugin_with_local_libs}.dylib" 2>/dev/null || true
         fi
      done
      rm -f "${plugin_package_dir}"/pup/libSDL3.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3.[0-9]*.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3_image*.dylib
      rm -f "${plugin_package_dir}"/pup/libSDL3_mixer*.dylib
      rm -f "${plugin_package_dir}"/pup/libpupdmd*.dylib
      # libpinmame is in ppuc/ beside the executables, reached through the
      # @loader_path/../.. rpath added below.
      rm -f "${plugin_package_dir}"/pinmame/libpinmame*.dylib

      # The glob copy resolves symlinks, so the unversioned aliases a consumer
      # links against have to be recreated.
      ppuc_relink_macos_dylib_alias "${plugin_package_dir}/altsound" "libaltsound.dylib" "libaltsound.[0-9]*.dylib"
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
   fi
   # Windows needed no post-build fixups once B2SLegacy was dropped: its only
   # step was removing SDL3 DLLs from the b2slegacy package directory.
}

ppuc_reset_stale_cmake_cache() {
   # A CMake build directory records the absolute path of the source tree it
   # was configured for. Moving, renaming or copying the workspace makes every
   # such directory unusable, and CMake then refuses to configure with an error
   # that only says "re-run cmake with a different source directory".
   #
   # Removing the build directory is the only remedy, and it is always safe:
   # these directories are generated and gitignored. Do it automatically rather
   # than making every developer diagnose it.
   #
   # Only build directories that are actually reconfigured need this. Build
   # directories inside cached dependency trees are never re-run (their
   # cache.txt short-circuits the whole block), and deleting those would remove
   # artifacts that external.sh later copies.
   local build_dir="$1"
   local source_dir="$2"
   local cached
   local cached_real
   local source_real

   [ -f "${build_dir}/CMakeCache.txt" ] || return 0

   cached="$(grep -m1 '^CMAKE_HOME_DIRECTORY:' "${build_dir}/CMakeCache.txt" |
      cut -d= -f2)"
   [ -n "${cached}" ] || return 0

   # Resolve both sides: a symlinked workspace root (for example
   # /Users/<user>/workspace -> /Volumes/data/workspace) spells the same
   # directory two ways and must not count as stale.
   cached_real="$(cd "${cached}" 2>/dev/null && pwd -P)" || cached_real=""
   source_real="$(cd "${source_dir}" 2>/dev/null && pwd -P)" || source_real=""

   if [ -n "${source_real}" ] && [ "${cached_real}" != "${source_real}" ]; then
      echo "Removing stale CMake cache: ${build_dir}"
      echo "  configured for: ${cached}"
      echo "  building:       ${source_real}"
      rm -rf "${build_dir}"
   fi
}

ppuc_stage_doctest() {
   # doctest is the unit test framework. It is staged like every other
   # dependency rather than committed, because third-party/include is
   # generated and gitignored.
   #
   # libppuc stages the same header, so a full build reuses that copy instead
   # of downloading it twice. But ppuc keeps its own pin and its own fallback:
   # doctest is a build tool, not an interface dependency, so the ability to
   # run ppuc's tests must not depend on which libppuc revision is pinned.
   local include_dir="${PPUC_SOURCE_ROOT}/third-party/include"
   local header="${include_dir}/doctest.h"
   local marker="${include_dir}/doctest.cache.txt"
   local from_libppuc="${PPUC_SOURCE_ROOT}/external/libppuc/libppuc/third-party/include/doctest.h"
   local expected="${DOCTEST_VERSION}"
   local found

   found="$([ -f "${marker}" ] && cat "${marker}" || echo "")"

   if [ "${expected}" = "${found}" ] && [ -f "${header}" ]; then
      return 0
   fi

   mkdir -p "${include_dir}"

   if [ -f "${from_libppuc}" ]; then
      echo "Staging doctest from libppuc"
      cp "${from_libppuc}" "${header}"
   else
      echo "Staging doctest ${DOCTEST_VERSION}"
      curl -sL \
         "https://raw.githubusercontent.com/doctest/doctest/v${DOCTEST_VERSION}/doctest/doctest.h" \
         -o "${header}"
   fi

   echo "${expected}" > "${marker}"
}

ppuc_parse_build_args() {
   # Shared option parsing for every platforms/*/*/build.sh.
   while [ $# -gt 0 ]; do
      case "$1" in
         --test|--tests)
            PPUC_RUN_TESTS=1
            ;;
         --help|-h)
            echo "Usage: build.sh [--test]"
            echo ""
            echo "  --test   after building, build and run the host-side C++"
            echo "           unit test suites (libppuc and ppuc)"
            exit 0
            ;;
         *)
            echo "Unknown build option: $1" >&2
            echo "Supported options: --test" >&2
            exit 1
            ;;
      esac
      shift
   done

   export PPUC_RUN_TESTS="${PPUC_RUN_TESTS:-0}"
}

PPUC_TEST_RESULTS=()
PPUC_TEST_FAILED_LOGS=()

ppuc_doctest_summary() {
   # Extracts a compact "N test cases, M assertions" line from a ctest log.
   local log="$1"
   local cases
   local assertions

   cases="$(grep -m1 'test cases:' "${log}" 2>/dev/null |
      sed -E 's/.*test cases: *([0-9]+).*/\1/')"
   assertions="$(grep -m1 'assertions:' "${log}" 2>/dev/null |
      sed -E 's/.*assertions: *([0-9]+).*/\1/')"

   if [ -n "${cases}" ] && [ -n "${assertions}" ]; then
      echo "${cases} test cases, ${assertions} assertions"
   else
      echo ""
   fi
}

ppuc_record_test_suite() {
   PPUC_TEST_RESULTS+=("$(printf '  %-10s %-8s %s' "$1" "$2" "$3")")
}

ppuc_run_test_suite() {
   # Configures, builds and runs one suite. All detail goes to a log file so
   # that the console stays readable; the log is replayed only on failure.
   local label="$1"
   local build_dir="$2"
   local log="$3"
   shift 3

   printf '  %-10s configuring and building ... ' "${label}"
   if ! "$@" > "${log}" 2>&1; then
      echo "BUILD FAILED"
      ppuc_record_test_suite "${label}" "FAILED" "build failed, see ${log}"
      PPUC_TEST_FAILED_LOGS+=("${log}")
      return 1
   fi

   printf 'running ... '
   if ( cd "${build_dir}" && ctest --verbose ) >> "${log}" 2>&1; then
      echo "ok"
      ppuc_record_test_suite "${label}" "PASSED" "$(ppuc_doctest_summary "${log}")"
      return 0
   fi

   echo "FAILED"
   ppuc_record_test_suite "${label}" "FAILED" "see ${log}"
   PPUC_TEST_FAILED_LOGS+=("${log}")
   return 1
}

ppuc_run_host_tests() {
   # Builds and runs the unit tests across the host-side C++ stack. libppuc is
   # tested from the copy staged under external/, which is the same source the
   # ppuc build links against, so the tests cover exactly what was built.
   #
   # Per-suite detail is written to build-tests/logs/ and only echoed when a
   # suite fails, so the results are legible at the end of a long build.
   local platform="$1"
   local arch="$2"
   local libppuc_dir
   local num_procs
   local log_dir
   local failures=0
   local line
   local log

   if [ "${PPUC_RUN_TESTS:-0}" != "1" ]; then
      return 0
   fi

   num_procs="$(ppuc_vpinball_media_num_procs)"
   libppuc_dir="${PPUC_SOURCE_ROOT}/external/libppuc/libppuc"
   log_dir="${PPUC_SOURCE_ROOT}/build-tests/logs"
   mkdir -p "${log_dir}"

   PPUC_TEST_RESULTS=()
   PPUC_TEST_FAILED_LOGS=()

   echo ""
   echo "================================================================"
   echo " Host-side C++ test suites"
   echo "================================================================"

   if [ ! -f "${libppuc_dir}/CMakeLists.txt" ]; then
      ppuc_record_test_suite "libppuc" "SKIPPED" "sources not staged"
   elif [ ! -d "${libppuc_dir}/tests" ]; then
      # The staged libppuc is whatever LIBPPUC_SHA points at. A revision that
      # predates the test suite has no tests/ directory and no BUILD_TESTS
      # option, so skip rather than fail the build.
      ppuc_record_test_suite "libppuc" "SKIPPED" \
         "pinned revision has no test suite; bump LIBPPUC_SHA or set LIBPPUC_SOURCE_DIR"
   else
      ppuc_reset_stale_cmake_cache "${libppuc_dir}/build-tests" "${libppuc_dir}"
      # The test target compiles the library sources directly, so the shared
      # and static libraries are not needed here.
      ppuc_run_test_suite "libppuc" "${libppuc_dir}/build-tests" \
         "${log_dir}/libppuc.log" \
         bash -c "cmake -S '${libppuc_dir}' -B '${libppuc_dir}/build-tests' \
               -DPLATFORM='${platform}' -DARCH='${arch}' \
               -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF -DBUILD_TESTS=ON \
               -DCMAKE_BUILD_TYPE='${BUILD_TYPE}' &&
            cmake --build '${libppuc_dir}/build-tests' -- -j${num_procs}" ||
         failures=1
   fi

   ppuc_reset_stale_cmake_cache "${PPUC_SOURCE_ROOT}/build-tests" "${PPUC_SOURCE_ROOT}"
   ppuc_run_test_suite "ppuc" "${PPUC_SOURCE_ROOT}/build-tests" \
      "${log_dir}/ppuc.log" \
      bash -c "cmake -S '${PPUC_SOURCE_ROOT}' -B '${PPUC_SOURCE_ROOT}/build-tests' \
            -DPLATFORM='${platform}' -DARCH='${arch}' \
            -DPPUC_BUILD_TESTS=ON -DPPUC_BUILD_MENU=OFF -DPPUC_BUILD_BACKBOX=OFF \
            -DCMAKE_BUILD_TYPE='${BUILD_TYPE}' &&
         cmake --build '${PPUC_SOURCE_ROOT}/build-tests' --target ppuc_tests -- -j${num_procs}" ||
      failures=1

   # Replay the detail for anything that failed, so the reason is visible
   # without hunting for a log file.
   for log in "${PPUC_TEST_FAILED_LOGS[@]}"; do
      echo ""
      echo "---------------- ${log} ----------------"
      cat "${log}"
      echo "---------------- end of ${log} ----------------"
   done

   echo ""
   echo "================================================================"
   echo " Test results"
   echo "================================================================"
   for line in "${PPUC_TEST_RESULTS[@]}"; do
      echo "${line}"
   done
   echo "================================================================"

   if [ "${failures}" != "0" ]; then
      echo " FAILED"
      echo "================================================================"
      return 1
   fi
   echo " All host-side test suites passed"
   echo "================================================================"
}

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
