#include "PinmameNvramMapLoader.h"

#include <SDL3/SDL.h>
#include <yaml-cpp/yaml.h>

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Only for the PINMAME_HARDWARE_GEN_* constants used to describe a generation
// and to match it against a map path. No libpinmame function is called from
// this translation unit.
#include "libpinmame.h"

static std::string NormalizeRomNameForMapLookup(const char* rom)
{
  if (rom == nullptr || rom[0] == '\0')
  {
    return "";
  }

  std::filesystem::path romPath(rom);
  std::string romName = romPath.filename().string();
  const std::string extension = romPath.extension().string();
  if (!extension.empty())
  {
    romName.resize(romName.size() - extension.size());
  }
  return romName;
}

static std::filesystem::path GetExecutableDirectory()
{
  const char* basePath = SDL_GetBasePath();
  if (basePath == nullptr || basePath[0] == '\0')
  {
    return std::filesystem::current_path();
  }

  std::filesystem::path path(basePath);
  SDL_free(const_cast<char*>(basePath));
  return path;
}

static std::filesystem::path GetPinmameBaseDirectory(const char* pinmamePath)
{
#if defined(_WIN32) || defined(_WIN64)
  if (pinmamePath != nullptr)
  {
    return std::filesystem::path(pinmamePath);
  }

  const char* homeDrive = getenv("HOMEDRIVE");
  const char* homePath = getenv("HOMEPATH");
  if (homeDrive != nullptr && homePath != nullptr)
  {
    return std::filesystem::path(std::string(homeDrive) + std::string(homePath)) / "pinmame";
  }
#else
  if (pinmamePath != nullptr)
  {
    return std::filesystem::path(pinmamePath);
  }

  const char* home = getenv("HOME");
  if (home != nullptr)
  {
    return std::filesystem::path(home) / ".pinmame";
  }
#endif

  return {};
}

static std::vector<std::filesystem::path> GetPinmameNvramMapsRootCandidates(const char* pinmamePath)
{
  const std::filesystem::path exeDir = GetExecutableDirectory();
  const std::filesystem::path cwd = std::filesystem::current_path();
  const std::filesystem::path pinmameDir = GetPinmameBaseDirectory(pinmamePath);

  std::vector<std::filesystem::path> candidates = {
      exeDir / "pinmame-nvram-maps",
      cwd / "pinmame-nvram-maps",
  };

  if (!pinmameDir.empty())
  {
    candidates.push_back(pinmameDir / "pinmame-nvram-maps");
  }

  return candidates;
}

static std::optional<std::filesystem::path> FindPinmameNvramMapsRoot(const char* pinmamePath)
{
  for (const auto& candidate : GetPinmameNvramMapsRootCandidates(pinmamePath))
  {
    if (std::filesystem::exists(candidate / "index.json") && std::filesystem::exists(candidate / "maps") &&
        std::filesystem::exists(candidate / "platforms"))
    {
      return candidate;
    }
  }

  return std::nullopt;
}

