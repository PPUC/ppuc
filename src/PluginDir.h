#pragma once

#include <string>

// The directory VPX plugins are loaded from: `configured` when it names one,
// otherwise the first that exists of the places build.sh and a package put
// them. The plugin bus and MediaPluginHost must agree on it, so both resolve
// it here.
std::string ResolvePluginDir(const char* configured);
