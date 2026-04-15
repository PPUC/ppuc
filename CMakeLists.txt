cmake_minimum_required(VERSION 3.25)

set(PLATFORM "win" CACHE STRING "Platform")
set(ARCH "x64" CACHE STRING "Arch")
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan for Debug builds" OFF)

message(STATUS "PLATFORM: ${PLATFORM}")
message(STATUS "ARCH: ${ARCH}")
message(STATUS "ENABLE_SANITIZERS: ${ENABLE_SANITIZERS}")

file(READ src/ppuc_version.h version)
string(REGEX MATCH "PPUC_EXECUTABLE_VERSION_MAJOR[ ]+([0-9]+)" _tmp ${version})
set(VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "PPUC_EXECUTABLE_VERSION_MINOR[ ]+([0-9]+)" _tmp ${version})
set(VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "PPUC_EXECUTABLE_VERSION_PATCH[ ]+([0-9]+)" _tmp ${version})
set(VERSION_PATCH "${CMAKE_MATCH_1}")

project(ppuc_executable VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}"
   DESCRIPTION "Pinball Power-Up Controller (PPUC)")

if(PLATFORM STREQUAL "win")
   if(ARCH STREQUAL "x86")
      add_compile_definitions(WIN32)
   endif()
elseif(PLATFORM STREQUAL "win-mingw")
   add_compile_options(-Wa,-mbig-obj)
elseif(PLATFORM STREQUAL "macos")
   if (ARCH STREQUAL "arm64")
      set(CMAKE_OSX_ARCHITECTURES arm64)
   elseif(ARCH STREQUAL "x64")
      set(CMAKE_OSX_ARCHITECTURES x86_64)
   endif()
   set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
   set(CMAKE_INSTALL_RPATH "@executable_path")
elseif(PLATFORM STREQUAL "linux")
   set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
   set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 99)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/SDLDMD/SDLDMD.h")
   message(FATAL_ERROR "Could not find staged SDLDMD headers in third-party/include/SDLDMD. Update ppuc external dependencies first.")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
   if(ENABLE_SANITIZERS AND (PLATFORM STREQUAL "macos" OR PLATFORM STREQUAL "linux"))
      set(SANITIZER_FLAGS -fsanitize=address,undefined)
      add_compile_options(${SANITIZER_FLAGS} -fno-omit-frame-pointer -g)
      add_link_options(${SANITIZER_FLAGS})
   endif()
endif()

add_executable(ppuc-pinmame
   src/AudioOutput.cpp
   src/PUPTriggerEngine.cpp
   src/SpeechCliSupport.cpp
   src/SpeechTriggerMap.cpp
   src/SpeechService.cpp
   src/ppuc.cpp
)

target_include_directories(ppuc-pinmame PUBLIC
   third-party/include
)

target_compile_definitions(ppuc-pinmame PRIVATE
   PPUC_RUNTIME_LIB_SUBDIR="${PLATFORM}-${ARCH}"
)

set(PPUC_HAS_FLITE_HEADERS FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/flite/flite.h" OR
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/flite/flite/flite.h")
   set(PPUC_HAS_FLITE_HEADERS TRUE)
endif()

set(PPUC_HAS_FLITE_LIBS FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/build-libs/${PLATFORM}-${ARCH}/libflite.a" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/build-libs/${PLATFORM}-${ARCH}/libflite_cmu_us_kal.a" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/build-libs/${PLATFORM}-${ARCH}/libflite_cmulex.a" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/build-libs/${PLATFORM}-${ARCH}/libflite_usenglish.a")
   set(PPUC_HAS_FLITE_LIBS TRUE)
endif()

set(PPUC_ENABLE_FLITE FALSE)
if(PPUC_HAS_FLITE_HEADERS AND PPUC_HAS_FLITE_LIBS)
   set(PPUC_ENABLE_FLITE TRUE)
   message(STATUS "Flite speech backend: enabled")
elseif(PPUC_HAS_FLITE_HEADERS OR PPUC_HAS_FLITE_LIBS)
   message(WARNING "Flite speech backend: disabled due to incomplete staged artifacts")
else()
   message(STATUS "Flite speech backend: disabled")
endif()

if(PPUC_ENABLE_FLITE)
   target_compile_definitions(ppuc-pinmame PRIVATE PPUC_HAS_FLITE)
   target_sources(ppuc-pinmame PRIVATE
      src/FliteSpeechService.cpp
   )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
   target_compile_definitions(ppuc-pinmame PRIVATE PPUC_HAS_ESPEAK_NG)
   target_sources(ppuc-pinmame PRIVATE
      src/ESpeakNgSpeechService.cpp
   )
endif()

if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw")
   target_link_directories(ppuc-pinmame PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )
   if(ARCH STREQUAL "x64")
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         pinmame64
         ppuc64
         dmdutil64
         sdldmd64
         cargs64
         SDL364
         SDL3_image64
         yaml-cpp
         ws2_32
      )
   else()
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         pinmame
         ppuc
         dmdutil
         sdldmd
         cargs
         SDL3
         SDL3_image
         yaml-cpp
         ws2_32
      )
   endif()

   if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
      EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         espeak-ng
      )
   endif()
