#pragma once

#include <string>
#include <vector>

#include "LedStringWalk.h"

// Reads the `ledStripes:` block of a game's io-boards.yaml for the LED string
// test.
//
// libppuc parses the same block, but keeps neither the string length nor an
// LED's position on the string -- the board needs them, the host never did. Read
// here rather than added to libppuc because only a test wants them.
bool LoadLedStringsFromYaml(const std::string& path, std::vector<LedStringWalk::String>* pStrings, std::string* pError);

// Lamp numbers the machine uses outside its LED strings: the lamp matrix and
// PWM outputs of type lamp. The test keeps clear of them, so lighting a test
// lamp never lights a bulb as well.
bool CollectNonStringLampNumbers(const std::string& path, std::vector<int>* pNumbers, std::string* pError);

// Writes a copy of the game config in which every planned pixel is a lamp of
// its own and nothing else is assigned to the strings: no flashers, no GI, no
// segments or effects -- a static segment repaints its LEDs and would cover the
// pixel under test. Strings without planned pixels are left as they are.
bool WriteLedStringTestConfig(const std::string& sourcePath, const std::string& targetPath,
                              const std::vector<LedStringWalk::Pixel>& pixels, std::string* pError);
