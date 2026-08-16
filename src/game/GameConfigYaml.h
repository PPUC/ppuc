#pragma once

#include <string>
#include <vector>

#include "GameConfig.h"
#include "PlayfieldAssist.h"

// Parses the `emGame:` block of a game's io-boards.yaml into a GameConfig.
//
// Deliberately host-side. libppuc's validator ignores keys it does not name, so
// this needs no libppuc change, no SHA pin bump and no config-tool release to
// ship -- the schema is defined here, in one place, and can be promoted into
// libppuc's validator later once it has settled against a real machine.

// Every device number the machine declares. Collected from the same YAML
// document rather than from libppuc, because libppuc only populates its device
// lists during Connect() -- and the tilt inhibit switch has to be handed to it
// before that.
struct MachineDevices
{
  std::vector<int> switches;
  std::vector<int> coils;
  std::vector<int> lamps;
};

// Returns false and fills *pError when the block is present but wrong. Returns
// true with pConfig->enabled == false when there is no `emGame:` block at all,
// which is the normal case for a ROM game.
//
// The cross-reference checks are the point of this function: they turn the worst
// electro-mechanical failure mode -- "I press start and nothing happens" -- into
// a startup error that names the switch number and the file position.
bool LoadGameConfigFromYaml(const std::string& path, GameConfig* pConfig, std::string* pError);

// Reads the top-level `tilt:` and `ballSave:` blocks.
//
// Deliberately NOT under `emGame:`: tilt warnings and ball save work under both
// engines, and giving an early-electronic ROM these features is a large part of
// why they exist. A ROM game carrying an `emGame:` block would be nonsense.
bool LoadPlayfieldAssistFromYaml(const std::string& path, TiltAssistConfig* pTilt, BallSaveConfig* pBallSave,
                                 std::string* pError);
