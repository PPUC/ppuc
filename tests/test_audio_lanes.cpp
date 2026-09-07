// Tests for the audio override decision.
//
// This is the logic that makes AltSound mode 1 possible, and it is the kind of
// logic that fails quietly: get it wrong in one direction and the ROM plays
// underneath every AltSound sample, get it wrong in the other and the ROM is
// silent exactly as it was before the migration, which looks like nothing
// changed rather than like a bug.

#include <string>
#include <vector>

#include "AudioLanes.h"
#include "doctest.h"

namespace
{

// Matches the real topology: libpinmame publishes {pinmameEndpoint, 0} and
// AltSound publishes its own source overriding it.
constexpr uint64_t kRom = 0x0000000300000000ull;
constexpr uint64_t kAltSound = 0x0000000400000000ull;
constexpr uint64_t kSecondPack = 0x0000000500000000ull;

std::vector<AudioLanes::Source> RomOnly() { return {{kRom, 0, "PinMAME"}}; }

std::vector<AudioLanes::Source> RomAndAltSound() { return {{kRom, 0, "PinMAME"}, {kAltSound, kRom, "AltSound"}}; }

}  // namespace

TEST_CASE("an unoverridden source always plays")
{
  AudioLanes::Table table;
  table.SetSources(RomOnly(), 0);
  CHECK(table.ShouldPlay(kRom, 0));
  CHECK(table.ShouldPlay(kRom, 100000));
}

TEST_CASE("a source nobody has published yet plays")
{
  // A stream can arrive before the source list describing it. Dropping audio
  // over that ordering would be worse than a brief leak.
  AudioLanes::Table table;
  table.SetSources(RomOnly(), 0);
  CHECK(table.ShouldPlay(kAltSound, 0));
}

TEST_CASE("Replace mode silences an overridden source unconditionally")
{
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Replace);
  table.SetSources(RomAndAltSound(), 0);

  CHECK(table.ShouldPlay(kAltSound, 0));
  CHECK_FALSE(table.ShouldPlay(kRom, 0));

  // Still silent long after AltSound last made a sound: this is the old
  // PinmameSetSoundMode behaviour, all or nothing.
  table.NoteMixed(kAltSound, false, 60000);
  CHECK_FALSE(table.ShouldPlay(kRom, 60000));
}

TEST_CASE("Fallback mode lets the ROM through once the pack goes quiet")
{
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Fallback);
  table.SetFallbackHoldMs(250);
  table.SetSources(RomAndAltSound(), 1000);

  // AltSound is playing: the ROM stays under it.
  table.NoteMixed(kAltSound, true, 1000);
  CHECK_FALSE(table.ShouldPlay(kRom, 1000));

  // Within the hold window, still under it. A gap between two words of one
  // voice line must not let the ROM speak.
  CHECK_FALSE(table.ShouldPlay(kRom, 1249));

  // Past the hold window, the pack evidently had nothing for this command.
  CHECK(table.ShouldPlay(kRom, 1250));

  // And it ducks straight back under as soon as the pack speaks again.
  table.NoteMixed(kAltSound, true, 1300);
  CHECK_FALSE(table.ShouldPlay(kRom, 1300));
}

TEST_CASE("a newly published overrider gets its hold window before the ROM leaks")
{
  // At game start AltSound has produced nothing yet. Treating "never audible"
  // as "silent forever" would leak a burst of ROM audio while the pack loads.
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Fallback);
  table.SetFallbackHoldMs(250);
  table.SetSources(RomAndAltSound(), 5000);

  CHECK_FALSE(table.ShouldPlay(kRom, 5000));
  CHECK_FALSE(table.ShouldPlay(kRom, 5249));
  CHECK(table.ShouldPlay(kRom, 5250));
}

TEST_CASE("audibility survives a republished source list")
{
  // OnAudioSrcChanged fires whenever any plugin touches its sources. Losing the
  // history each time would restart the hold window and stutter the fallback.
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Fallback);
  table.SetFallbackHoldMs(250);
  table.SetSources(RomAndAltSound(), 0);
  table.NoteMixed(kAltSound, true, 1000);

  table.SetSources(RomAndAltSound(), 1100);
  CHECK_FALSE(table.ShouldPlay(kRom, 1100));
  CHECK(table.ShouldPlay(kRom, 1250));
}

TEST_CASE("a chain is decided from the top down")
{
  // Second pack overrides AltSound, which overrides the ROM. The ROM must stay
  // silent while the top of the chain is playing, even though its immediate
  // overrider is quiet.
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Fallback);
  table.SetFallbackHoldMs(250);
  table.SetSources({{kRom, 0, "PinMAME"}, {kAltSound, kRom, "AltSound"}, {kSecondPack, kAltSound, "Second pack"}}, 0);

  table.NoteMixed(kSecondPack, true, 1000);
  table.NoteMixed(kAltSound, false, 1000);

  CHECK_FALSE(table.ShouldPlay(kAltSound, 1000));
  CHECK_FALSE(table.ShouldPlay(kRom, 1000));

  // Only when the whole chain above it has gone quiet does the ROM return.
  CHECK(table.ShouldPlay(kRom, 1250));
}

TEST_CASE("lanes are ordered so an overrider is decided before what it overrides")
{
  AudioLanes::Table table;
  table.SetSources({{kSecondPack, kAltSound, "Second pack"}, {kRom, 0, "PinMAME"}, {kAltSound, kRom, "AltSound"}}, 0);

  const std::vector<AudioLanes::Lane>& lanes = table.Lanes();
  REQUIRE(lanes.size() == 3);
  CHECK(lanes[0].id == kSecondPack);
  CHECK(lanes[1].id == kAltSound);
  CHECK(lanes[2].id == kRom);
  CHECK(lanes[0].overridden == false);
  CHECK(lanes[1].overridden == true);
  CHECK(lanes[2].overridden == true);
}

TEST_CASE("a published override cycle terminates")
{
  // Two plugins each claiming to override the other is a configuration error,
  // not something to hang or recurse forever over.
  AudioLanes::Table table;
  table.SetMode(AudioLanes::OverrideMode::Fallback);
  table.SetSources({{kRom, kAltSound, "PinMAME"}, {kAltSound, kRom, "AltSound"}}, 0);

  CHECK_FALSE(table.ShouldPlay(kRom, 0));
  CHECK_FALSE(table.ShouldPlay(kAltSound, 0));
  CHECK(table.ShouldPlay(kRom, 100000));
}

TEST_CASE("a source overriding itself is ignored")
{
  AudioLanes::Table table;
  table.SetSources({{kRom, kRom, "PinMAME"}}, 0);
  CHECK(table.ShouldPlay(kRom, 0));
}
