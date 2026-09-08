// Tests for picking which published display is the machine's DMD.
//
// Under libpinmame's callback API this was a flag test -- take CORE_DMD, skip
// CORE_NODISP. CTLPI carries neither flag, and libpinmame publishes every
// CORE_DMD layout, so a host that just takes the first one renders a Stern SAM
// mini-display where the score should be. The rule here is a heuristic forced
// by that gap, which is exactly why it needs pinning down.

#include <vector>

#include "DmdSourceSelect.h"
#include "doctest.h"

namespace
{

using DmdSourceSelect::Candidate;

constexpr unsigned int kBitplane2 = 1u;
constexpr unsigned int kBitplane4 = 2u;

// A controller's own display: identify frames, overrides nothing.
Candidate Display(uint32_t resId, unsigned int width, unsigned int height, unsigned int format = kBitplane2,
                  bool hasIdentify = true)
{
  Candidate candidate;
  candidate.id = 0x300000000ull | resId;
  candidate.resId = resId;
  candidate.width = width;
  candidate.height = height;
  candidate.identifyFormat = format;
  candidate.hasIdentifyFrame = hasIdentify;
  return candidate;
}

// A colorizer's output: render frames only, overriding the display it colorized.
Candidate Colorized(uint32_t resId, unsigned int width, unsigned int height, uint64_t overrides,
                    unsigned int frameFormat = 3u /* SRGB565 */)
{
  Candidate candidate;
  candidate.id = 0x900000000ull | resId;
  candidate.overrideId = overrides;
  candidate.resId = resId;
  candidate.width = width;
  candidate.height = height;
  candidate.frameFormat = frameFormat;
  candidate.hasRenderFrame = true;
  return candidate;
}

}  // namespace

TEST_CASE("identify formats map to the depths DMDUtil takes")
{
  CHECK(DmdSourceSelect::DepthForIdentifyFormat(kBitplane2) == 2);
  CHECK(DmdSourceSelect::DepthForIdentifyFormat(kBitplane4) == 4);
  CHECK(DmdSourceSelect::DepthForIdentifyFormat(0) == 0);
  CHECK(DmdSourceSelect::DepthForIdentifyFormat(99) == 0);
}

TEST_CASE("no displays means nothing to render")
{
  // The normal case for the alphanumeric games PPUC mostly runs.
  CHECK(DmdSourceSelect::SelectMainDisplay({}) == -1);
}

TEST_CASE("a single DMD is chosen")
{
  const std::vector<Candidate> candidates = {Display(0, 128, 32)};
  CHECK(DmdSourceSelect::SelectMainDisplay(candidates) == 0);
}

TEST_CASE("the Stern SAM mini-displays lose to the score DMD")
{
  // sammini1_dmd128x32: the 128x32 comes first via DISP_SEG_IMPORT, then
  // fourteen 5x7 CORE_NODISP displays. Taking the first works here, but so does
  // taking the largest, and the second survives the import order changing.
  std::vector<Candidate> candidates = {Display(0, 128, 32)};
  for (uint32_t i = 1; i <= 14; ++i)
  {
    candidates.push_back(Display(i, 5, 7));
  }
  CHECK(DmdSourceSelect::SelectMainDisplay(candidates) == 0);

  // And with the order reversed, which is the case flag-free selection would
  // otherwise get wrong.
  std::vector<Candidate> reversed(candidates.rbegin(), candidates.rend());
  const int chosen = DmdSourceSelect::SelectMainDisplay(reversed);
  REQUIRE(chosen >= 0);
  CHECK(reversed[static_cast<size_t>(chosen)].resId == 0);
}

TEST_CASE("equal-sized displays fall back to publication order")
{
  const std::vector<Candidate> candidates = {Display(3, 128, 32), Display(1, 128, 32)};
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen >= 0);
  CHECK(candidates[static_cast<size_t>(chosen)].resId == 1);
}