std::string DescribeHardwareGen(const uint64_t hardwareGen)
{
  struct HardwareGenLabel
  {
    PINMAME_HARDWARE_GEN bit;
    const char* name;
  };

  static constexpr HardwareGenLabel kLabels[] = {
      {PINMAME_HARDWARE_GEN_WPCALPHA_1, "WPCALPHA_1"},
      {PINMAME_HARDWARE_GEN_WPCALPHA_2, "WPCALPHA_2"},
      {PINMAME_HARDWARE_GEN_WPCDMD, "WPCDMD"},
      {PINMAME_HARDWARE_GEN_WPCFLIPTRON, "WPCFLIPTRON"},
      {PINMAME_HARDWARE_GEN_WPCDCS, "WPCDCS"},
      {PINMAME_HARDWARE_GEN_WPCSECURITY, "WPCSECURITY"},
      {PINMAME_HARDWARE_GEN_WPC95DCS, "WPC95DCS"},
      {PINMAME_HARDWARE_GEN_WPC95, "WPC95"},
      {PINMAME_HARDWARE_GEN_S11, "S11"},
      {PINMAME_HARDWARE_GEN_S11X, "S11X"},
      {PINMAME_HARDWARE_GEN_S11B2, "S11B2"},
      {PINMAME_HARDWARE_GEN_S11C, "S11C"},
      {PINMAME_HARDWARE_GEN_S9, "S9"},
      {PINMAME_HARDWARE_GEN_DE, "DE"},
      {PINMAME_HARDWARE_GEN_DEDMD16, "DEDMD16"},
      {PINMAME_HARDWARE_GEN_DEDMD32, "DEDMD32"},
      {PINMAME_HARDWARE_GEN_DEDMD64, "DEDMD64"},
      {PINMAME_HARDWARE_GEN_S7, "S7"},
      {PINMAME_HARDWARE_GEN_S6, "S6"},
      {PINMAME_HARDWARE_GEN_S4, "S4"},
      {PINMAME_HARDWARE_GEN_S3C, "S3C"},
      {PINMAME_HARDWARE_GEN_S3, "S3"},
      {PINMAME_HARDWARE_GEN_BY17, "BY17"},
      {PINMAME_HARDWARE_GEN_BY35, "BY35"},
      {PINMAME_HARDWARE_GEN_STMPU100, "STMPU100"},
      {PINMAME_HARDWARE_GEN_STMPU200, "STMPU200"},
      {PINMAME_HARDWARE_GEN_ASTRO, "ASTRO"},
      {PINMAME_HARDWARE_GEN_HNK, "HNK"},
      {PINMAME_HARDWARE_GEN_BYPROTO, "BYPROTO"},
      {PINMAME_HARDWARE_GEN_BY6803, "BY6803"},
      {PINMAME_HARDWARE_GEN_BY6803A, "BY6803A"},
      {PINMAME_HARDWARE_GEN_BOWLING, "BOWLING"},
      {PINMAME_HARDWARE_GEN_GTS1, "GTS1"},
      {PINMAME_HARDWARE_GEN_GTS80, "GTS80"},
      {PINMAME_HARDWARE_GEN_GTS80B, "GTS80B"},
      {PINMAME_HARDWARE_GEN_WS, "WS"},
      {PINMAME_HARDWARE_GEN_WS_1, "WS_1"},
      {PINMAME_HARDWARE_GEN_WS_2, "WS_2"},
      {PINMAME_HARDWARE_GEN_GTS3, "GTS3"},
      {PINMAME_HARDWARE_GEN_ZAC1, "ZAC1"},
      {PINMAME_HARDWARE_GEN_ZAC2, "ZAC2"},
      {PINMAME_HARDWARE_GEN_SAM, "SAM"},
      {PINMAME_HARDWARE_GEN_ALVG, "ALVG"},
      {PINMAME_HARDWARE_GEN_ALVG_DMD2, "ALVG_DMD2"},
      {PINMAME_HARDWARE_GEN_MRGAME, "MRGAME"},
      {PINMAME_HARDWARE_GEN_SLEIC, "SLEIC"},
      {PINMAME_HARDWARE_GEN_WICO, "WICO"},
      {PINMAME_HARDWARE_GEN_SPA, "SPA"},
  };

  std::ostringstream stream;
  stream << "0x" << std::hex << static_cast<uint64_t>(hardwareGen) << std::dec;

  bool first = true;
  for (const auto& label : kLabels)
  {
    if ((hardwareGen & label.bit) == 0)
    {
      continue;
    }

    stream << (first ? " (" : ", ");
    stream << label.name;
    first = false;
  }

  if (!first)
  {
    stream << ")";
  }

  return stream.str();
}

