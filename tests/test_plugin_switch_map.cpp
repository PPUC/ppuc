// The PinMAME switch-number mapping.
//
// Two transforms stack here and both fail silently when wrong. PPUC numbers
// cabinet switches above 240 and PinMAME numbers them negative, so 243 means
// -3. And the plugin API carries the PinMAME number in a uint32_t mappingId
// that a negative switch reaches by truncation, so recovering it needs an
// int16_t round-trip. Get either wrong and the switch simply never arrives --
// no error, no log, just a coin door that does nothing.
//
// PluginEngine::SendSwitch cannot be reached without a live plugin, so the two
// transforms are mirrored here exactly as it applies them.

#include "doctest.h"

#include <cstdint>

namespace
{

// As PluginEngine::SendSwitch computes it.
int ToPinmameSwitch(int ppucNumber) { return (ppucNumber < 241) ? ppucNumber : 240 - ppucNumber; }

bool IsBoardLocal(int ppucNumber) { return ppucNumber >= 200 && ppucNumber <= 241; }

// As OnStateSrcChanged recovers it from StateDef::mappingId.
int FromMappingId(uint32_t mappingId) { return static_cast<int>(static_cast<int16_t>(mappingId)); }

}  // namespace

TEST_CASE("Switch numbers above 240 map into PinMAME's negative space")
{
  CHECK(ToPinmameSwitch(243) == -3);
  CHECK(ToPinmameSwitch(242) == -2);
  CHECK(ToPinmameSwitch(250) == -10);
}

TEST_CASE("Playfield switch numbers pass through unchanged")
{
  CHECK(ToPinmameSwitch(1) == 1);
  CHECK(ToPinmameSwitch(11) == 11);
  CHECK(ToPinmameSwitch(88) == 88);
}

TEST_CASE("241 is the boundary and belongs to the negative space")
{
  // 240 is the last number that passes through; 241 is the first that folds.
  CHECK(ToPinmameSwitch(240) == 240);
  CHECK(ToPinmameSwitch(241) == -1);
}

TEST_CASE("Board-local switches never reach the ROM")
{
  CHECK(IsBoardLocal(200));
  CHECK(IsBoardLocal(207));
  CHECK(IsBoardLocal(241));
  CHECK_FALSE(IsBoardLocal(199));
  CHECK_FALSE(IsBoardLocal(242));
}

TEST_CASE("Negative PinMAME switches survive the uint32 mappingId round-trip")
{
  // This is how libpinmame publishes them: static_cast<uint16_t>(swNo) widened
  // into mappingId, so -3 arrives as 0xFFFD.
  CHECK(FromMappingId(static_cast<uint32_t>(static_cast<uint16_t>(-3))) == -3);
  CHECK(FromMappingId(0xFFFDu) == -3);
  CHECK(FromMappingId(static_cast<uint32_t>(static_cast<uint16_t>(-1))) == -1);
  CHECK(FromMappingId(static_cast<uint32_t>(static_cast<uint16_t>(-16))) == -16);
}

TEST_CASE("Positive PinMAME switches round-trip unchanged")
{
  CHECK(FromMappingId(0u) == 0);
  CHECK(FromMappingId(11u) == 11);
  CHECK(FromMappingId(88u) == 88);
  CHECK(FromMappingId(240u) == 240);
}

TEST_CASE("A cabinet switch survives both transforms end to end")
{
  // 243 in the game YAML must reach the StateDef whose mappingId is 0xFFFD.
  const int pinmameNumber = ToPinmameSwitch(243);
  CHECK(pinmameNumber == -3);
  CHECK(FromMappingId(0xFFFDu) == pinmameNumber);
}
