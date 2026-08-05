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

FirmwareFileName ParseFirmwareFileName(const std::string& fileName) {
  FirmwareFileName result;

  if (fileName.size() < 5 ||
      fileName.compare(fileName.size() - 4, 4, ".uf2") != 0) {
    return result;
  }
  std::string stem = fileName.substr(0, fileName.size() - 4);

  // Optional +<hex> build id, taken off the end first so the version parse
  // below does not have to know about it.
  const size_t plus = stem.rfind('+');
  if (plus != std::string::npos) {
    const std::string idText = stem.substr(plus + 1);
    if (idText.empty() || idText.size() > 8) {
      return result;
    }
    uint32_t id = 0;
    for (char c : idText) {
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return result;  // not hex: refuse rather than guess
      }
      id = (id << 4) | digit;
    }
    result.hasBuildId = true;
    result.buildId = id;
    stem = stem.substr(0, plus);
  }

  // The board type may itself contain dashes and digits, so the version is
  // taken from the last dash and must parse completely.
  const size_t dash = stem.rfind('-');
  if (dash == std::string::npos || dash == 0) {
    return result;
  }

  unsigned major = 0, minor = 0, patch = 0;
  char trailing = 0;
  const std::string versionText = stem.substr(dash + 1);
  if (sscanf(versionText.c_str(), "%u.%u.%u%c", &major, &minor, &patch,
             &trailing) != 3) {
    return result;
  }
  if (major > 255 || minor > 255 || patch > 255) {
    return result;
  }

  result.boardTypeName = stem.substr(0, dash);
  result.version = versionText;
  result.versionOrdinal = (major << 16) | (minor << 8) | patch;
  result.valid = true;
  return result;
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