static bool TryParseMapUnsigned(const YAML::Node& node, uint32_t* pValue)
{
  if (!node || pValue == nullptr)
  {
    return false;
  }

  if (node.IsScalar())
  {
    const std::string text = node.as<std::string>();
    if (text.empty())
    {
      return false;
    }

    char* end = nullptr;
    const unsigned long value = strtoul(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0' || value > UINT32_MAX)
    {
      return false;
    }

    *pValue = static_cast<uint32_t>(value);
    return true;
  }

  return false;
}

static std::optional<PinmameMapNibble> TryParseMapNibble(const YAML::Node& node)
{
  if (!node || !node.IsScalar())
  {
    return std::nullopt;
  }

  const std::string nibble = node.as<std::string>();
  if (nibble == "both")
  {
    return PinmameMapNibble::BOTH;
  }
  if (nibble == "high")
  {
    return PinmameMapNibble::HIGH;
  }
  if (nibble == "low")
  {
    return PinmameMapNibble::LOW;
  }

  return std::nullopt;
}

static bool TryLoadPlatformNibbleDefaults(const std::filesystem::path& path,
                                          std::vector<PinmamePlatformMemoryRange>* pRanges, std::string* pError)
{
  if (pRanges == nullptr)
  {
    if (pError) *pError = "platform range output missing";
    return false;
  }

  YAML::Node root;
  try
  {
    root = YAML::LoadFile(path.string());
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = std::string("failed to parse platform file: ") + ex.what();
    return false;
  }

  const YAML::Node memoryLayout = root["memory_layout"];
  if (!memoryLayout || !memoryLayout.IsSequence())
  {
    if (pError) *pError = "platform file has no memory_layout sequence";
    return false;
  }

  pRanges->clear();
  for (const YAML::Node& entry : memoryLayout)
  {
    uint32_t address = 0;
    uint32_t size = 0;
    if (!TryParseMapUnsigned(entry["address"], &address) || !TryParseMapUnsigned(entry["size"], &size))
    {
      continue;
    }

    PinmamePlatformMemoryRange range;
    range.address = address;
    range.size = size;
    range.nibble = TryParseMapNibble(entry["nibble"]).value_or(PinmameMapNibble::BOTH);
    pRanges->push_back(range);
  }

  return true;
}

static std::optional<PinmameMapNibble> ResolvePlatformNibbleDefault(
    const std::vector<PinmamePlatformMemoryRange>& ranges, const uint32_t address)
{
  for (const auto& range : ranges)
  {
    if (address >= range.address && address < range.address + range.size)
    {
      return range.nibble;
    }
  }

  return std::nullopt;
}

static bool HardwareGenMatchesNvramMapPath(const uint64_t hardwareGen, const std::string& relativeMapPath)
{
  struct HardwareGenPathPrefix
  {
    PINMAME_HARDWARE_GEN bit;
    const char* prefix;
  };

  static constexpr HardwareGenPathPrefix kPrefixes[] = {
      {PINMAME_HARDWARE_GEN_S3, "maps/williams/system3/"},
      {PINMAME_HARDWARE_GEN_S3C, "maps/williams/system3/"},
      {PINMAME_HARDWARE_GEN_S4, "maps/williams/system4/"},
      {PINMAME_HARDWARE_GEN_S6, "maps/williams/system6/"},
      {PINMAME_HARDWARE_GEN_S7, "maps/williams/system7/"},
      {PINMAME_HARDWARE_GEN_S9, "maps/williams/system9/"},
      {PINMAME_HARDWARE_GEN_S11, "maps/williams/system11/"},
      {PINMAME_HARDWARE_GEN_S11X, "maps/williams/system11/"},
      {PINMAME_HARDWARE_GEN_S11B2, "maps/williams/system11/"},
      {PINMAME_HARDWARE_GEN_S11C, "maps/williams/system11/"},
      {PINMAME_HARDWARE_GEN_WPCALPHA_1, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPCALPHA_2, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPCDMD, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPCFLIPTRON, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPCDCS, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPCSECURITY, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPC95DCS, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_WPC95, "maps/williams/wpc/"},
      {PINMAME_HARDWARE_GEN_DE, "maps/dataeast/"},
      {PINMAME_HARDWARE_GEN_DEDMD16, "maps/dataeast/"},
      {PINMAME_HARDWARE_GEN_DEDMD32, "maps/dataeast/"},
      {PINMAME_HARDWARE_GEN_DEDMD64, "maps/dataeast/"},
      {PINMAME_HARDWARE_GEN_BY17, "maps/bally/as-2518-17/"},
      {PINMAME_HARDWARE_GEN_BY35, "maps/bally/as-2518-35/"},
      {PINMAME_HARDWARE_GEN_BY6803, "maps/bally/as-2518-133/"},
      {PINMAME_HARDWARE_GEN_BY6803A, "maps/bally/as-2518-133/"},
      {PINMAME_HARDWARE_GEN_STMPU100, "maps/stern/m100/"},
      {PINMAME_HARDWARE_GEN_STMPU200, "maps/stern/m200/"},
      {PINMAME_HARDWARE_GEN_WS, "maps/sega/whitestar/"},
      {PINMAME_HARDWARE_GEN_WS, "maps/stern/whitestar/"},
      {PINMAME_HARDWARE_GEN_WS_1, "maps/sega/whitestar/"},
      {PINMAME_HARDWARE_GEN_WS_1, "maps/stern/whitestar/"},
      {PINMAME_HARDWARE_GEN_WS_2, "maps/sega/whitestar/"},
      {PINMAME_HARDWARE_GEN_WS_2, "maps/stern/whitestar/"},
      {PINMAME_HARDWARE_GEN_SAM, "maps/stern/sam/"},
      {PINMAME_HARDWARE_GEN_GTS80, "maps/gottlieb/system80/"},
      {PINMAME_HARDWARE_GEN_GTS80B, "maps/gottlieb/system80b/"},
      {PINMAME_HARDWARE_GEN_GTS3, "maps/gottlieb/system3/"},
  };

  bool matchedKnownHardware = false;
  for (const auto& prefix : kPrefixes)
  {
    if ((hardwareGen & prefix.bit) == 0)
    {
      continue;
    }

    matchedKnownHardware = true;
    if (relativeMapPath.rfind(prefix.prefix, 0) == 0)
    {
      return true;
    }
  }

  return !matchedKnownHardware;
}

static bool TryLoadTrackedFieldFromMap(const YAML::Node& fieldNode,
                                       const std::vector<PinmamePlatformMemoryRange>& platformRanges,
                                       PinmameTrackedField* pField, std::string* pError)
{
  if (pField == nullptr)
  {
    return false;
  }

  pField->available = false;
  if (!fieldNode || !fieldNode.IsMap())
  {
    return true;
  }

  uint32_t address = 0;
  if (!TryParseMapUnsigned(fieldNode["start"], &address))
  {
    if (pError) *pError = "tracked field is missing a valid start address";
    return false;
  }

  const std::string encodingText = fieldNode["encoding"] ? fieldNode["encoding"].as<std::string>() : "";
  PinmameMapEncoding encoding;
  if (encodingText == "int")
  {
    encoding = PinmameMapEncoding::INT;
  }
  else if (encodingText == "bcd")
  {
    encoding = PinmameMapEncoding::BCD;
  }
  else
  {
    if (pError) *pError = "tracked field uses unsupported encoding: " + encodingText;
    return false;
  }

  uint32_t mask = 0xFF;
  if (fieldNode["mask"] && !TryParseMapUnsigned(fieldNode["mask"], &mask))
  {
    if (pError) *pError = "tracked field has an invalid mask";
    return false;
  }

  PinmameMapNibble nibble = ResolvePlatformNibbleDefault(platformRanges, address).value_or(PinmameMapNibble::BOTH);
  if (fieldNode["nibble"])
  {
    const auto parsedNibble = TryParseMapNibble(fieldNode["nibble"]);
    if (!parsedNibble.has_value())
    {
      if (pError) *pError = "tracked field has an invalid nibble setting";
      return false;
    }
    nibble = parsedNibble.value();
  }

  int offset = 0;
  if (fieldNode["offset"])
  {
    offset = fieldNode["offset"].as<int>();
  }

  bool treatZeroAsUnavailable = false;
  const YAML::Node specialValues = fieldNode["special_values"];
  if (specialValues && specialValues.IsMap())
  {
    const YAML::Node zeroNode = specialValues["0"];
    treatZeroAsUnavailable = zeroNode && zeroNode.IsScalar();
  }

  pField->available = true;
  pField->address = address;
  pField->encoding = encoding;
  pField->nibble = nibble;
  pField->mask = static_cast<uint8_t>(mask & 0xFF);
  pField->offset = offset;
  pField->treatZeroAsUnavailable = treatZeroAsUnavailable;
  return true;
}

bool TryLoadPinmameTrackingConfig(const char* rom, const uint64_t hardwareGen, const char* pinmamePath,
                                  PinmameTrackingConfig* pConfig, std::string* pError)
{
  if (pConfig == nullptr)
  {
    if (pError) *pError = "tracking config output missing";
    return false;
  }

  pConfig->loaded = false;
  pConfig->mapPath.clear();
  pConfig->currentPlayer = PinmameTrackedField{};
  pConfig->currentBall = PinmameTrackedField{};

  const std::string romName = NormalizeRomNameForMapLookup(rom);
  if (romName.empty())
  {
    if (pError) *pError = "ROM name is empty";
    return false;
  }

  const auto mapsRoot = FindPinmameNvramMapsRoot(pinmamePath);
  if (!mapsRoot.has_value())
  {
    if (pError) *pError = "pinmame-nvram-maps assets not found";
    return false;
  }

  YAML::Node indexRoot;
  try
  {
    indexRoot = YAML::LoadFile((mapsRoot.value() / "index.json").string());
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = std::string("failed to parse index.json: ") + ex.what();
    return false;
  }

  const YAML::Node mapPathNode = indexRoot[romName];
  if (!mapPathNode || !mapPathNode.IsScalar())
  {
    if (pError) *pError = "no nvram map found for ROM " + romName;
    return false;
  }

  const std::string relativeMapPath = mapPathNode.as<std::string>();
  if (!HardwareGenMatchesNvramMapPath(hardwareGen, relativeMapPath))
  {
    if (pError) *pError = "nvram map path does not match reported hardware generation: " + relativeMapPath;
    return false;
  }

  const std::filesystem::path mapPath = mapsRoot.value() / relativeMapPath;
  YAML::Node mapRoot;
  try
  {
    mapRoot = YAML::LoadFile(mapPath.string());
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = std::string("failed to parse map file: ") + ex.what();
    return false;
  }

  const YAML::Node metadata = mapRoot["_metadata"];
  const YAML::Node platformNode = metadata["platform"];
  if (!platformNode || !platformNode.IsScalar())
  {
    if (pError) *pError = "map file is missing _metadata.platform";
    return false;
  }

  std::vector<PinmamePlatformMemoryRange> platformRanges;
  const std::filesystem::path platformPath =
      mapsRoot.value() / "platforms" / (platformNode.as<std::string>() + ".json");
  if (!TryLoadPlatformNibbleDefaults(platformPath, &platformRanges, pError))
  {
    return false;
  }

  const YAML::Node gameState = mapRoot["game_state"];
  if (!gameState || !gameState.IsMap())
  {
    if (pError) *pError = "map file is missing game_state";
    return false;
  }

  if (!TryLoadTrackedFieldFromMap(gameState["current_player"], platformRanges, &pConfig->currentPlayer, pError))
  {
    return false;
  }
  if (!TryLoadTrackedFieldFromMap(gameState["current_ball"], platformRanges, &pConfig->currentBall, pError))
  {
    return false;
  }

  if (!pConfig->currentPlayer.available && !pConfig->currentBall.available)
  {
    if (pError) *pError = "map file does not define current_player or current_ball";
    return false;
  }

  pConfig->loaded = true;
  pConfig->mapPath = relativeMapPath;
  return true;
}
