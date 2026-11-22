#pragma once

#include <string>
#include <vector>

#include "BenchBalls.h"
#include "GameConfig.h"

namespace bench
{

// Reads the `bench:` block of an io-boards.yaml. What it does not say about the
// trough and shooter lane comes from `game` (the `emGame:` block), which is all
// a ROM-less machine needs. False with *pError when the block is wrong or a
// machine with neither has nothing to serve a ball with.
bool LoadConfig(const std::string& path, const GameConfig* game, Config* pConfig, std::string* pError);

// A switch or output as io-boards.yaml declares it. Without boards libppuc
// never reads the device list (it does so while connecting), so the switch and
// coil monitor takes it from here on the bench.
struct Device
{
  int number = 0;
  int board = 0;
  int port = 0;
  std::string description;
  std::string type;  // outputs: coil, flasher, lamp, motor, shaker
};
// False with *pError when the file cannot be read.
bool LoadDevices(const std::string& path, std::vector<Device>* pSwitches, std::vector<Device>* pOutputs,
                 std::string* pError);

}  // namespace bench
