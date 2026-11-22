#pragma once

#include <string>

#include "BenchBalls.h"
#include "GameConfig.h"

namespace bench
{

// Reads the `bench:` block of an io-boards.yaml. What it does not say about the
// trough and shooter lane comes from `game` (the `emGame:` block), which is all
// a ROM-less machine needs. False with *pError when the block is wrong or a
// machine with neither has nothing to serve a ball with.
bool LoadConfig(const std::string& path, const GameConfig* game, Config* pConfig, std::string* pError);

}  // namespace bench
