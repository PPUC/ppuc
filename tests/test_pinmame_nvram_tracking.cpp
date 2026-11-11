// Tests for the pinmame-nvram-maps field decoder.
//
// This decoder turns a byte of emulated CPU memory into the current ball or
// player number, and every game's map exercises a different corner of it: BCD
// across both nibbles, a single digit in one nibble with flags in the other,
// masks, offsets, and values that mean "not available yet". It is the subtlest
// logic in the PinMAME-facing code and it had no coverage at all, because it
// used to call libpinmame directly. It now takes an injected byte reader, which
// is the whole reason these tests can exist.
//
// A failed decode must leave the caller's value untouched: the main loop keeps
// the last known ball rather than reporting a wrong one.

#include <cstdint>
#include <optional>

#include "PinmameNvramTracking.h"
#include "doctest.h"

namespace
{

// Returns a reader that serves `value` at `address` and fails everywhere else,
// so a test that reads the wrong address fails loudly instead of silently
// decoding zero.
PinmameByteReader ReaderAt(uint32_t address, uint8_t value)
{
  return [address, value](uint32_t requested, uint8_t* pValue)
  {
    if (requested != address)
    {
      return false;
    }
    *pValue = value;
    return true;
  };
}

PinmameByteReader FailingReader()
{
  return [](uint32_t, uint8_t*) { return false; };
}

PinmameTrackedField Field(PinmameMapEncoding encoding, PinmameMapNibble nibble = PinmameMapNibble::BOTH)
{
  PinmameTrackedField field;
  field.available = true;
  field.address = 0x100;
  field.encoding = encoding;
  field.nibble = nibble;
  return field;
}

// Decodes and returns the value, or nullopt when the decoder refused.
std::optional<uint8_t> Decode(const PinmameTrackedField& field, const PinmameByteReader& reader)
{
  uint8_t value = 0xEE;  // poison: a refused decode must not touch it
  if (!TryDecodeTrackedPinmameValue(field, reader, &value))
  {
    return std::nullopt;
  }
  return value;
}

}  // namespace

TEST_CASE("int encoding returns the masked byte")
{
  const PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  CHECK(Decode(field, ReaderAt(0x100, 3)) == 3);
  CHECK(Decode(field, ReaderAt(0x100, 0)) == 0);
  CHECK(Decode(field, ReaderAt(0x100, 255)) == 255);
}

TEST_CASE("BCD across both nibbles decodes as two decimal digits")
{
  const PinmameTrackedField field = Field(PinmameMapEncoding::BCD);
  CHECK(Decode(field, ReaderAt(0x100, 0x00)) == 0);
  CHECK(Decode(field, ReaderAt(0x100, 0x03)) == 3);
  CHECK(Decode(field, ReaderAt(0x100, 0x12)) == 12);
  CHECK(Decode(field, ReaderAt(0x100, 0x99)) == 99);
}

TEST_CASE("BCD keeps the valid digit when the other nibble holds flags")
{
  // Games that store one decimal digit alongside a flags nibble would otherwise
  // decode 0xF1 as 151. The rule is: prefer the low nibble, then the high one.
  const PinmameTrackedField field = Field(PinmameMapEncoding::BCD);
  CHECK(Decode(field, ReaderAt(0x100, 0xF1)) == 1);
  CHECK(Decode(field, ReaderAt(0x100, 0xA4)) == 4);
  CHECK(Decode(field, ReaderAt(0x100, 0x2F)) == 2);
  CHECK(Decode(field, ReaderAt(0x100, 0x9C)) == 9);
}

TEST_CASE("BCD refuses when neither nibble is a decimal digit")
{
  const PinmameTrackedField field = Field(PinmameMapEncoding::BCD);
  CHECK(Decode(field, ReaderAt(0x100, 0xFF)) == std::nullopt);
  CHECK(Decode(field, ReaderAt(0x100, 0xAB)) == std::nullopt);
}

TEST_CASE("nibble selection isolates half the byte")
{
  CHECK(Decode(Field(PinmameMapEncoding::INT, PinmameMapNibble::HIGH), ReaderAt(0x100, 0x37)) == 3);
  CHECK(Decode(Field(PinmameMapEncoding::INT, PinmameMapNibble::LOW), ReaderAt(0x100, 0x37)) == 7);

  // Once a nibble is selected the value is already a single digit, so the BCD
  // both-nibbles conversion must not run again and turn 7 into 7*10.
  CHECK(Decode(Field(PinmameMapEncoding::BCD, PinmameMapNibble::HIGH), ReaderAt(0x100, 0x37)) == 3);
  CHECK(Decode(Field(PinmameMapEncoding::BCD, PinmameMapNibble::LOW), ReaderAt(0x100, 0x37)) == 7);

  // A nibble read must not be rejected for holding a non-decimal value.
  CHECK(Decode(Field(PinmameMapEncoding::BCD, PinmameMapNibble::LOW), ReaderAt(0x100, 0x0F)) == 15);
}

TEST_CASE("mask is applied before the nibble and encoding")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  field.mask = 0x07;
  CHECK(Decode(field, ReaderAt(0x100, 0xFF)) == 7);
  CHECK(Decode(field, ReaderAt(0x100, 0xF2)) == 2);

  PinmameTrackedField bcd = Field(PinmameMapEncoding::BCD);
  bcd.mask = 0x0F;
  // Masking away the flags nibble leaves a clean single digit.
  CHECK(Decode(bcd, ReaderAt(0x100, 0xF6)) == 6);
}

TEST_CASE("offset shifts the decoded value")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  field.offset = 1;
  CHECK(Decode(field, ReaderAt(0x100, 0)) == 1);

  field.offset = -1;
  CHECK(Decode(field, ReaderAt(0x100, 3)) == 2);
}

