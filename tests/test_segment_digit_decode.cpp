// Tests for segment display decoding.
//
// The bit table came across from PinmameEngine unchanged, but its input did
// not: the plugin API delivers sixteen floats per element instead of one 16-bit
// word, so the mask now has to be rebuilt before the table can be applied. That
// rebuild is where an alphanumeric game silently goes blank or starts
// flickering, and neither shows up anywhere except on a backglass.

#include <array>
#include <cstdint>

#include "SegmentDigitDecode.h"
#include "doctest.h"

namespace
{

using SegmentDigitDecode::kSegmentsPerElement;

// One element's frame: sixteen floats, `mask` mapped to `level`.
std::array<float, kSegmentsPerElement> Lit(uint16_t mask, float level = 1.0f)
{
  std::array<float, kSegmentsPerElement> frame{};
  for (size_t i = 0; i < kSegmentsPerElement; ++i)
  {
    frame[i] = (mask & (1u << i)) != 0 ? level : 0.0f;
  }
  return frame;
}

}  // namespace

TEST_CASE("the digit table decodes every numeral")
{
  CHECK(SegmentDigitDecode::DecodeDigit(0x003F) == 0);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0006) == 1);
  CHECK(SegmentDigitDecode::DecodeDigit(0x005B) == 2);
  CHECK(SegmentDigitDecode::DecodeDigit(0x004F) == 3);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0066) == 4);
  CHECK(SegmentDigitDecode::DecodeDigit(0x006D) == 5);
  CHECK(SegmentDigitDecode::DecodeDigit(0x007D) == 6);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0007) == 7);
  CHECK(SegmentDigitDecode::DecodeDigit(0x007F) == 8);
  CHECK(SegmentDigitDecode::DecodeDigit(0x006F) == 9);
}

TEST_CASE("the alternate shapes some ROMs use decode too")
{
  // 1 drawn on the right-hand pair of a 14-segment element, 6 without its top
  // bar, 9 without its tail. Dropping any of these turns a real score into
  // blanks.
  CHECK(SegmentDigitDecode::DecodeDigit(0x0300) == 1);
  CHECK(SegmentDigitDecode::DecodeDigit(0x007C) == 6);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0067) == 9);
}

TEST_CASE("the dot is ignored and anything else is not a digit")
{
  CHECK(SegmentDigitDecode::DecodeDigit(0x003F | 0x0080) == 0);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0000) == -1);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0080) == -1);
  // A letter, and a shape caught between two digits mid-strobe.
  CHECK(SegmentDigitDecode::DecodeDigit(0x0077) == -1);
  CHECK(SegmentDigitDecode::DecodeDigit(0x0036) == -1);
}

TEST_CASE("layout segment counts match the provider's")
{
  // CTLPI_SEG_LAYOUT_7, _7C, _7D, _9, _9C, _14, _14D, _14DC, _16.
  CHECK(SegmentDigitDecode::SegmentCountForLayout(0) == 7);
  CHECK(SegmentDigitDecode::SegmentCountForLayout(1) == 8);
  CHECK(SegmentDigitDecode::SegmentCountForLayout(5) == 15);
  CHECK(SegmentDigitDecode::SegmentCountForLayout(8) == 16);
  // A layout added upstream that this build does not know about must read the
  // whole element rather than truncate it to nothing.
  CHECK(SegmentDigitDecode::SegmentCountForLayout(99) == 16);
  CHECK(SegmentDigitDecode::SegmentCountForLayout(-1) == 16);
}

TEST_CASE("a strictly on/off frame rebuilds its mask exactly")
{
  // What System 6 actually delivers: coreGlobals.nAlphaSegs is unset, so the
  // provider writes 1.0f or 0.0f and nothing in between.
  const auto frame = Lit(0x003F);
  CHECK(SegmentDigitDecode::MaskFromLuminance(frame.data(), 7, 0) == 0x003F);
  CHECK(SegmentDigitDecode::DecodeDigit(SegmentDigitDecode::MaskFromLuminance(frame.data(), 7, 0)) == 0);
}

TEST_CASE("segments beyond the element's count never reach the mask")
{
  // The provider only writes the first nSegs floats of each element; the rest
  // hold whatever was there before. Reading all sixteen would fold that stale
  // data into the mask, and the table would then report a blank.
  std::array<float, kSegmentsPerElement> frame = Lit(0x003F);
  frame[10] = 1.0f;  // stale, past a 7-segment element
  frame[13] = 1.0f;

  CHECK(SegmentDigitDecode::MaskFromLuminance(frame.data(), 7, 0) == 0x003F);
  CHECK(SegmentDigitDecode::MaskFromLuminance(frame.data(), 16, 0) == 0x243F);
}

TEST_CASE("the on threshold matches the one alphadmd builds identify frames with")
{
  const auto below = Lit(0x0001, 0.49f);
  const auto atThreshold = Lit(0x0001, 0.5f);
  CHECK(SegmentDigitDecode::MaskFromLuminance(below.data(), 7, 0) == 0x0000);
  CHECK(SegmentDigitDecode::MaskFromLuminance(atThreshold.data(), 7, 0) == 0x0001);
}

TEST_CASE("a lit segment holds through the hysteresis band")
{
  // A modulated display's luminance is a duty cycle that wobbles. Without the
  // band a segment sitting near the threshold toggles every frame and the score
  // visibly flickers.
  const auto inBand = Lit(0x0001, 0.4f);
  CHECK(SegmentDigitDecode::MaskFromLuminance(inBand.data(), 7, 0x0001) == 0x0001);
  CHECK(SegmentDigitDecode::MaskFromLuminance(inBand.data(), 7, 0x0000) == 0x0000);

  const auto belowBand = Lit(0x0001, 0.29f);
  CHECK(SegmentDigitDecode::MaskFromLuminance(belowBand.data(), 7, 0x0001) == 0x0000);
}

TEST_CASE("a null frame leaves the previous mask standing")
{
  // Better a stale digit for one poll than a display that blanks whenever a
  // provider hands back nothing.
  CHECK(SegmentDigitDecode::MaskFromLuminance(nullptr, 7, 0x003F) == 0x003F);
  const auto frame = Lit(0x003F);
  CHECK(SegmentDigitDecode::MaskFromLuminance(frame.data(), 0, 0x0006) == 0x0006);
}
