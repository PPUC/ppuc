#pragma once

#include <cstdint>
#include <vector>

// Choosing which published display is the machine's DMD.
//
// Under libpinmame's callback API this was decided by flags on the layout:
// take CORE_DMD, skip CORE_NODISP. CTLPI carries neither -- `DisplaySrcId` has
// no "this is the main display" hint and no way to say a display is internal
// state rather than something to show. libpinmame publishes every CORE_DMD
// layout including the NODISP ones, so a host that takes the first, or all of
// them, renders a Stern SAM mini-display instead of the score.
//
// The rule below is a heuristic forced by that gap, not a reading of the spec.
// It is safe on the case that motivates it because both plausible rules agree
// there: in `sam.c` the 128x32 DMD is imported first *and* the NODISP extras
// are 5x7. Ordering alone would be enough, and size alone would be enough; using
// size with ordering as the tie-break means either convention changing upstream
// still lands on the right display.
//
// The principled fix is `DisplaySinkId` plus a NODISP hint -- see
// docs/PLUGIN_MIGRATION.md.
namespace DmdSourceSelect
{

// One published display, reduced to what the choice needs.
struct Candidate
{
  uint32_t resId = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int identifyFormat = 0;
  bool hasIdentifyFrame = false;
};

// Index into `candidates` of the display to render, or -1 when none qualifies.
int SelectMainDisplay(const std::vector<Candidate>& candidates);

// Bits per pixel for a CTLPI identify format, or 0 if PPUC cannot render it.
//
// Identify frames rather than render frames, and not only for convenience: the
// identify frame is one byte per pixel of raw indexed data, which is byte for
// byte what DMDUtil::UpdateData already took from libpinmame. The render frame
// is LUM32F floats, which would change every ROM's look and break the keying
// Serum colorizations depend on.
int DepthForIdentifyFormat(unsigned int identifyFormat);

}  // namespace DmdSourceSelect
