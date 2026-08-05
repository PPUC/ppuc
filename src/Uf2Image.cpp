#include "Uf2Image.h"

#include <cstdio>
#include <cstring>

namespace uf2 {

namespace {

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

Uf2Image Reject(const std::string& why) {
  Uf2Image image;
  image.valid = false;
  image.error = why;
  return image;
}

}  // namespace

Uf2Image ParseUf2(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr || length == 0) {
    return Reject("image is empty");
  }
  if ((length % kUf2BlockBytes) != 0) {
    return Reject("length " + std::to_string(length) +
                  " is not a whole number of 512-byte UF2 blocks");
  }

  Uf2Image image;
  const size_t blockCount = length / kUf2BlockBytes;
  uint32_t expectedAddress = 0;
  bool first = true;

  for (size_t i = 0; i < blockCount; ++i) {
    const uint8_t* block = bytes + i * kUf2BlockBytes;

    if (ReadLe32(block) != kUf2Magic0 || ReadLe32(block + 4) != kUf2Magic1 ||
        ReadLe32(block + kUf2BlockBytes - 4) != kUf2MagicEnd) {
      return Reject("block " + std::to_string(i) + " is not a UF2 block");
    }

    const uint32_t flags = ReadLe32(block + 8);
    const uint32_t targetAddress = ReadLe32(block + 12);
    const uint32_t payloadSize = ReadLe32(block + 16);

    // Blocks that are not destined for main flash carry metadata, not
    // firmware. Skipping them keeps the image contiguous.
    if ((flags & kUf2FlagNotMainFlash) != 0) {
      continue;
    }
    if (payloadSize > kUf2MaxPayloadBytes) {
      return Reject("block " + std::to_string(i) + " claims " +
                    std::to_string(payloadSize) + " payload bytes, max is " +
                    std::to_string(kUf2MaxPayloadBytes));
    }

    if ((flags & kUf2FlagFamilyIdPresent) != 0) {
      const uint32_t family = ReadLe32(block + 28);
      if (image.blocks == 0) {
        image.familyId = family;
      } else if (family != image.familyId) {
        return Reject("image mixes UF2 family ids");
      }
    }

    if (first) {
      image.baseAddress = targetAddress;
      expectedAddress = targetAddress;
      first = false;
    } else if (targetAddress != expectedAddress) {
      // A gap would be filled with nothing in particular once the payload is
      // concatenated, and the board would flash it.
      return Reject("block " + std::to_string(i) + " jumps to 0x" +
                    std::to_string(targetAddress) + ", expected contiguous 0x" +
                    std::to_string(expectedAddress));
    }

    image.data.insert(image.data.end(), block + 32, block + 32 + payloadSize);
    expectedAddress += payloadSize;
    image.blocks++;
  }

  if (image.data.empty()) {
    return Reject("image contains no flash payload");
  }

  image.valid = true;
  return image;
}

Uf2Image LoadUf2File(const std::string& path) {
  FILE* file = fopen(path.c_str(), "rb");
  if (!file) {
    return Reject("cannot open " + path);
  }

  std::vector<uint8_t> bytes;
  uint8_t buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    bytes.insert(bytes.end(), buffer, buffer + read);
  }
  fclose(file);

  return ParseUf2(bytes.data(), bytes.size());
}

}  // namespace uf2
