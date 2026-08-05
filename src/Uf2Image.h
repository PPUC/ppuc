#pragma once

// Reading a UF2 firmware image.
//
// UF2 is the format the io-boards CI produces and the same one you drag onto a
// board over USB. It is a sequence of 512-byte blocks, each carrying up to 476
// bytes of payload and the flash address that payload belongs at. Only the
// payload bytes go over RS485; the block headers are a container, not part of
// the firmware.
//
// Kept separate from the transfer so it can be tested without a bus: a
// malformed image should be rejected here, at the point where the file is
// read, rather than discovered halfway through writing a board's flash.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ppuc {

struct Uf2Image {
  bool valid = false;
  std::string error;          // why it was rejected, when !valid
  uint32_t baseAddress = 0;   // flash address the image starts at
  std::vector<uint8_t> data;  // contiguous payload, headers stripped
  uint32_t familyId = 0;
  size_t blocks = 0;
};

// The RP2040's UF2 family id. An image for another chip has to be refused: it
// would be accepted by the transfer and then bricked on arrival.
constexpr uint32_t kUf2FamilyRp2040 = 0xe48bff56u;

constexpr size_t kUf2BlockBytes = 512;
constexpr size_t kUf2MaxPayloadBytes = 476;
constexpr uint32_t kUf2Magic0 = 0x0A324655u;
constexpr uint32_t kUf2Magic1 = 0x9E5D5157u;
constexpr uint32_t kUf2MagicEnd = 0x0AB16F30u;
constexpr uint32_t kUf2FlagFamilyIdPresent = 0x00002000u;
constexpr uint32_t kUf2FlagNotMainFlash = 0x00000001u;

// Parses UF2 blocks into one contiguous image.
//
// Requires the payload to be contiguous and ascending. A gap would otherwise
// be silently filled with whatever the vector was padded with, which is not
// something to discover on a board.
Uf2Image ParseUf2(const uint8_t* bytes, size_t length);

// Reads a UF2 file from disk and parses it.
Uf2Image LoadUf2File(const std::string& path);

}  // namespace ppuc
