#include "SegmentDigitDecode.h"

namespace SegmentDigitDecode
{

int DecodeDigit(uint16_t bitState)
{
  switch (bitState & ~0x0080u)
  {
    case 0x003Fu:
      return 0;
    case 0x0006u:
    case 0x0300u:
      return 1;
    case 0x005Bu:
      return 2;
    case 0x004Fu:
      return 3;
    case 0x0066u:
      return 4;
    case 0x006Du:
      return 5;
    case 0x007Du:
    case 0x007Cu:
      return 6;
    case 0x0007u:
      return 7;
    case 0x007Fu:
      return 8;
    case 0x006Fu:
    case 0x0067u:
      return 9;
    default:
      return -1;
  }
}

int SegmentCountForLayout(int elementType)
{
  // SegElementType order: 7, 7C, 7D, 9, 9C, 14, 14D, 14DC, 16. Same table as
  // alphadmd's, and it must stay that way for the same reason the threshold does.
  static constexpr int kCounts[] = {7, 8, 8, 10, 10, 15, 15, 16, 16};
  if (elementType < 0 || static_cast<size_t>(elementType) >= sizeof(kCounts) / sizeof(kCounts[0]))
  {
    return static_cast<int>(kSegmentsPerElement);
  }
  return kCounts[elementType];
}

uint16_t MaskFromLuminance(const float* luminances, int segmentCount, uint16_t previous)
{
  if (luminances == nullptr || segmentCount <= 0)
  {
    return previous;
  }
  const size_t count =
      static_cast<size_t>(segmentCount) < kSegmentsPerElement ? static_cast<size_t>(segmentCount) : kSegmentsPerElement;

  uint16_t mask = 0;
  for (size_t i = 0; i < count; ++i)
  {
    const float value = luminances[i];
    const uint16_t bit = static_cast<uint16_t>(1u << i);
    const bool wasOn = (previous & bit) != 0;
    const bool isOn = value >= kSegmentOnThreshold || (wasOn && value > kSegmentOffThreshold);
    if (isOn)
    {
      mask |= bit;
    }
  }
  return mask;
}

}  // namespace SegmentDigitDecode