TEST_CASE("a display with no identify frame is not renderable")
{
  // Render frames are LUM32F floats. Taking one instead would change the ROM's
  // look and break the keying Serum colorizations depend on, so a source that
  // offers only that is skipped rather than converted.
  const std::vector<Candidate> candidates = {Display(0, 128, 32, kBitplane2, false)};
  CHECK(DmdSourceSelect::SelectMainDisplay(candidates) == -1);
}

TEST_CASE("an identify format this build cannot render is skipped, not guessed at")
{
  const std::vector<Candidate> candidates = {Display(0, 256, 64, 99), Display(1, 128, 32)};
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen >= 0);
  CHECK(candidates[static_cast<size_t>(chosen)].resId == 1);
}

TEST_CASE("a zero-sized display is skipped")
{
  const std::vector<Candidate> candidates = {Display(0, 0, 32), Display(1, 128, 0), Display(2, 128, 32)};
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen >= 0);
  CHECK(candidates[static_cast<size_t>(chosen)].resId == 2);
}

TEST_CASE("a colorizer's output wins over the frame it colorized")
{
  // Same size, so nothing but the override chain can tell them apart. Taking
  // the raw frame would show an uncolorized DMD while the colorizer sat there
  // working for nothing.
  const Candidate raw = Display(0, 128, 32);
  const std::vector<Candidate> candidates = {raw, Colorized(1, 128, 32, raw.id)};

  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen == 1);
  CHECK(DmdSourceSelect::SourceFor(candidates[1]) == DmdSourceSelect::FrameSource::RenderRgb16);
}

TEST_CASE("a colorizer's 24 bit output is read as RGB24")
{
  const Candidate raw = Display(0, 128, 32);
  const std::vector<Candidate> candidates = {raw, Colorized(1, 128, 32, raw.id, 2u /* SRGB888 */)};
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen == 1);
  CHECK(DmdSourceSelect::SourceFor(candidates[1]) == DmdSourceSelect::FrameSource::RenderRgb24);
}

TEST_CASE("the raw display is read through its identify frame, never its render frame")
{
  // A controller's render frame is LUM32F, which would change every ROM's look
  // and break the keying a colorization depends on. Only an override -- which
  // is what a colorizer publishes -- may be read as colour.
  Candidate raw = Display(0, 128, 32);
  raw.hasRenderFrame = true;
  raw.frameFormat = 1u;  // LUM32F
  CHECK(DmdSourceSelect::SourceFor(raw) == DmdSourceSelect::FrameSource::Identify);
}

TEST_CASE("with no colorizer the controller's own display is still chosen")
{
  const std::vector<Candidate> candidates = {Display(0, 128, 32)};
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen == 0);
  CHECK(DmdSourceSelect::SourceFor(candidates[0]) == DmdSourceSelect::FrameSource::Identify);
}

TEST_CASE("a two step chain resolves to its far end")
{
  // alphadmd renders segments, serum colorizes that, upscaledmd scales the
  // result. Only the last one should be rendered.
  const Candidate rendered = Display(0, 128, 32);
  const Candidate colorized = Colorized(1, 128, 32, rendered.id);
  const Candidate upscaled = Colorized(2, 256, 64, colorized.id);

  const int chosen = DmdSourceSelect::SelectMainDisplay({rendered, colorized, upscaled});
  CHECK(chosen == 2);
}

TEST_CASE("the mini displays still lose once a colorizer is present")
{
  // The chain tail rule must not resurrect the NODISP displays: they are
  // un-overridden too, so size still has to decide between them.
  const Candidate raw = Display(0, 128, 32);
  std::vector<Candidate> candidates = {raw, Colorized(1, 128, 32, raw.id)};
  for (uint32_t i = 2; i <= 5; ++i)
  {
    candidates.push_back(Display(i, 5, 7));
  }
  const int chosen = DmdSourceSelect::SelectMainDisplay(candidates);
  REQUIRE(chosen >= 0);
  CHECK(candidates[static_cast<size_t>(chosen)].hasRenderFrame);
}
