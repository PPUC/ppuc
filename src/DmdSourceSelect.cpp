#include "DmdSourceSelect.h"

#include <algorithm>

namespace DmdSourceSelect
{

int DepthForIdentifyFormat(unsigned int identifyFormat)
{
  // CTLPI_DISPLAY_ID_FORMAT_BITPLANE2 / _BITPLANE4. Mirrored rather than
  // included so this stays free of the plugin SDK; PluginEngine.cpp asserts the
  // two agree.
  switch (identifyFormat)
  {
    case 1u:
      return 2;
    case 2u:
      return 4;
    default:
      return 0;
  }
}

FrameSource SourceFor(const Candidate& candidate)
{
  // CTLPI_DISPLAY_FORMAT_SRGB888 / _SRGB565, mirrored to keep this free of the
  // plugin SDK; PluginEngine.cpp asserts the values agree.
  if (candidate.hasRenderFrame && candidate.overrideId != 0)
  {
    if (candidate.frameFormat == 3u) return FrameSource::RenderRgb16;
    if (candidate.frameFormat == 2u) return FrameSource::RenderRgb24;
  }
  if (candidate.hasIdentifyFrame && DepthForIdentifyFormat(candidate.identifyFormat) != 0)
  {
    return FrameSource::Identify;
  }
  return FrameSource::None;
}

namespace
{

// Some other published display replaces this one.
bool IsOverridden(const Candidate& candidate, const std::vector<Candidate>& all)
{
  if (candidate.id == 0)
  {
    return false;
  }
  return std::any_of(all.begin(), all.end(),
                     [&candidate](const Candidate& other) { return other.overrideId == candidate.id; });
}

bool IsRenderable(const Candidate& candidate)
{
  return candidate.width != 0 && candidate.height != 0 && SourceFor(candidate) != FrameSource::None;
}

}  // namespace

int SelectMainDisplay(const std::vector<Candidate>& candidates)
{
  // When anything sits at the end of a chain, only chain tails are considered.
  // A colorizer's output and the frame it colorized are the same size, so size
  // cannot tell them apart; being un-overridden can.
  const bool anyTail = std::any_of(candidates.begin(), candidates.end(), [&candidates](const Candidate& candidate)
                                   { return IsRenderable(candidate) && !IsOverridden(candidate, candidates); });

  int best = -1;
  uint64_t bestArea = 0;
  for (size_t i = 0; i < candidates.size(); ++i)
  {
    const Candidate& candidate = candidates[i];
    if (!IsRenderable(candidate) || (anyTail && IsOverridden(candidate, candidates)))
    {
      continue;
    }

    const uint64_t area = static_cast<uint64_t>(candidate.width) * candidate.height;
    if (best == -1 || area > bestArea ||
        (area == bestArea && candidate.resId < candidates[static_cast<size_t>(best)].resId))
    {
      best = static_cast<int>(i);
      bestArea = area;
    }
  }

  return best;
}

}  // namespace DmdSourceSelect
