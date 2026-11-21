// Tests for splitting a rule's sound command into the bytes a ROM writes.
//
// libaltsound reassembles commands from sound-board bytes, by rules that
// differ per hardware generation. Splitting a command wrongly does not fail
// loudly: libaltsound pairs the wrong bytes and plays some other sample, or
// keeps a dangling byte that silently corrupts the next real command. Nothing
// shows that except listening, so the byte sequences are pinned here.

#include <cstdint>
#include <string>
#include <vector>

#include "AltSoundCommand.h"
#include "doctest.h"

using namespace AltSoundCommand;

namespace
{

std::vector<uint32_t> Bytes(uint64_t gen, uint32_t cmd)
{
  std::vector<uint32_t> bytes;
  std::string error;
  REQUIRE_MESSAGE(Encode(gen, cmd, &bytes, &error), error);
  return bytes;
}

bool Rejected(uint64_t gen, uint32_t cmd)
{
  std::vector<uint32_t> bytes;
  std::string error;
  const bool ok = Encode(gen, cmd, &bytes, &error);
  if (!ok)
  {
    CHECK_FALSE(error.empty());
  }
  return !ok;
}

}  // namespace

TEST_CASE("the generation names an operator writes are understood")
{
  uint64_t gen = 99;
  CHECK(ParseHardware("wpcs", &gen));
  CHECK(gen == kGenWpcSecurity);
  CHECK(ParseHardware("WPC-Security", &gen));
  CHECK(gen == kGenWpcSecurity);
  CHECK(ParseHardware("wpc_dcs", &gen));
  CHECK(gen == kGenWpcDcs);
  CHECK(ParseHardware("WPC95", &gen));
  CHECK(gen == kGenWpc95);
  CHECK(ParseHardware("none", &gen));
  CHECK(gen == kGenNone);
  CHECK(ParseHardware("0x20", &gen));
  CHECK(gen == kGenWpcSecurity);
  CHECK(ParseHardware("16", &gen));
  CHECK(gen == kGenWpcDcs);
  CHECK_FALSE(ParseHardware("wpc-ss", &gen));
  CHECK_FALSE(ParseHardware("", &gen));
  CHECK_FALSE(ParseHardware("0x2g", &gen));
}

TEST_CASE("the generation values are PinMAME's, which libaltsound repeats")
{
  // PinMAME hands its GEN_* value to AltSound unchanged, and libaltsound
  // switches on its own enum of the same numbers. A value off by a bit would
  // land in libaltsound's default case and read DCS bytes one at a time.
  CHECK(kGenWpcDcs == 0x10);
  CHECK(kGenWpcSecurity == 0x20);
  CHECK(kGenWpc95Dcs == 0x40);
  CHECK(kGenWpc95 == 0x80);
  CHECK(kGenS11 == 0x80000000ULL);
  CHECK(kGenGts80 == 0x200000000ULL);
  CHECK(kGenWs == 0x4000000000ULL);
  CHECK(HardwareName(kGenWpcSecurity) == "wpcs");
  CHECK(HardwareName(0x40000) == "0x40000");
}

TEST_CASE("DCS commands go over as high byte, then low byte")
{
  for (const uint64_t gen : {kGenWpcDcs, kGenWpcSecurity, kGenWpc95Dcs, kGenWpc95})
  {
    CAPTURE(gen);
    CHECK(SendsBytePairs(gen));
    CHECK(Bytes(gen, 0x03E6) == std::vector<uint32_t>{0x03, 0xE6});
    // A command below 0x100 is still a pair: a lone byte would pair with the
    // next command's high byte.
    CHECK(Bytes(gen, 0x0015) == std::vector<uint32_t>{0x00, 0x15});
    // A custom game's own numbers work the same way.
    CHECK(Bytes(gen, 0x1000) == std::vector<uint32_t>{0x10, 0x00});
    CHECK(Rejected(gen, 0x10000));
  }
}

TEST_CASE("pre-DCS WPC sends one byte, or 0x7A and a second one")
{
  CHECK_FALSE(SendsBytePairs(kGenWpcFliptron));
  CHECK(Bytes(kGenWpcFliptron, 0x45) == std::vector<uint32_t>{0x45});
  CHECK(Bytes(kGenWpcDmd, 0x7A12) == std::vector<uint32_t>{0x7A, 0x12});
  CHECK(Rejected(kGenWpcDmd, 0x7A));
  CHECK(Rejected(kGenWpcDmd, 0x1234));
}

TEST_CASE("single-byte generations refuse what they cannot say")
{
  CHECK(Bytes(kGenS11C, 0x20) == std::vector<uint32_t>{0x20});
  CHECK(Rejected(kGenS11X, 0x0120));
  CHECK(Bytes(kGenDe, 0x42) == std::vector<uint32_t>{0x42});
  CHECK(Rejected(kGenDeDmd32, 0x00));
  CHECK(Rejected(kGenDeDmd32, 0xFF));
  CHECK(Rejected(kGenGts80, 0));
}

TEST_CASE("Whitestar commands always start with 0xFC to 0xFF")
{
  CHECK(Bytes(kGenWs, 0xFE20) == std::vector<uint32_t>{0xFE, 0x20});
  CHECK(Rejected(kGenWs, 0x0020));
}

TEST_CASE("without a generation the number goes over whole")
{
  // libaltsound's default case takes each value as a complete command, so a
  // pack is looked up by the number exactly as the rule wrote it.
  CHECK_FALSE(SendsBytePairs(kGenNone));
  CHECK(Bytes(kGenNone, 0x03E6) == std::vector<uint32_t>{0x03E6});
  CHECK(Bytes(kGenBy35, 0x1F) == std::vector<uint32_t>{0x1F});
}
