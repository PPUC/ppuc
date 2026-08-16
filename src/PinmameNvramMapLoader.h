#pragma once

#include <cstdint>
#include <string>

#include "PinmameNvramTracking.h"

// Discovery and parsing of pinmame-nvram-maps assets.
//
// The hardware generation crosses this interface as a plain uint64_t rather
// than PINMAME_HARDWARE_GEN so the header stays free of libpinmame; only the
// implementation needs the generation constants.

// Human-readable rendering of a PINMAME_HARDWARE_GEN bitmask, e.g.
// "0x80 (WPCDMD)". Used for the startup log line.
std::string DescribeHardwareGen(uint64_t hardwareGen);

// Locates the nvram map for `rom`, verifies it matches the reported hardware
// generation, and fills *pConfig with the current_player / current_ball fields.
// `pinmamePath` may be null, in which case the per-user PinMAME directory is
// used. Returns false and fills *pError when no usable map was found; that is a
// normal outcome for a ROM nobody has mapped, not a failure worth aborting on.
bool TryLoadPinmameTrackingConfig(const char* rom, uint64_t hardwareGen, const char* pinmamePath,
                                  PinmameTrackingConfig* pConfig, std::string* pError);
