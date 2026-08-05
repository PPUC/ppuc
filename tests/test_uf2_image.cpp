// Tests for UF2 parsing.
//
// This is the last point at which a bad firmware image can be refused for
// free. After it, bytes go to a board's flash. So the cases here are mostly
// about rejection: the parser is more useful for what it refuses than for what
// it accepts.

#include <vector>

#include "Uf2Image.h"
#include "doctest.h"

using uf2::kUf2BlockBytes;
using uf2::kUf2FamilyRp2040;
using uf2::kUf2FlagFamilyIdPresent;
using uf2::kUf2FlagNotMainFlash;
using uf2::kUf2Magic0;
using uf2::kUf2Magic1;
using uf2::kUf2MagicEnd;
using uf2::ParseUf2;

namespace {

void PutLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// One well-formed UF2 block. Tests corrupt individual fields from here so a
// failure names the field that was changed.
std::vector<uint8_t> Block(uint32_t address, uint32_t payloadSize,
                           uint8_t fill, uint32_t flags = kUf2FlagFamilyIdPresent,
                           uint32_t family = kUf2FamilyRp2040) {
  std::vector<uint8_t> block(kUf2BlockBytes, 0);
  PutLe32(&block[0], kUf2Magic0);
  PutLe32(&block[4], kUf2Magic1);
  PutLe32(&block[8], flags);
  PutLe32(&block[12], address);
  PutLe32(&block[16], payloadSize);
  PutLe32(&block[28], family);
  for (uint32_t i = 0; i < payloadSize; ++i) {
    block[32 + i] = fill;
  }
  PutLe32(&block[kUf2BlockBytes - 4], kUf2MagicEnd);
  return block;
}

std::vector<uint8_t> Concat(const std::vector<std::vector<uint8_t>>& parts) {
  std::vector<uint8_t> out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

}  // namespace

TEST_CASE("a two-block image parses into contiguous payload") {
  const auto bytes = Concat({Block(0x10000000, 256, 0xAA),
                             Block(0x10000100, 256, 0xBB)});
  const auto image = ParseUf2(bytes.data(), bytes.size());

  REQUIRE(image.valid);
  CHECK(image.baseAddress == 0x10000000);
  CHECK(image.blocks == 2);
  CHECK(image.data.size() == 512);
  CHECK(image.data.front() == 0xAA);
  CHECK(image.data[255] == 0xAA);
  CHECK(image.data[256] == 0xBB);
  CHECK(image.familyId == kUf2FamilyRp2040);
}

TEST_CASE("a gap in the address range is refused") {
  // Concatenating across a gap would flash whatever happened to follow, at an
  // address the image never claimed.
  const auto bytes = Concat({Block(0x10000000, 256, 0xAA),
                             Block(0x10000200, 256, 0xBB)});
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("contiguous") != std::string::npos);
}

TEST_CASE("a truncated file is refused") {
  auto bytes = Concat({Block(0x10000000, 256, 0xAA)});
  bytes.resize(bytes.size() - 8);
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("512-byte") != std::string::npos);
}

TEST_CASE("a block with the wrong magic is refused") {
  auto bytes = Concat({Block(0x10000000, 256, 0xAA)});
  bytes[0] ^= 0xFF;
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("not a UF2 block") != std::string::npos);
}

TEST_CASE("an oversized payload claim is refused") {
  auto bytes = Concat({Block(0x10000000, 256, 0xAA)});
  PutLe32(&bytes[16], 999);
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("payload bytes") != std::string::npos);
}

TEST_CASE("non-flash blocks are skipped without breaking contiguity") {
  // Metadata blocks sit between firmware blocks in some images. Treating them
  // as payload would both corrupt the image and look like an address gap.
  const auto bytes = Concat({
      Block(0x10000000, 256, 0xAA),
      Block(0x20000000, 16, 0xCC, kUf2FlagNotMainFlash),
      Block(0x10000100, 256, 0xBB),
  });
  const auto image = ParseUf2(bytes.data(), bytes.size());

  REQUIRE(image.valid);
  CHECK(image.blocks == 2);
  CHECK(image.data.size() == 512);
  CHECK(image.data[256] == 0xBB);
}

TEST_CASE("mixed family ids are refused") {
  const auto bytes = Concat({
      Block(0x10000000, 256, 0xAA, kUf2FlagFamilyIdPresent, kUf2FamilyRp2040),
      Block(0x10000100, 256, 0xBB, kUf2FlagFamilyIdPresent, 0x12345678),
  });
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("family") != std::string::npos);
}

