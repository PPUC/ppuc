#pragma once

// The commit ppuc is built from. CMake passes it (PPUC_GIT_SHA); a build that
// does not know it says so instead of printing a placeholder that looks like
// an answer.
#ifndef PPUC_EXECUTABLE_SHA
#define PPUC_EXECUTABLE_SHA "unknown"
#endif

#define PPUC_EXECUTABLE_VERSION_MAJOR 0  // X Digits
#define PPUC_EXECUTABLE_VERSION_MINOR 3  // Max 2 Digits
#define PPUC_EXECUTABLE_VERSION_PATCH 0  // Max 2 Digits

#define _PPUC_EXECUTABLE_STR(x) #x
#define PPUC_EXECUTABLE_STR(x) _PPUC_EXECUTABLE_STR(x)

#define PPUC_EXECUTABLE_VERSION                             \
  PPUC_STR(PPUC_EXECUTABLE_VERSION_MAJOR)                   \
  "." PPUC_STR(PPUC_EXECUTABLE_VERSION_MINOR) "." PPUC_STR( \
      PPUC_EXECUTABLE_VERSION_PATCH)
#define PPUC_EXECUTABLE_MINOR_VERSION     \
  PPUC_STR(PPUC_EXECUTABLE_VERSION_MAJOR) \
  "." PPUC_STR(PPUC_EXECUTABLE_VERSION_MINOR)
