#include "PluginDir.h"

#include <filesystem>
#include <vector>

#include "SDL3/SDL.h"

std::string ResolvePluginDir(const char* configured)
{
  if (configured != nullptr && configured[0] != '\0')
  {
    return configured;
  }

  std::vector<std::filesystem::path> candidates;
  if (const char* basePath = SDL_GetBasePath(); basePath != nullptr && basePath[0] != '\0')
  {
    const std::filesystem::path executableDir(basePath);
    candidates.push_back(executableDir / "plugins");
    candidates.push_back(executableDir / "ppuc" / "plugins");
  }
  candidates.emplace_back("ppuc/plugins");
  candidates.emplace_back("plugins");
  candidates.emplace_back("/usr/lib/ppuc/plugins");
  candidates.emplace_back("../vpinball/plugins");

  for (const auto& candidate : candidates)
  {
    if (std::filesystem::exists(candidate))
    {
      return candidate.string();
    }
  }
  // None exists. Name the first one, so the error says where they belong.
  return candidates.front().string();
}
