#include "PinmameNvramTracking.h"

#include <climits>

bool TryDecodeTrackedPinmameValue(const PinmameTrackedField& field, const PinmameByteReader& readByte, uint8_t* pValue)
{
  if (!field.available || pValue == nullptr || !readByte)
  {
    return false;
  }

  uint8_t rawByte = 0;
  if (!readByte(field.address, &rawByte))
  {
    return false;
  }

  uint8_t value = static_cast<uint8_t>(rawByte & field.mask);
  switch (field.nibble)
  {
    case PinmameMapNibble::HIGH:
      value = static_cast<uint8_t>((value >> 4) & 0x0F);
      break;
    case PinmameMapNibble::LOW:
      value = static_cast<uint8_t>(value & 0x0F);
      break;
    case PinmameMapNibble::BOTH:
      break;
  }

  uint8_t decodedValue = value;
  if (field.encoding == PinmameMapEncoding::BCD)
  {
    if (field.nibble == PinmameMapNibble::BOTH)
    {
      const uint8_t highNibble = static_cast<uint8_t>((value >> 4) & 0x0F);
      const uint8_t lowNibble = static_cast<uint8_t>(value & 0x0F);

      // Some games store a single decimal digit in one nibble and use the
      // other nibble for flags. If one nibble is not valid BCD, keep the
      // valid digit instead of decoding values like 0xF1 as 151.
      if (highNibble <= 9 && lowNibble <= 9)
      {
        decodedValue = static_cast<uint8_t>(highNibble * 10 + lowNibble);
      }
      else if (lowNibble <= 9)
      {
        decodedValue = lowNibble;
      }
      else if (highNibble <= 9)
      {
        decodedValue = highNibble;
      }
      else
      {
        return false;
      }
    }
  }

  if (field.treatZeroAsUnavailable && decodedValue == 0)
  {
    return false;
  }

  const int adjustedValue = static_cast<int>(decodedValue) + field.offset;
  if (adjustedValue < 0 || adjustedValue > UCHAR_MAX)
  {
    return false;
  }

  *pValue = static_cast<uint8_t>(adjustedValue);
  return true;
}
