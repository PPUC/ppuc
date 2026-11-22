#pragma once

#include <string>

#include "ScoreboardModel.h"

namespace scoreboard
{

// Reads `<game folder>/scoreboard/scoreboard.yaml`. The pictures and the font
// it names are relative to that folder.
//
// False with *pError when the file is there but wrong: a broken layout is
// reported at startup, with the line, rather than drawn half.
bool LoadLayout(const std::string& path, Layout* pLayout, std::string* pError);

}  // namespace scoreboard
