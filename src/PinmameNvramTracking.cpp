#include "PinmameNvramTracking.h"

#include <climits>

static uint8_t ApplyNibble(uint8_t value, PinmameMapNibble nibble)
{
  switch (nibble)
  {
    case PinmameMapNibble::HIGH:
      return static_cast<uint8_t>((value >> 4) & 0x0F);
    case PinmameMapNibble::LOW:
      return static_cast<uint8_t>(value & 0x0F);
    case PinmameMapNibble::BOTH:
      break;
  }
  return value;
}

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

  const uint8_t value = ApplyNibble(static_cast<uint8_t>(rawByte & field.mask), field.nibble);

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

bool TryDecodeTrackedPinmameFlag(const PinmameTrackedFlag& flag, const PinmameByteReader& readByte, bool* pValue)
{
  if (!flag.available || pValue == nullptr || !readByte)
  {
    return false;
  }

  uint8_t rawByte = 0;
  if (!readByte(flag.address, &rawByte))
  {
    return false;
  }

  const uint8_t value = ApplyNibble(static_cast<uint8_t>(rawByte & flag.mask), flag.nibble);
  bool decoded;
  if (flag.enumValues.empty())
  {
    decoded = value != 0;
  }
  else
  {
    if (value >= flag.enumValues.size())
    {
      return false;
    }
    decoded = flag.enumValues[value];
  }

  *pValue = flag.invert ? !decoded : decoded;
  return true;
}
