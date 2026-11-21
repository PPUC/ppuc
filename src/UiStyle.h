#pragma once

// What PPUC's own screens look like: the tools menu, the switch and coil
// monitor and the firmware update screen in ppuc-pinmame, and the game
// selector in ppuc-menu. Kept in one place so that the screen a player meets
// at power-on and the one a technician meets with a keyboard read as the same
// machine, and so a font that works on one works on both.
//
// Depends on nothing but SDL's colour struct.

#include "SDL3/SDL_pixels.h"

namespace uistyle
{

// Where a font might be. Deliberately a search rather than a setting: these
// screens have to work on a machine nobody configured for them. DejaVu is what
// the ZeDMDOS image ships; the last two are for a desk.
inline const char* const kFontCandidates[] = {
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
};

// Point sizes on a 1080-line screen.
constexpr float kFontLargePt = 48.0f;
constexpr float kFontSmallPt = 28.0f;
constexpr float kFontCompactPt = 23.0f;

constexpr SDL_Color kText{235, 235, 235, 255};
constexpr SDL_Color kDim{140, 140, 150, 255};
constexpr SDL_Color kAmber{255, 200, 70, 255};
constexpr SDL_Color kPanel{24, 26, 34, 255};
constexpr SDL_Color kPanelBorder{90, 96, 120, 255};
constexpr SDL_Color kRowSelected{44, 48, 62, 255};

}  // namespace uistyle
