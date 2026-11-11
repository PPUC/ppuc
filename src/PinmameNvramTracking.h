#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Decoding of the ball/player fields that pinmame-nvram-maps describes.
//
// This header is deliberately free of libpinmame, yaml-cpp and SDL so the
// decoder -- the subtlest logic in this area, and the part worth testing
// exhaustively -- can be linked into ppuc_tests on its own. Loading a map file
// lives in PinmameNvramMapLoader.h, which does need all three.

enum class PinmameMapEncoding
{
  INT,
  BCD
};

enum class PinmameMapNibble
{
  BOTH,
  HIGH,
  LOW
};

struct PinmameTrackedField
{
  bool available = false;
  uint32_t address = 0;
  PinmameMapEncoding encoding = PinmameMapEncoding::INT;
  PinmameMapNibble nibble = PinmameMapNibble::BOTH;
  uint8_t mask = 0xFF;
  int offset = 0;
  bool treatZeroAsUnavailable = false;
};

// A yes/no field: the map's `bool` and `enum` encodings, as game_over uses them.
//
// `bool` is true when the masked byte is non-zero, and `invert` turns that
// round. `enum` looks the masked byte up in a list of values, which for
// game_over are all booleans -- System 6 keeps a status in two bits of which
// only one value means "game over". A byte the list has no entry for is not a
// game state at all, and does not decode.
struct PinmameTrackedFlag
{
  bool available = false;
  uint32_t address = 0;
  PinmameMapNibble nibble = PinmameMapNibble::BOTH;
  uint8_t mask = 0xFF;
  bool invert = false;
  // Empty for `bool`; the value per masked byte for `enum`.
  std::vector<bool> enumValues;
};

struct PinmameTrackingConfig
{
  bool attemptedLoad = false;
  bool loaded = false;
  std::string mapPath;
  PinmameTrackedField currentPlayer;
  PinmameTrackedField currentBall;
  // Whether the ROM considers the game over. Optional: a map without it, or with
  // a form this cannot read, leaves it unavailable and says why in
  // gameOverUnavailableReason, and the ball and the player are tracked anyway.
  PinmameTrackedFlag gameOver;
  std::string gameOverUnavailableReason;
};

struct PinmamePlatformMemoryRange
{
  uint32_t address = 0;
  uint32_t size = 0;
  PinmameMapNibble nibble = PinmameMapNibble::BOTH;
};

// Reads one byte of emulated main-CPU memory. Returns false when the address is
// not readable. Injected rather than calling PinmameReadMainCPUByte directly so
// the decoder is testable without libpinmame and without a running machine.
using PinmameByteReader = std::function<bool(uint32_t address, uint8_t* pValue)>;

// Decodes one tracked field. Returns false when the field is unavailable, the
// read failed, the value is not valid for its encoding, or the offset pushes it
// out of range -- in which case *pValue is untouched and the caller keeps the
// previous value.
bool TryDecodeTrackedPinmameValue(const PinmameTrackedField& field, const PinmameByteReader& readByte,
                                  uint8_t* pValue);

// Decodes one yes/no field. Same contract as above: false leaves *pValue alone.
bool TryDecodeTrackedPinmameFlag(const PinmameTrackedFlag& flag, const PinmameByteReader& readByte, bool* pValue);