else()
   target_link_directories(ppuc-pinmame PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )

   target_link_libraries(ppuc-pinmame LINK_PUBLIC
      pinmame
      ppuc
      dmdutil
      sdldmd
      cargs
      SDL3
      SDL3_image
   )

   if(PPUC_ENABLE_FLITE)
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         flite_cmu_us_kal
         flite_cmulex
         flite_usenglish
         flite
      )

      if(PLATFORM STREQUAL "linux")
         target_link_libraries(ppuc-pinmame LINK_PUBLIC
            asound
         )
      endif()
   endif()

   if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
      EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         espeak-ng
      )
   endif()
endif()

add_executable(ppuc-menu
   src/AudioOutput.cpp
   src/menu.cpp
   src/SpeechCliSupport.cpp
   src/SpeechService.cpp
)

target_include_directories(ppuc-menu PUBLIC
   third-party/include
)

target_compile_definitions(ppuc-menu PRIVATE
   PPUC_RUNTIME_LIB_SUBDIR="${PLATFORM}-${ARCH}"
)

if(PPUC_ENABLE_FLITE)
   target_compile_definitions(ppuc-menu PRIVATE PPUC_HAS_FLITE)
   target_sources(ppuc-menu PRIVATE
      src/FliteSpeechService.cpp
   )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
   target_compile_definitions(ppuc-menu PRIVATE PPUC_HAS_ESPEAK_NG)
   target_sources(ppuc-menu PRIVATE
      src/ESpeakNgSpeechService.cpp
   )
endif()

if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw")
   target_link_directories(ppuc-menu PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )
   if(ARCH STREQUAL "x64")
      target_link_libraries(ppuc-menu LINK_PUBLIC
         ppuc64
         cargs64
         SDL364
         SDL3_image64
         yaml-cpp
      )
   else()
      target_link_libraries(ppuc-menu LINK_PUBLIC
         ppuc
         cargs
         SDL3
         SDL3_image
         yaml-cpp
      )
   endif()

   if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
      EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
      target_link_libraries(ppuc-menu LINK_PUBLIC
         espeak-ng
      )
   endif()
else()
   target_link_directories(ppuc-menu PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )

   target_link_libraries(ppuc-menu LINK_PUBLIC
      ppuc
      cargs
      SDL3
      SDL3_image
      yaml-cpp
   )

   if(PPUC_ENABLE_FLITE)
      target_link_libraries(ppuc-menu LINK_PUBLIC
         flite_cmu_us_kal
         flite_cmulex
         flite_usenglish
         flite
      )

      if(PLATFORM STREQUAL "linux")
         target_link_libraries(ppuc-menu LINK_PUBLIC
            asound
         )
      endif()
   endif()

   if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/speak_lib.h" OR
      EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/espeak-ng/espeak-ng/speak_lib.h")
      target_link_libraries(ppuc-menu LINK_PUBLIC
         espeak-ng
      )
   endif()
endif()

add_executable(ppuc-backbox
   src/backbox.cpp
)

target_include_directories(ppuc-backbox PUBLIC
   third-party/include
)

if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw")
   target_link_directories(ppuc-backbox PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )
   if(ARCH STREQUAL "x64")
      target_link_libraries(ppuc-backbox LINK_PUBLIC
         dmdutil64
         sdldmd64
         cargs64
         SDL364
         SDL3_image64
         sockpp64
         ws2_32
      )
   else()
      target_link_libraries(ppuc-backbox LINK_PUBLIC
         dmdutil
         sdldmd
         cargs
         SDL3
         SDL3_image
         sockpp
         ws2_32
      )
   endif()
else()
   target_link_directories(ppuc-backbox PUBLIC
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )

   target_link_libraries(ppuc-backbox LINK_PUBLIC
      dmdutil
      sdldmd
      cargs
      SDL3
      SDL3_image
      sockpp
   )
endif()
