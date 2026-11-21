#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How a sound command from a Lua rule has to reach libaltsound to mean what it
// means when a ROM sends it.
//
// libaltsound is not handed sound commands; it is handed the bytes the CPU
// board wrote to the sound board, one at a time, and reassembles commands from
// them by rules that differ per hardware generation. A WPC DCS / WPC-S / WPC95
// ROM writes every command as two bytes, high byte first, and the pack is keyed
// by the 16-bit result. A WPC DMD ROM writes one byte, or 0x7A and a second
// byte. System 11 writes single bytes. A pack's command numbers are the
// reassembled values, so a rule naming the pack's number has to be split the
// way the ROM would have split it, or libaltsound pairs the wrong bytes and
// plays a different sample -- or, worse, swallows the next real command.
//
// Dependency-free on purpose: ppuc_tests covers it without libaltsound.

namespace AltSoundCommand
{

// PinMAME's GEN_* values, which libaltsound's ALTSOUND_HARDWARE_GEN repeats
// verbatim (PinMAME hands its own value straight through). Duplicated here
// instead of including altsound.h, which only the plugin links.
constexpr uint64_t kGenNone = 0;
constexpr uint64_t kGenWpcAlpha1 = 0x1;
constexpr uint64_t kGenWpcAlpha2 = 0x2;
constexpr uint64_t kGenWpcDmd = 0x4;
constexpr uint64_t kGenWpcFliptron = 0x8;
constexpr uint64_t kGenWpcDcs = 0x10;
constexpr uint64_t kGenWpcSecurity = 0x20;
constexpr uint64_t kGenWpc95Dcs = 0x40;
constexpr uint64_t kGenWpc95 = 0x80;
constexpr uint64_t kGenS11X = 0x100;
constexpr uint64_t kGenS11B2 = 0x200;
constexpr uint64_t kGenS11C = 0x400;
constexpr uint64_t kGenDe = 0x1000;
constexpr uint64_t kGenDeDmd16 = 0x2000;
constexpr uint64_t kGenDeDmd32 = 0x4000;
constexpr uint64_t kGenDeDmd64 = 0x8000;
constexpr uint64_t kGenBy17 = 0x200000;
constexpr uint64_t kGenBy35 = 0x400000;
constexpr uint64_t kGenS11 = 0x80000000;
constexpr uint64_t kGenGts80 = 0x200000000;
constexpr uint64_t kGenWs = 0x4000000000;
constexpr uint64_t kGenWs1 = 0x8000000000;
constexpr uint64_t kGenWs2 = 0x10000000000;

// Parses the AltSoundHardware INI value: a name such as "wpcs", "wpc-dcs",
// "wpc95", "s11c" or "none" (case, '-', '_' and spaces ignored), or a number
// (decimal or 0x hex) that is used as is. Returns false for anything else.
bool ParseHardware(const std::string& text, uint64_t* hardwareGen);

// The canonical name of a generation, for logs; "0x..." for one without a name.
std::string HardwareName(uint64_t hardwareGen);

// Whether this generation sends every command as a high/low byte pair. While a
// ROM's pair is half sent, a rule's command must wait, or the two interleave.
bool SendsBytePairs(uint64_t hardwareGen);

// Splits `cmd` into the bytes the ROM of this generation would write, in
// order. Returns false and says why when the generation has no way to express
// the command (e.g. 0x1234 on System 11, which only knows single bytes).
//
// With no generation (kGenNone), or one libaltsound reads one value at a time
// (Bally, Gottlieb System 80), the command is handed over whole: libaltsound
// then looks the full number up in the pack as given.
bool Encode(uint64_t hardwareGen, uint32_t cmd, std::vector<uint32_t>* bytes, std::string* error);

}  // namespace AltSoundCommand