TEST_CASE("offset that leaves the byte range is refused")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);

  field.offset = -1;
  CHECK(Decode(field, ReaderAt(0x100, 0)) == std::nullopt);

  field.offset = 1;
  CHECK(Decode(field, ReaderAt(0x100, 255)) == std::nullopt);
}

TEST_CASE("treatZeroAsUnavailable refuses only zero")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  field.treatZeroAsUnavailable = true;
  CHECK(Decode(field, ReaderAt(0x100, 0)) == std::nullopt);
  CHECK(Decode(field, ReaderAt(0x100, 1)) == 1);

  // The zero test happens before the offset is applied, so a field that maps
  // "0 means no game" onto a 1-based ball number still refuses zero.
  field.offset = 1;
  CHECK(Decode(field, ReaderAt(0x100, 0)) == std::nullopt);
  CHECK(Decode(field, ReaderAt(0x100, 1)) == 2);
}

TEST_CASE("an unavailable field is never read")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  field.available = false;

  bool read = false;
  const PinmameByteReader trap = [&read](uint32_t, uint8_t* pValue)
  {
    read = true;
    *pValue = 5;
    return true;
  };

  CHECK(Decode(field, trap) == std::nullopt);
  CHECK_FALSE(read);
}

TEST_CASE("a failed read propagates and leaves the value untouched")
{
  const PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  CHECK(Decode(field, FailingReader()) == std::nullopt);

  uint8_t value = 42;
  CHECK_FALSE(TryDecodeTrackedPinmameValue(field, FailingReader(), &value));
  CHECK(value == 42);
}

TEST_CASE("a missing reader or output pointer is refused rather than crashing")
{
  const PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  uint8_t value = 0;
  CHECK_FALSE(TryDecodeTrackedPinmameValue(field, PinmameByteReader{}, &value));
  CHECK_FALSE(TryDecodeTrackedPinmameValue(field, ReaderAt(0x100, 1), nullptr));
}

TEST_CASE("the configured address is the one read")
{
  PinmameTrackedField field = Field(PinmameMapEncoding::INT);
  field.address = 0x1F40;
  CHECK(Decode(field, ReaderAt(0x1F40, 4)) == 4);
  CHECK(Decode(field, ReaderAt(0x1F41, 4)) == std::nullopt);
}

// ---- game_over --------------------------------------------------------------
//
// Attract mode, and with it the NVRAM saves, follow this flag. Every form below
// occurs in pinmame-nvram-maps: WPC's plain byte, Data East's inverted one,
// Stern's masked bits and System 6's two-bit status.

namespace
{

PinmameTrackedFlag Flag()
{
  PinmameTrackedFlag flag;
  flag.available = true;
  flag.address = 0x8D;
  return flag;
}

std::optional<bool> DecodeFlag(const PinmameTrackedFlag& flag, const PinmameByteReader& reader)
{
  bool value = false;
  bool poison = true;
  if (!TryDecodeTrackedPinmameFlag(flag, reader, &value))
  {
    // A refused decode must not touch the caller's value.
    CHECK(TryDecodeTrackedPinmameFlag(flag, reader, &poison) == false);
    CHECK(poison);
    return std::nullopt;
  }
  return value;
}

}  // namespace

TEST_CASE("a bool flag is set by any non-zero byte")
{
  const PinmameTrackedFlag flag = Flag();
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0)) == false);
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 1)) == true);
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0xFF)) == true);
}

TEST_CASE("an inverted bool flag reads the other way round")
{
  PinmameTrackedFlag flag = Flag();
  flag.invert = true;
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0)) == true);
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 3)) == false);
}

TEST_CASE("a masked bool flag looks only at its bits")
{
  PinmameTrackedFlag flag = Flag();
  flag.mask = 0x04;
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0xFB)) == false);
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0x04)) == true);
}

TEST_CASE("an enum flag looks the masked byte up in its values")
{
  // System 6: status in the low two bits, 1 means game over, and a tilt (2 or
  // 3) is still a game.
  PinmameTrackedFlag flag = Flag();
  flag.address = 0x78;
  flag.mask = 0x03;
  flag.enumValues = {false, true, false, false};
  CHECK(DecodeFlag(flag, ReaderAt(0x78, 0x00)) == false);
  CHECK(DecodeFlag(flag, ReaderAt(0x78, 0x01)) == true);
  CHECK(DecodeFlag(flag, ReaderAt(0x78, 0x02)) == false);
  CHECK(DecodeFlag(flag, ReaderAt(0x78, 0xF1)) == true);
}

TEST_CASE("an enum byte the values do not cover does not decode")
{
  PinmameTrackedFlag flag = Flag();
  flag.enumValues = {false, true};
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 2)) == std::nullopt);
}

TEST_CASE("a flag in a nibble-wide platform range reads only that nibble")
{
  PinmameTrackedFlag flag = Flag();
  flag.nibble = PinmameMapNibble::LOW;
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0xF0)) == false);
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 0x01)) == true);
}

TEST_CASE("an unavailable flag, a failed read or a missing reader does not decode")
{
  PinmameTrackedFlag flag = Flag();
  CHECK(DecodeFlag(flag, FailingReader()) == std::nullopt);
  CHECK(DecodeFlag(flag, ReaderAt(0x8E, 1)) == std::nullopt);
  bool value = false;
  CHECK_FALSE(TryDecodeTrackedPinmameFlag(flag, PinmameByteReader{}, &value));
  CHECK_FALSE(TryDecodeTrackedPinmameFlag(flag, ReaderAt(0x8D, 1), nullptr));
  flag.available = false;
  CHECK(DecodeFlag(flag, ReaderAt(0x8D, 1)) == std::nullopt);
}
