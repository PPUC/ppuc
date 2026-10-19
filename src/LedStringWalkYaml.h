#pragma once

#include <string>
#include <vector>

#include "LedStringWalk.h"

// Reads the `ledStripes:` block of a game's io-boards.yaml for the LED string
// test.
//
// libppuc parses the same block, but keeps neither the string length nor an
// LED's position on the string -- the board needs them, the host never did. Read
// here rather than added to libppuc because only a test wants them. libppuc has
// already validated the file by the time this runs, so a failure here is not a
// reason to stop a game; the caller only loses the test.
bool LoadLedStringsFromYaml(const std::string& path, std::vector<LedStringWalk::String>* pStrings, std::string* pError);