TEST_CASE("an image of only metadata blocks is refused") {
  const auto bytes = Concat(
      {Block(0x20000000, 16, 0xCC, kUf2FlagNotMainFlash)});
  const auto image = ParseUf2(bytes.data(), bytes.size());

  CHECK_FALSE(image.valid);
  CHECK(image.error.find("no flash payload") != std::string::npos);
}

TEST_CASE("an empty buffer is refused rather than accepted as empty firmware") {
  const auto image = ParseUf2(nullptr, 0);
  CHECK_FALSE(image.valid);
}

TEST_CASE("a short final block is allowed") {
  // The last block of a real image is rarely a full 256 bytes.
  const auto bytes = Concat({Block(0x10000000, 256, 0xAA),
                             Block(0x10000100, 17, 0xBB)});
  const auto image = ParseUf2(bytes.data(), bytes.size());

  REQUIRE(image.valid);
  CHECK(image.data.size() == 273);
}

// --- firmware file names -----------------------------------------------------
//
// The name is how an image is paired with a board and how two builds are
// compared. A name that does not fit must be refused rather than
// half-understood: a misparsed board type flashes the wrong hardware.

using uf2::ParseFirmwareFileName;

TEST_CASE("a release name parses to type and version") {
  const auto n = ParseFirmwareFileName("IO_16_8_1-0.3.0.uf2");
  REQUIRE(n.valid);
  CHECK(n.boardTypeName == "IO_16_8_1");
  CHECK(n.version == "0.3.0");
  CHECK(n.versionOrdinal == ((0u << 16) | (3u << 8) | 0u));
  CHECK_FALSE(n.hasBuildId);
}

TEST_CASE("a snapshot name also carries a build id") {
  const auto n = ParseFirmwareFileName("IO_16_8_1-0.3.0+a1b2c3d4.uf2");
  REQUIRE(n.valid);
  CHECK(n.boardTypeName == "IO_16_8_1");
  CHECK(n.versionOrdinal == ((0u << 16) | (3u << 8) | 0u));
  REQUIRE(n.hasBuildId);
  CHECK(n.buildId == 0xa1b2c3d4);
}

TEST_CASE("board type names containing dashes and digits survive") {
  // The version is taken from the *last* dash, so a type name is free to
  // contain them.
  const auto n = ParseFirmwareFileName("IO_16x8_matrix-1.2.3.uf2");
  REQUIRE(n.valid);
  CHECK(n.boardTypeName == "IO_16x8_matrix");
  CHECK(n.versionOrdinal == ((1u << 16) | (2u << 8) | 3u));
}

TEST_CASE("a short build id is accepted") {
  const auto n = ParseFirmwareFileName("Opto_16-0.1.0+abc.uf2");
  REQUIRE(n.valid);
  REQUIRE(n.hasBuildId);
  CHECK(n.buildId == 0xabc);
}

TEST_CASE("a non-hex build id is refused rather than guessed at") {
  CHECK_FALSE(ParseFirmwareFileName("Opto_16-0.1.0+zzzz.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("Opto_16-0.1.0+.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("Opto_16-0.1.0+aabbccdde.uf2").valid);
}

TEST_CASE("names that are not firmware are refused") {
  CHECK_FALSE(ParseFirmwareFileName("notes.txt").valid);
  CHECK_FALSE(ParseFirmwareFileName("firmware.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("-1.2.3.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("IO_16_8_1-1.2.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("IO_16_8_1-1.2.3.4.uf2").valid);
  CHECK_FALSE(ParseFirmwareFileName("IO_16_8_1-x.y.z.uf2").valid);
}

TEST_CASE("a version component that cannot fit a byte is refused") {
  // The ordinal packs each component into 8 bits; accepting 256 would make
  // two different versions compare equal.
  CHECK_FALSE(ParseFirmwareFileName("IO_16_8_1-0.256.0.uf2").valid);
}

TEST_CASE("version ordering is by component, not lexicographic") {
  const auto nine = ParseFirmwareFileName("IO_16_8_1-0.9.0.uf2");
  const auto ten = ParseFirmwareFileName("IO_16_8_1-0.10.0.uf2");
  REQUIRE(nine.valid);
  REQUIRE(ten.valid);
  CHECK(ten.versionOrdinal > nine.versionOrdinal);
}
