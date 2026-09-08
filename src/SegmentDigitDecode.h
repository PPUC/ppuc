#pragma once

#include <cstddef>
#include <cstdint>

// Turning a segment display's state into digits.
//
// The bit layout is unchanged from libpinmame's `coreGlobals.segments[].w`; what
// changed with the plugin API is the encoding. `SegSrcId::GetState` hands back
// sixteen relative luminances per element instead of one 16-bit mask, and when
// `coreGlobals.nAlphaSegs` is set those are PWM-integrated values rather than
// 0 or 1. So the lookup table below still applies -- it just has to be given a
// mask that was rebuilt from floats first.
namespace SegmentDigitDecode
{

// The frame carries sixteen floats per element whatever the element's real
// segment count, so this is a stride, not a count.
inline constexpr size_t kSegmentsPerElement = 16;

// A segment counts as lit above this. It matches the `> 0.5f` in vpinball's
// alphadmd plugin deliberately: that is what builds the identify frame Serum
// keys an alphanumeric colorization on, and a display whose digits PPUC and the
// colorizer disagree about would be worse than either choice alone.
inline constexpr float kSegmentOnThreshold = 0.5f;

// Once lit, a segment stays lit until it falls below this. The band exists
// because a modulated display's luminance is a duty cycle that wobbles: a
// single threshold sitting inside that wobble makes the score flicker.
//
// Untested against a game that actually sets `coreGlobals.nAlphaSegs` -- on
// System 6 the values are strictly 0.0 or 1.0 and the band never comes into
// play. If a modulated game renders blank, this pair is the first thing to
// suspect.
inline constexpr float kSegmentOffThreshold = 0.3f;

// Meaningful segments for a `SegElementType`, or 16 for anything unrecognised.
// Beyond that count the provider leaves the frame untouched, so those floats
// are stale rather than zero and must not reach the mask.
int SegmentCountForLayout(int elementType);

// The digit a 7-segment mask spells, or -1 for anything that is not a digit --
// a blank, a letter, or a partly-lit element caught mid-strobe. Bit 7 (the
// dot/comma) is ignored: it carries no numeric meaning here.
int DecodeDigit(uint16_t bitState);

// Rebuilds the mask for one element from the first `segmentCount` of its
// luminances, carrying `previous` through the hysteresis band. Pass 0 for the
// first sample.
uint16_t MaskFromLuminance(const float* luminances, int segmentCount, uint16_t previous);

}  // namespace SegmentDigitDecode
