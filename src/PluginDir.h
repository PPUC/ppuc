#pragma once

#include <string>

// The directory VPX plugins are loaded from: `configured` when it names one,
// otherwise the first that exists of the places build.sh and a package put
// them. The plugin bus and MediaPluginHost must agree on it, so both resolve
// it here.
std::string ResolvePluginDir(const char* configured);

// The directory board firmware images are read from: `configured` when it names
// one, otherwise the `firmware` folder a build puts next to the executable.
// Empty when there is neither. Unlike plugins, never anywhere relative to where
// ppuc was started: images are what boards get flashed with, so a stray folder
// must not be picked up because of the current directory.
std::string ResolveFirmwareDir(const char* configured);
