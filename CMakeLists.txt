cmake_minimum_required(VERSION 3.25)

set(PLATFORM "win" CACHE STRING "Platform")
set(ARCH "x64" CACHE STRING "Arch")

message(STATUS "PLATFORM: ${PLATFORM}")
message(STATUS "ARCH: ${ARCH}")

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

add_executable(ppuc-pinmame
   src/ppuc.cpp
   src/VirtualDMD.cpp
   src/xbrz/xbrz.cpp
)

target_include_directories(ppuc-pinmame PUBLIC
   third-party/include
)

if(PLATFORM STREQUAL "win")
   target_link_directories(ppuc-pinmame PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )
   if(ARCH STREQUAL "x64")
      target_link_libraries(ppuc-pinmame LINK_PUBLIC
         pinmame64
         ppuc64
         dmdutil64
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
         cargs
         SDL3
         SDL3_image
         yaml-cpp
         ws2_32
      )
   endif()
else()
   target_link_directories(ppuc-pinmame PUBLIC
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )

   target_link_libraries(ppuc-pinmame LINK_PUBLIC
      pinmame
      ppuc
      dmdutil
      cargs
      SDL3
      SDL3_image
   )
endif()

add_executable(ppuc-backbox
   src/backbox.cpp
   src/VirtualDMD.cpp
   src/xbrz/xbrz.cpp
)

target_include_directories(ppuc-backbox PUBLIC
   third-party/include
)

if(PLATFORM STREQUAL "win")
   target_link_directories(ppuc-backbox PUBLIC
      third-party/build-libs/${PLATFORM}-${ARCH}
      third-party/runtime-libs/${PLATFORM}-${ARCH}
   )
   if(ARCH STREQUAL "x64")
      target_link_libraries(ppuc-backbox LINK_PUBLIC
         dmdutil64
         cargs64
         SDL364
         SDL3_image64
         sockpp64
         ws2_32
      )
   else()
      target_link_libraries(ppuc-backbox LINK_PUBLIC
         dmdutil
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
      cargs
      SDL3
      SDL3_image
      sockpp
   )
endif()
