// Tests for UF2 parsing.
//
// This is the last point at which a bad firmware image can be refused for
// free. After it, bytes go to a board's flash. So the cases here are mostly
// about rejection: the parser is more useful for what it refuses than for what
// it accepts.

#include <vector>

#include "Uf2Image.h"
#include "doctest.h"

using ppuc::kUf2BlockBytes;
using ppuc::kUf2FamilyRp2040;
using ppuc::kUf2FlagFamilyIdPresent;
using ppuc::kUf2FlagNotMainFlash;
using ppuc::kUf2Magic0;
using ppuc::kUf2Magic1;
using ppuc::kUf2MagicEnd;
using ppuc::ParseUf2;

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
