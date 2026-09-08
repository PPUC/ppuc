#include "DmdSourceSelect.h"

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

int SelectMainDisplay(const std::vector<Candidate>& candidates)
{
  int best = -1;
  uint64_t bestArea = 0;

  for (size_t i = 0; i < candidates.size(); ++i)
  {
    const Candidate& candidate = candidates[i];
    if (!candidate.hasIdentifyFrame || candidate.width == 0 || candidate.height == 0 ||
        DepthForIdentifyFormat(candidate.identifyFormat) == 0)
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
