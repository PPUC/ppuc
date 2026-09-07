#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Which audio sources on the plugin bus may be heard, and when.
//
// CTLPI lets one audio source declare that it overrides another: AltSound
// publishes a source whose overrideId names PinMAME's ROM stream, meaning "play
// me instead". Both streams keep arriving; deciding what reaches the speakers is
// the host's job, and this is that decision, separated from SDL and from the
// sample math so it can be tested.
//
// Two modes, and the second is the reason this exists at all:
//
//   Replace  - an overridden source is silent for as long as it is overridden.
//              This is what PinMAME used to do internally via
//              PinmameSetSoundMode(PINMAME_SOUND_MODE_ALTSOUND), which silenced
//              the ROM stream inside the emulator. All or nothing.
//
//   Fallback - an overridden source is heard while its overrider has produced
//              nothing audible for kFallbackHoldMs. An AltSound pack with no
//              sample for a given command then falls back to the ROM sound
//              rather than to silence. This was impossible before PinMAME
//              became a plugin, because ROM audio never reached the bus as a
//              stream anything could mix against.
//
// Fallback is a heuristic and is documented as one: it infers "this pack has
// nothing for that command" from silence, so a pack with long near-silent
// ambience will leak ROM audio under it, and there is up to kFallbackHoldMs of
// delay before ROM sound returns. An explicit "not handling this command"
// signal from the overrider would be exact; the bus has no such message today.
namespace AudioLanes
{

enum class OverrideMode
{
  Replace,
  Fallback,
};

// Long enough that the gaps between words in one AltSound voice line do not
// let the ROM speak underneath, short enough that a command the pack ignores
// does not feel like a dropout.
inline constexpr uint64_t kFallbackHoldMs = 250;

// One published audio source, reduced to what the decision needs. Ids are the
// uint64 form of CtlResId, so 0 reliably means "none".
struct Source
{
  uint64_t id = 0;
  uint64_t overrideId = 0;
  std::string name;
};

// A source plus what has been observed about it.
struct Lane
{
  uint64_t id = 0;
  uint64_t overrideId = 0;
  std::string name;
  // Some other source declares that it overrides this one.
  bool overridden = false;
  // How far this lane is from the head of its override chain. Lanes are kept
  // sorted by it, so an overrider is always decided before what it overrides.
  unsigned int depth = 0;
  // Host clock, not sample position: the mixer is asked to decide once per
  // device callback and the two agree closely enough for a 250 ms window.
  uint64_t lastAudibleMs = 0;
};

class Table
{
 public:
  void SetMode(OverrideMode mode) { mode_ = mode; }
  OverrideMode Mode() const { return mode_; }
  void SetFallbackHoldMs(uint64_t holdMs) { fallbackHoldMs_ = holdMs; }
  uint64_t FallbackHoldMs() const { return fallbackHoldMs_; }

  // Replaces the published topology. Lanes that survive keep their audibility
  // history; new ones start as if they had just been audible, so a fresh
  // overrider is given its hold window before the lane under it is let through.
  // Without that, every game start would leak a burst of ROM audio while
  // AltSound is still loading its pack.
  void SetSources(const std::vector<Source>& sources, uint64_t nowMs);

  const std::vector<Lane>& Lanes() const { return lanes_; }

  // Whether this source appears in the published topology at all.
  bool Knows(uint64_t sourceId) const { return Find(sourceId) != nullptr; }

  // Whether this lane's samples should reach the mix right now. Unknown ids
  // play: a stream can arrive before the source list that describes it, and
  // dropping audio because of that ordering would be worse than a brief leak.
  bool ShouldPlay(uint64_t sourceId, uint64_t nowMs) const;

  // Records what mixing this lane produced. Must be called for every lane every
  // block, including muted ones -- a muted lane is drained, not stalled, and its
  // audibility is what an outer chain link would key on.
  void NoteMixed(uint64_t sourceId, bool audible, uint64_t nowMs);

 private:
  const Lane* Find(uint64_t sourceId) const;
  // The lane that declares it overrides `sourceId`, if any.
  const Lane* FindOverriderOf(uint64_t sourceId) const;

  std::vector<Lane> lanes_;
  OverrideMode mode_ = OverrideMode::Replace;
  uint64_t fallbackHoldMs_ = kFallbackHoldMs;
};

}  // namespace AudioLanes
