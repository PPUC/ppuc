#include "MediaPluginHost.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/MsgPluginManager.h"
#include "pup/PUPPlugin.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/VPXPlugin.h"

namespace
{
constexpr uint32_t kHostEndpointId = 1;
constexpr int kDefaultBackglassWidth = 1920;
constexpr int kDefaultBackglassHeight = 1080;
constexpr float kPi = 3.14159265358979323846f;

uint8_t FloatToByte(float value);

struct SegmentPoint
{
  float x;
  float y;
};

SDL_FColor ScaleColorAlpha(SDL_FColor color, float alphaScale)
{
  color.a = std::clamp(color.a * alphaScale, 0.0f, 1.0f);
  return color;
}

SegmentPoint SkewSegmentPoint(const SDL_FRect& rect, SegmentPoint point,
                              float skew)
{
  const float centerY = rect.y + rect.h * 0.5f;
  point.x += (point.y - centerY) * skew;
  return point;
}

SDL_FColor SegmentColor(VPXSegDisplayRenderStyle style, float tintR,
                        float tintG, float tintB, float luminance,
                        float brightness, float alpha)
{
  float baseR = 1.0f;
  float baseG = 0.35f;
  float baseB = 0.12f;
  switch (style)
  {
    case VPXSegStyle_BlueVFD:
      baseR = 0.35f;
      baseG = 0.75f;
      baseB = 1.0f;
      break;
    case VPXSegStyle_GreenVFD:
    case VPXSegStyle_GreenLED:
      baseR = 0.35f;
      baseG = 1.0f;
      baseB = 0.45f;
      break;
    case VPXSegStyle_RedLED:
      baseR = 1.0f;
      baseG = 0.12f;
      baseB = 0.08f;
      break;
    case VPXSegStyle_YellowLED:
      baseR = 1.0f;
      baseG = 0.85f;
      baseB = 0.16f;
      break;
    case VPXSegStyle_Plasma:
    case VPXSegStyle_GenPlasma:
    case VPXSegStyle_GenLED:
    default:
      break;
  }

  const float intensity = std::clamp(luminance * brightness, 0.0f, 1.0f);
  return SDL_FColor{std::clamp(baseR * tintR * intensity, 0.0f, 1.0f),
                    std::clamp(baseG * tintG * intensity, 0.0f, 1.0f),
                    std::clamp(baseB * tintB * intensity, 0.0f, 1.0f),
                    std::clamp(alpha, 0.0f, 1.0f)};
}

SDL_FRect ScaleSourceRectToOutput(const VPXRenderContext2D* ctx, float srcX,
                                  float srcY, float srcW, float srcH)
{
  const float srcWidth = ctx != nullptr && ctx->srcWidth > 0.0f
                             ? ctx->srcWidth
                             : static_cast<float>(kDefaultBackglassWidth);
  const float srcHeight = ctx != nullptr && ctx->srcHeight > 0.0f
                              ? ctx->srcHeight
                              : static_cast<float>(kDefaultBackglassHeight);
  const float outWidth = ctx != nullptr && ctx->outWidth > 0.0f
                             ? ctx->outWidth
                             : srcWidth;
  const float outHeight = ctx != nullptr && ctx->outHeight > 0.0f
                              ? ctx->outHeight
                              : srcHeight;
  const float scaleX = outWidth / srcWidth;
  const float scaleY = outHeight / srcHeight;
  return SDL_FRect{srcX * scaleX, srcY * scaleY, srcW * scaleX,
                   srcH * scaleY};
}

SDL_FRect ScaleSourceRectToOutputBottomOrigin(const VPXRenderContext2D* ctx,
                                              float srcX, float srcY,
                                              float srcW, float srcH)
{
  const float srcHeight = ctx != nullptr && ctx->srcHeight > 0.0f
                              ? ctx->srcHeight
                              : static_cast<float>(kDefaultBackglassHeight);
  return ScaleSourceRectToOutput(ctx, srcX, srcHeight - srcY - srcH, srcW,
                                 srcH);
}

void DrawFilledPolygon(SDL_Renderer* renderer,
                       const std::vector<SegmentPoint>& points,
                       const SDL_FColor& color)
{
  if (points.size() < 3)
  {
    return;
  }

  std::vector<SDL_Vertex> vertices;
  vertices.reserve(points.size());
  for (const SegmentPoint& point : points)
  {
    vertices.push_back(SDL_Vertex{SDL_FPoint{point.x, point.y}, color,
                                  SDL_FPoint{0.0f, 0.0f}});
  }

  std::vector<int> indices;
  indices.reserve((points.size() - 2) * 3);
  for (int i = 1; i < static_cast<int>(points.size()) - 1; ++i)
  {
    indices.push_back(0);
    indices.push_back(i);
    indices.push_back(i + 1);
  }

  SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                     static_cast<int>(vertices.size()), indices.data(),
                     static_cast<int>(indices.size()));
}

void DrawBeveledSegment(SDL_Renderer* renderer, float x1, float y1, float x2,
                        float y2, float thickness, const SDL_FColor& color,
                        const SDL_FRect& rect, float skew)
{
  const float dx = x2 - x1;
  const float dy = y2 - y1;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.01f)
  {
    return;
  }

  const float ux = dx / length;
  const float uy = dy / length;
  const float px = -uy;
  const float py = ux;
  const float half = thickness * 0.5f;
  const float cap = std::min(thickness * 0.85f, length * 0.28f);

  std::vector<SegmentPoint> points{
      {x1, y1},
      {x1 + ux * cap + px * half, y1 + uy * cap + py * half},
      {x2 - ux * cap + px * half, y2 - uy * cap + py * half},
      {x2, y2},
      {x2 - ux * cap - px * half, y2 - uy * cap - py * half},
      {x1 + ux * cap - px * half, y1 + uy * cap - py * half}};
  for (SegmentPoint& point : points)
  {
    point = SkewSegmentPoint(rect, point, skew);
  }
  DrawFilledPolygon(renderer, points, color);
}

void DrawSegmentDot(SDL_Renderer* renderer, float cx, float cy, float radius,
                    const SDL_FColor& color, const SDL_FRect& rect, float skew)
{
  std::vector<SegmentPoint> points;
  points.reserve(12);
  for (int i = 0; i < 12; ++i)
  {
    const float angle = (static_cast<float>(i) / 12.0f) * 2.0f * kPi;
    points.push_back(SkewSegmentPoint(
        rect,
        SegmentPoint{cx + std::cos(angle) * radius,
                     cy + std::sin(angle) * radius},
        skew));
  }
  DrawFilledPolygon(renderer, points, color);
}

void DrawSegmentByIndex(SDL_Renderer* renderer, int index, const SDL_FRect& rect,
                        float thickness, const SDL_FColor& color, float skew)
{
  const float left = rect.x + rect.w * 0.12f;
  const float right = rect.x + rect.w * 0.88f;
  const float top = rect.y + rect.h * 0.10f;
  const float middle = rect.y + rect.h * 0.50f;
  const float bottom = rect.y + rect.h * 0.90f;
  const float upper = rect.y + rect.h * 0.27f;
  const float lower = rect.y + rect.h * 0.73f;
  const float center = rect.x + rect.w * 0.50f;
  const float dotSize = std::max(2.0f, thickness * 1.5f);

  switch (index)
  {
    case 0:
      DrawBeveledSegment(renderer, left, top, right, top, thickness, color,
                         rect, skew);
      break;
    case 1:
      DrawBeveledSegment(renderer, right, top, right, middle, thickness, color,
                         rect, skew);
      break;
    case 2:
      DrawBeveledSegment(renderer, right, middle, right, bottom, thickness,
                         color, rect, skew);
      break;
    case 3:
      DrawBeveledSegment(renderer, left, bottom, right, bottom, thickness,
                         color, rect, skew);
      break;
    case 4:
      DrawBeveledSegment(renderer, left, middle, left, bottom, thickness,
                         color, rect, skew);
      break;
    case 5:
      DrawBeveledSegment(renderer, left, top, left, middle, thickness, color,
                         rect, skew);
      break;
    case 6:
      DrawBeveledSegment(renderer, left, middle, right, middle, thickness,
                         color, rect, skew);
      break;
    case 7:
      DrawSegmentDot(renderer, right + dotSize * 0.9f, bottom - dotSize * 0.5f,
                     dotSize * 0.5f, color, rect, skew);
      break;
    case 8:
      DrawBeveledSegment(renderer, left, top, center, middle, thickness, color,
                         rect, skew);
      break;
    case 9:
      DrawBeveledSegment(renderer, right, top, center, middle, thickness,
                         color, rect, skew);
      break;
    case 10:
      DrawBeveledSegment(renderer, left, bottom, center, middle, thickness,
                         color, rect, skew);
      break;
    case 11:
      DrawBeveledSegment(renderer, right, bottom, center, middle, thickness,
                         color, rect, skew);
      break;
    case 12:
      DrawBeveledSegment(renderer, center, top, center, upper, thickness,
                         color, rect, skew);
      break;
    case 13:
      DrawBeveledSegment(renderer, center, lower, center, bottom, thickness,
                         color, rect, skew);
      break;
    case 14:
      DrawBeveledSegment(renderer, left, upper, center, upper, thickness,
                         color, rect, skew);
      break;
    case 15:
      DrawBeveledSegment(renderer, center, lower, right, lower, thickness,
                         color, rect, skew);
      break;
    default:
      break;
  }
}

struct B2SSegmentDigitMsg
{
  int digit;
  int value;
};

struct B2SPlayerScoreMsg
{
  int player;
  int score;
};

struct HostTexture
{
  std::mutex mutex;
  int width = 0;
  int height = 0;
  VPXTextureFormat format = VPXTEXFMT_sRGBA8;
  std::vector<uint8_t> pixels;
  SDL_Texture* sdlTexture = nullptr;
  SDL_Renderer* sdlRenderer = nullptr;
  int sdlTextureWidth = 0;
  int sdlTextureHeight = 0;
  VPXTextureFormat sdlTextureFormat = VPXTEXFMT_sRGBA8;
  bool dirty = false;
};

int BytesPerPixel(VPXTextureFormat format)
{
  switch (format)
  {
    case VPXTEXFMT_sRGB565:
      return 2;
    case VPXTEXFMT_BW32F:
    case VPXTEXFMT_sRGB8:
      return 3;
    case VPXTEXFMT_sRGBA8:
    default:
      return 4;
  }
}

SDL_PixelFormat SdlFormat(VPXTextureFormat format)
{
  switch (format)
  {
    case VPXTEXFMT_sRGB565:
      return SDL_PIXELFORMAT_RGB565;
    case VPXTEXFMT_sRGB8:
      return SDL_PIXELFORMAT_RGB24;
    case VPXTEXFMT_BW32F:
    case VPXTEXFMT_sRGBA8:
    default:
      return SDL_PIXELFORMAT_RGBA32;
  }
}

uint8_t FloatToByte(float value)
{
  const float clamped = std::max(0.0f, std::min(1.0f, value));
  return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

int16_t ClampInt16(float value)
{
  if (value > static_cast<float>(std::numeric_limits<int16_t>::max()))
  {
    return std::numeric_limits<int16_t>::max();
  }
  if (value < static_cast<float>(std::numeric_limits<int16_t>::min()))
  {
    return std::numeric_limits<int16_t>::min();
  }
  return static_cast<int16_t>(std::lround(value));
}

void ConvertAudioUpdateToS16(const AudioUpdateMsg& msg,
                             std::vector<int16_t>* samples)
{
  samples->clear();
  if (msg.buffer == nullptr || msg.bufferSize == 0)
  {
    return;
  }

  const float volume = std::max(0.0f, msg.volume);
  if (msg.format == CTLPI_AUDIO_FORMAT_SAMPLE_FLOAT)
  {
    const size_t count = msg.bufferSize / sizeof(float);
    const auto* input = reinterpret_cast<const float*>(msg.buffer);
    samples->resize(count);
    for (size_t i = 0; i < count; ++i)
    {
      (*samples)[i] = ClampInt16(input[i] * volume * 32767.0f);
    }
    return;
  }

  if (msg.format == CTLPI_AUDIO_FORMAT_SAMPLE_INT16)
  {
    const size_t count = msg.bufferSize / sizeof(int16_t);
    const auto* input = reinterpret_cast<const int16_t*>(msg.buffer);
    samples->resize(count);
    for (size_t i = 0; i < count; ++i)
    {
      (*samples)[i] = ClampInt16(static_cast<float>(input[i]) * volume);
    }
  }
}

class SDLModuleLoader final : public MsgPI::MsgModuleLoader
{
public:
  void* Link(const std::string& directory, const std::string& file) override
  {
#if defined(_WIN32) || defined(__MINGW32__)
    SetDllDirectory(directory.c_str());
#else
    (void)directory;
#endif
    void* module = SDL_LoadObject(file.c_str());
#if defined(_WIN32) || defined(__MINGW32__)
    SetDllDirectory(nullptr);
#endif
    return module;
  }

  void Unlink(void* dynamicModule) override
  {
    SDL_UnloadObject(static_cast<SDL_SharedObject*>(dynamicModule));
  }

  void* GetFunction(void* dynamicModule,
                    const std::string& functionName) override
  {
    return reinterpret_cast<void*>(
        SDL_LoadFunction(static_cast<SDL_SharedObject*>(dynamicModule),
                         functionName.c_str()));
  }
};
}  // namespace

class MediaPluginHost::Impl
{
public:
  explicit Impl(AudioOutput* audioOutput) : audioOutput_(audioOutput) {}
  ~Impl() { Shutdown(); }

  bool Initialize(const Options& options, std::string* errorMessage);
  void Shutdown();
  void SetGameInfo(const char* gameId, uint64_t hardwareGen);
  void OnGameStart();
  void OnGameEnd();
  void QueueEvent(char source, int id, int value);
  void QueueSegmentDisplay(int digit, int value);
  void QueuePlayerScore(int player, int score);
  void QueueDmdTrigger(uint16_t id);
  void OnSoundCommand(int boardNo, int cmd);
  void Process();

private:
  static void HostPluginLoad(uint32_t, const MsgPluginAPI*) {}
  static void HostPluginUnload() {}

  static void MSGPIAPI OnGetLoggingApi(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetScriptApi(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetVpxApi(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetAudioSrc(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnAudioUpdate(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetBackglassRenderer(unsigned int, void*,
                                              void* msgData);

  static void MSGPIAPI Log(const char* source, const char* func, int line,
                           unsigned int level, const char* message);

  static void MSGPIAPI RegisterScriptClass(ScriptClassDef* classDef);
  static void MSGPIAPI RegisterScriptTypeAlias(const char*, const char*) {}
  static void MSGPIAPI RegisterScriptArrayType(ScriptArrayDef*) {}
  static void MSGPIAPI SubmitTypeLibrary(unsigned int) {}
  static void MSGPIAPI UnregisterScriptClass(ScriptClassDef* classDef);
  static void MSGPIAPI UnregisterScriptTypeAlias(const char*) {}
  static void MSGPIAPI UnregisterScriptArrayType(ScriptArrayDef*) {}
  static void MSGPIAPI OnScriptError(unsigned int type, const char* message);
  static void MSGPIAPI SetCOMObjectOverride(const char* classname,
                                            const ScriptClassDef* classDef);
  static ScriptClassDef* MSGPIAPI GetClassDef(const char* typeName);

  static void MSGPIAPI GetVpxInfo(VPXInfo* info);
  static void MSGPIAPI GetTableInfo(VPXTableInfo* info);
  static unsigned int MSGPIAPI PushNotification(const char*, int) { return 0; }
  static void MSGPIAPI UpdateNotification(unsigned int, const char*, int) {}
  static void MSGPIAPI DisableStaticPrerendering(int) {}
  static void MSGPIAPI GetActiveViewSetup(VPXViewSetupDef*) {}
  static void MSGPIAPI SetActiveViewSetup(VPXViewSetupDef*) {}
  static void MSGPIAPI GetInputState(VPXInputState*) {}
  static void MSGPIAPI SetInputState(VPXInputState*) {}
  static double MSGPIAPI GetGameTime();
  static VPXTexture MSGPIAPI CreateTexture(uint8_t* rawData, int size);
  static void MSGPIAPI UpdateTexture(VPXTexture* texture, int width, int height,
                                     VPXTextureFormat format,
                                     const void* image);
  static VPXTextureInfo* MSGPIAPI GetTextureInfo(VPXTexture texture);
  static void MSGPIAPI DeleteTexture(VPXTexture texture);

  static int MSGPIAPI RenderBackglass(VPXRenderContext2D* renderCtx,
                                      void* context);
  static void MSGPIAPI DrawImage(VPXRenderContext2D* ctx, VPXTexture texture,
                                 float tintR, float tintG, float tintB,
                                 float alpha, float texX, float texY,
                                 float texW, float texH, float pivotX,
                                 float pivotY, float rotation, float srcX,
                                 float srcY, float srcW, float srcH);
  static void MSGPIAPI DrawDisplay(VPXRenderContext2D*, VPXDisplayRenderStyle,
                                   VPXTexture, float, float, float, float,
                                   float, float, float, float, float, float,
                                   float, VPXTexture, float, float, float,
                                   float, float, float, float, float, float,
                                   float, float, float, float) {}
  static void MSGPIAPI DrawSegDisplay(VPXRenderContext2D*,
                                      VPXSegDisplayRenderStyle,
                                      VPXSegDisplayHint, VPXTexture, float,
                                      float, float, float, float, float, float,
                                      float, float, float, float,
                                      SegElementType,
                                      const float*, float, float, float, float,
                                      float, float, float, float, float, float,
                                      float, float, float);

  void LoadPluginById(const std::string& id);
  void ConfigureSetting(const std::string& pluginId,
                        MsgPI::MsgPluginManager::SettingAction action,
                        MsgSettingDef* settingDef);
  bool EnsureB2SServer();
  void ReleaseB2SServer();
  std::optional<unsigned int> FindScriptMember(
      const ScriptClassDef* classDef, const char* name,
      std::initializer_list<const char*> argTypes) const;
  bool CallB2SMember(const char* name, std::initializer_list<const char*> argTypes,
                     ScriptVariant* args);
  void DispatchB2SEvent(const PUPQueueEventMsg& event);
  void DispatchB2SSegmentDigit(const B2SSegmentDigitMsg& digit);
  void DispatchB2SPlayerScore(const B2SPlayerScoreMsg& score);
  bool EnsureBackglassWindow();
  void DestroyBackglassWindow();

  AudioOutput* audioOutput_ = nullptr;
  MsgPI::MsgPluginManager pluginManager_;
  std::shared_ptr<MsgPI::MsgPlugin> hostPlugin_;
  std::vector<std::shared_ptr<MsgPI::MsgPlugin>> loadedPlugins_;
  std::mutex pendingMutex_;
  std::vector<PUPQueueEventMsg> pendingEvents_;
  std::vector<B2SSegmentDigitMsg> pendingB2SSegmentDigits_;
  std::vector<B2SPlayerScoreMsg> pendingB2SPlayerScores_;
  std::vector<std::pair<int, int>> pendingSoundCommands_;
  std::unordered_map<int, int> debugLastB2SSegmentDigit_;
  Options options_;
  std::string pluginDir_;
  std::string pupFolder_;
  std::string altSoundFolder_;
  std::string tablePath_;
  std::string prefPath_;
  std::string gameId_;
  bool initialized_ = false;
  bool gameStarted_ = false;
  bool sdlVideoInitialized_ = false;
  SDL_Window* backglassWindow_ = nullptr;
  SDL_Renderer* backglassRenderer_ = nullptr;
  bool backglassFrameDrewImage_ = false;
  bool loggedNoBackglassRenderer_ = false;
  bool loggedNoPluginBackglassRenderer_ = false;
  bool loggedBackglassRenderer_ = false;
  bool loggedB2SServerUnavailable_ = false;
  uint64_t lastBackglassDiagnosticMs_ = 0;

  unsigned int getLoggingApiId_ = 0;
  unsigned int getScriptApiId_ = 0;
  unsigned int getVpxApiId_ = 0;
  unsigned int getAudioSrcId_ = 0;
  unsigned int audioUpdateId_ = 0;
  unsigned int getAuxRendererId_ = 0;
  unsigned int onControllerGameStartId_ = 0;
  unsigned int onControllerGameEndId_ = 0;
  unsigned int onVpxGameEndId_ = 0;
  unsigned int onSoundCommandId_ = 0;
  unsigned int pupQueueEventId_ = 0;

  AudioSrcId pinmameAudioSrc_ = {};
  uint32_t nextAudioResId_ = 1;
  std::unordered_map<std::string, ScriptClassDef*> scriptClasses_;
  std::unordered_map<std::string, const ScriptClassDef*> comOverrides_;
  const ScriptClassDef* b2sServerClass_ = nullptr;
  void* b2sServer_ = nullptr;

  static Impl* instance_;
  static LoggingPluginAPI loggingApi_;
  static ScriptablePluginAPI scriptApi_;
  static VPXPluginAPI vpxApi_;
};

MediaPluginHost::Impl* MediaPluginHost::Impl::instance_ = nullptr;

LoggingPluginAPI MediaPluginHost::Impl::loggingApi_ = {
    .Log = &MediaPluginHost::Impl::Log,
};

ScriptablePluginAPI MediaPluginHost::Impl::scriptApi_ = {
    .RegisterScriptClass = &MediaPluginHost::Impl::RegisterScriptClass,
    .RegisterScriptTypeAlias = &MediaPluginHost::Impl::RegisterScriptTypeAlias,
    .RegisterScriptArrayType = &MediaPluginHost::Impl::RegisterScriptArrayType,
    .SubmitTypeLibrary = &MediaPluginHost::Impl::SubmitTypeLibrary,
    .UnregisterScriptClass = &MediaPluginHost::Impl::UnregisterScriptClass,
    .UnregisterScriptTypeAlias =
        &MediaPluginHost::Impl::UnregisterScriptTypeAlias,
    .UnregisterScriptArrayType =
        &MediaPluginHost::Impl::UnregisterScriptArrayType,
    .OnError = &MediaPluginHost::Impl::OnScriptError,
    .SetCOMObjectOverride = &MediaPluginHost::Impl::SetCOMObjectOverride,
    .GetClassDef = &MediaPluginHost::Impl::GetClassDef,
};

VPXPluginAPI MediaPluginHost::Impl::vpxApi_ = {
    .GetVpxInfo = &MediaPluginHost::Impl::GetVpxInfo,
    .GetTableInfo = &MediaPluginHost::Impl::GetTableInfo,
    .PushNotification = &MediaPluginHost::Impl::PushNotification,
    .UpdateNotification = &MediaPluginHost::Impl::UpdateNotification,
    .DisableStaticPrerendering =
        &MediaPluginHost::Impl::DisableStaticPrerendering,
    .GetActiveViewSetup = &MediaPluginHost::Impl::GetActiveViewSetup,
    .SetActiveViewSetup = &MediaPluginHost::Impl::SetActiveViewSetup,
    .GetInputState = &MediaPluginHost::Impl::GetInputState,
    .SetInputState = &MediaPluginHost::Impl::SetInputState,
    .GetGameTime = &MediaPluginHost::Impl::GetGameTime,
    .CreateTexture = &MediaPluginHost::Impl::CreateTexture,
    .UpdateTexture = &MediaPluginHost::Impl::UpdateTexture,
    .GetTextureInfo = &MediaPluginHost::Impl::GetTextureInfo,
    .DeleteTexture = &MediaPluginHost::Impl::DeleteTexture,
};

bool MediaPluginHost::Impl::Initialize(const Options& options,
                                       std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }
  if (initialized_)
  {
    return true;
  }

  instance_ = this;
  options_ = options;
  if (options.pluginDir && options.pluginDir[0] != '\0')
  {
    pluginDir_ = options.pluginDir;
  }
  else
  {
    std::vector<std::filesystem::path> candidates;
    if (const char* basePath = SDL_GetBasePath();
        basePath != nullptr && basePath[0] != '\0')
    {
      const std::filesystem::path executableDir(basePath);
      candidates.push_back(executableDir / "plugins");
      candidates.push_back(executableDir / "ppuc" / "plugins");
    }
    candidates.emplace_back("ppuc/plugins");
    candidates.emplace_back("plugins");
    candidates.emplace_back("../vpinball/plugins");

    for (const auto& candidate : candidates)
    {
      if (std::filesystem::exists(candidate))
      {
        pluginDir_ = candidate.string();
        break;
      }
    }
    if (pluginDir_.empty())
    {
      pluginDir_ = candidates.back().string();
    }
  }
  pupFolder_ = options.pupFolder && options.pupFolder[0] != '\0'
                   ? options.pupFolder
                   : (std::getenv("HOME") ? std::getenv("HOME") : "");
  altSoundFolder_ = options.altSoundFolder && options.altSoundFolder[0] != '\0'
                        ? options.altSoundFolder
                        : "";
  tablePath_ = options.tablePath && options.tablePath[0] != '\0'
                   ? options.tablePath
                   : ".";
  prefPath_ = options.prefPath && options.prefPath[0] != '\0'
                  ? options.prefPath
                  : (std::getenv("HOME") ? std::getenv("HOME") : ".");
  SetGameInfo(options.gameId, options.hardwareGen);

  hostPlugin_ = pluginManager_.RegisterPlugin(
      "PPUC", "PPUC", "PPUC media plugin host", "", "", "", HostPluginLoad,
      HostPluginUnload);
  pluginManager_.LoadPlugin(*hostPlugin_);

  pluginManager_.SetSettingsHandler(
      [this](const std::string& pluginId,
             MsgPI::MsgPluginManager::SettingAction action,
             MsgSettingDef* settingDef)
      { ConfigureSetting(pluginId, action, settingDef); });

  const MsgPluginAPI& api = pluginManager_.GetMsgAPI();
  getLoggingApiId_ = api.GetMsgID(LOGPI_NAMESPACE, LOGPI_MSG_GET_API);
  getScriptApiId_ = api.GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
  getVpxApiId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);
  getAudioSrcId_ = api.GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_GET_SRC_MSG);
  audioUpdateId_ = api.GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_ON_UPDATE_MSG);
  getAuxRendererId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_AUX_RENDERER);
  onControllerGameStartId_ =
      api.GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START);
  onControllerGameEndId_ =
      api.GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END);
  onVpxGameEndId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END);
  onSoundCommandId_ =
      api.GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_SOUND_COMMAND);
  pupQueueEventId_ = api.GetMsgID(PUPPI_NAMESPACE, PUPPI_MSG_QUEUE_EVENT);

  api.SubscribeMsg(kHostEndpointId, getLoggingApiId_, OnGetLoggingApi, this);
  api.SubscribeMsg(kHostEndpointId, getScriptApiId_, OnGetScriptApi, this);
  api.SubscribeMsg(kHostEndpointId, getVpxApiId_, OnGetVpxApi, this);
  api.SubscribeMsg(kHostEndpointId, getAudioSrcId_, OnGetAudioSrc, this);
  api.SubscribeMsg(kHostEndpointId, audioUpdateId_, OnAudioUpdate, this);
  api.SubscribeMsg(kHostEndpointId, getAuxRendererId_, OnGetBackglassRenderer,
                   this);

  if (!std::filesystem::exists(pluginDir_))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "plugin directory does not exist: " + pluginDir_;
    }
    return false;
  }
  std::printf("Media plugin directory: %s\n", pluginDir_.c_str());

  pluginManager_.ScanPluginFolder(std::make_shared<SDLModuleLoader>(),
                                  pluginDir_, [](MsgPI::MsgPlugin&) {});
  if (options.enablePup)
  {
    LoadPluginById("PUP");
  }
  if (options.enableAltSound)
  {
    LoadPluginById("AltSound");
  }
  if (options.enableB2S)
  {
    LoadPluginById("B2S");
    EnsureB2SServer();
  }

  pinmameAudioSrc_.id.endpointId = kHostEndpointId;
  pinmameAudioSrc_.id.resId = nextAudioResId_++;
  pinmameAudioSrc_.overrideId.id = 0;
  pinmameAudioSrc_.type = CTLPI_AUDIO_SRC_BACKGLASS_STEREO;
  pinmameAudioSrc_.format = CTLPI_AUDIO_FORMAT_SAMPLE_INT16;
  pinmameAudioSrc_.sampleRate = 44100;

  initialized_ = true;
  return true;
}

void MediaPluginHost::Impl::Shutdown()
{
  if (!initialized_ && instance_ != this)
  {
    return;
  }

  OnGameEnd();
  ReleaseB2SServer();
  DestroyBackglassWindow();

  const MsgPluginAPI& api = pluginManager_.GetMsgAPI();
  for (auto it = loadedPlugins_.rbegin(); it != loadedPlugins_.rend(); ++it)
  {
    if (*it && (*it)->IsLoaded())
    {
      pluginManager_.UnloadPlugin(**it);
    }
  }
  loadedPlugins_.clear();
  scriptClasses_.clear();
  comOverrides_.clear();

  if (getLoggingApiId_ != 0)
  {
    api.UnsubscribeMsg(getLoggingApiId_, OnGetLoggingApi, this);
    api.ReleaseMsgID(getLoggingApiId_);
    getLoggingApiId_ = 0;
  }
  if (getScriptApiId_ != 0)
  {
    api.UnsubscribeMsg(getScriptApiId_, OnGetScriptApi, this);
    api.ReleaseMsgID(getScriptApiId_);
    getScriptApiId_ = 0;
  }
  if (getVpxApiId_ != 0)
  {
    api.UnsubscribeMsg(getVpxApiId_, OnGetVpxApi, this);
    api.ReleaseMsgID(getVpxApiId_);
    getVpxApiId_ = 0;
  }
  if (getAudioSrcId_ != 0)
  {
    api.UnsubscribeMsg(getAudioSrcId_, OnGetAudioSrc, this);
    api.ReleaseMsgID(getAudioSrcId_);
    getAudioSrcId_ = 0;
  }
  if (audioUpdateId_ != 0)
  {
    api.UnsubscribeMsg(audioUpdateId_, OnAudioUpdate, this);
    api.ReleaseMsgID(audioUpdateId_);
    audioUpdateId_ = 0;
  }
  if (getAuxRendererId_ != 0)
  {
    api.UnsubscribeMsg(getAuxRendererId_, OnGetBackglassRenderer, this);
    api.ReleaseMsgID(getAuxRendererId_);
    getAuxRendererId_ = 0;
  }
  if (onControllerGameStartId_ != 0)
  {
    api.ReleaseMsgID(onControllerGameStartId_);
    onControllerGameStartId_ = 0;
  }
  if (onControllerGameEndId_ != 0)
  {
    api.ReleaseMsgID(onControllerGameEndId_);
    onControllerGameEndId_ = 0;
  }
  if (onVpxGameEndId_ != 0)
  {
    api.ReleaseMsgID(onVpxGameEndId_);
    onVpxGameEndId_ = 0;
  }
  if (onSoundCommandId_ != 0)
  {
    api.ReleaseMsgID(onSoundCommandId_);
    onSoundCommandId_ = 0;
  }
  if (pupQueueEventId_ != 0)
  {
    api.ReleaseMsgID(pupQueueEventId_);
    pupQueueEventId_ = 0;
  }

  if (hostPlugin_ && hostPlugin_->IsLoaded())
  {
    pluginManager_.UnloadPlugin(*hostPlugin_);
  }
  hostPlugin_.reset();
  pluginManager_.SetSettingsHandler(nullptr);
  initialized_ = false;
  if (instance_ == this)
  {
    instance_ = nullptr;
  }
}

void MediaPluginHost::Impl::SetGameInfo(const char* gameId,
                                        uint64_t hardwareGen)
{
  gameId_ = gameId && gameId[0] != '\0' ? gameId : "";
  options_.hardwareGen = hardwareGen;
}

void MediaPluginHost::Impl::OnGameStart()
{
  if (!initialized_ || gameStarted_ || gameId_.empty())
  {
    return;
  }
  gameStarted_ = true;
  CtlOnGameStartMsg msg{
      .gameId = gameId_.c_str(),
      .hardwareGen = options_.hardwareGen,
  };
  const MsgPluginAPI& api = pluginManager_.GetMsgAPI();
  api.BroadcastMsg(kHostEndpointId, onControllerGameStartId_, &msg);
}

void MediaPluginHost::Impl::OnGameEnd()
{
  if (!initialized_ || !gameStarted_)
  {
    return;
  }
  const MsgPluginAPI& api = pluginManager_.GetMsgAPI();
  api.BroadcastMsg(kHostEndpointId, onControllerGameEndId_, nullptr);
  api.BroadcastMsg(kHostEndpointId, onVpxGameEndId_, nullptr);
  gameStarted_ = false;
}

void MediaPluginHost::Impl::QueueEvent(char source, int id, int value)
{
  if (!initialized_)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  if (pendingEvents_.size() < 1024)
  {
    pendingEvents_.push_back(PUPQueueEventMsg{source, id, value});
  }
}

void MediaPluginHost::Impl::QueueSegmentDisplay(int digit, int value)
{
  if (!initialized_ || !options_.enableB2S)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  if (pendingB2SSegmentDigits_.size() < 1024)
  {
    pendingB2SSegmentDigits_.push_back(B2SSegmentDigitMsg{digit, value});
  }
}

void MediaPluginHost::Impl::QueuePlayerScore(int player, int score)
{
  if (!initialized_ || !options_.enableB2S)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  if (pendingB2SPlayerScores_.size() < 128)
  {
    pendingB2SPlayerScores_.push_back(B2SPlayerScoreMsg{player, score});
  }
}

void MediaPluginHost::Impl::QueueDmdTrigger(uint16_t id)
{
  QueueEvent('D', id, 1);
  QueueEvent('D', id, 0);
}

void MediaPluginHost::Impl::OnSoundCommand(int boardNo, int cmd)
{
  if (!initialized_ || onSoundCommandId_ == 0)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(pendingMutex_);
  if (pendingSoundCommands_.size() < 1024)
  {
    pendingSoundCommands_.push_back({boardNo, cmd});
  }
}

void MediaPluginHost::Impl::Process()
{
  if (!initialized_)
  {
    return;
  }
  std::vector<PUPQueueEventMsg> events;
  std::vector<B2SSegmentDigitMsg> b2sSegmentDigits;
  std::vector<B2SPlayerScoreMsg> b2sPlayerScores;
  std::vector<std::pair<int, int>> soundCommands;
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    events.swap(pendingEvents_);
    b2sSegmentDigits.swap(pendingB2SSegmentDigits_);
    b2sPlayerScores.swap(pendingB2SPlayerScores_);
    soundCommands.swap(pendingSoundCommands_);
  }
  for (PUPQueueEventMsg& event : events)
  {
    if (pupQueueEventId_ != 0)
    {
      pluginManager_.GetMsgAPI().BroadcastMsg(kHostEndpointId, pupQueueEventId_,
                                              &event);
    }
    DispatchB2SEvent(event);
  }
  for (const B2SSegmentDigitMsg& digit : b2sSegmentDigits)
  {
    DispatchB2SSegmentDigit(digit);
  }
  for (const B2SPlayerScoreMsg& score : b2sPlayerScores)
  {
    DispatchB2SPlayerScore(score);
  }
  for (const auto& [boardNo, cmd] : soundCommands)
  {
    CtlOnSoundCommandMsg msg{
        .boardNo = static_cast<unsigned int>(boardNo),
        .cmd = static_cast<unsigned int>(cmd),
    };
    pluginManager_.GetMsgAPI().BroadcastMsg(kHostEndpointId, onSoundCommandId_,
                                            &msg);
  }
  pluginManager_.ProcessAsyncCallbacks();

  if (!options_.enablePup && !options_.enableB2S)
  {
    return;
  }
  if (!EnsureBackglassWindow())
  {
    return;
  }

  AncillaryRendererDef entries[8] = {};
  GetAncillaryRendererMsg getRenderer{
      .window = VPXWINDOW_Backglass,
      .maxEntryCount = 8,
      .count = 0,
      .entries = entries,
  };
  pluginManager_.GetMsgAPI().BroadcastMsg(kHostEndpointId, getAuxRendererId_,
                                          &getRenderer);
  if (getRenderer.count == 0)
  {
    if (!loggedNoBackglassRenderer_)
    {
      std::printf("Media backglass: no ancillary renderers registered\n");
      loggedNoBackglassRenderer_ = true;
    }
    return;
  }

  AncillaryRendererDef* renderer = nullptr;
  const char* preferredRendererId = options_.enableB2S ? "B2S" : nullptr;
  if (preferredRendererId != nullptr)
  {
    for (unsigned int i = 0;
         i < getRenderer.count && i < getRenderer.maxEntryCount; ++i)
    {
      if (entries[i].Render != nullptr && entries[i].id != nullptr &&
          std::strcmp(entries[i].id, preferredRendererId) == 0)
      {
        renderer = &entries[i];
        break;
      }
    }
  }
  for (unsigned int i = 0; i < getRenderer.count && i < getRenderer.maxEntryCount;
       ++i)
  {
    if (renderer != nullptr)
    {
      break;
    }
    if (entries[i].Render != nullptr &&
        (entries[i].id == nullptr ||
         std::strcmp(entries[i].id, "PPUCBackglass") != 0))
    {
      renderer = &entries[i];
      break;
    }
  }
  if (renderer == nullptr)
  {
    if (!loggedNoPluginBackglassRenderer_)
    {
      std::printf("Media backglass: no plugin renderer found among %u renderers\n",
                  getRenderer.count);
      loggedNoPluginBackglassRenderer_ = true;
    }
    return;
  }
  if (!loggedBackglassRenderer_)
  {
    std::printf("Media backglass: using renderer %s (%s)\n",
                renderer->id ? renderer->id : "<unnamed>",
                renderer->name ? renderer->name : "");
    loggedBackglassRenderer_ = true;
  }

  SDL_SetRenderDrawColor(backglassRenderer_, 0, 0, 0, 255);
  SDL_RenderClear(backglassRenderer_);

  VPXRenderContext2D ctx{
      .window = VPXWINDOW_Backglass,
      .srcWidth = static_cast<float>(options_.backglassWidth > 0
                                         ? options_.backglassWidth
                                         : kDefaultBackglassWidth),
      .srcHeight = static_cast<float>(options_.backglassHeight > 0
                                          ? options_.backglassHeight
                                          : kDefaultBackglassHeight),
      .is2D = 1,
      .outWidth = static_cast<float>(options_.backglassWidth > 0
                                         ? options_.backglassWidth
                                         : kDefaultBackglassWidth),
      .outHeight = static_cast<float>(options_.backglassHeight > 0
                                          ? options_.backglassHeight
                                          : kDefaultBackglassHeight),
      .DrawImage = &DrawImage,
      .DrawDisplay = &DrawDisplay,
      .DrawSegDisplay = &DrawSegDisplay,
      .rendererData = this,
  };

  backglassFrameDrewImage_ = false;
  const int rendered = renderer->Render(&ctx, renderer->context);
  const uint64_t nowMs = SDL_GetTicks();
  if ((!rendered || !backglassFrameDrewImage_) &&
      nowMs - lastBackglassDiagnosticMs_ >= 2000)
  {
    std::printf("Media backglass: renderer=%s rendered=%d drewImage=%d\n",
                renderer->id ? renderer->id : "<unnamed>", rendered,
                backglassFrameDrewImage_ ? 1 : 0);
    lastBackglassDiagnosticMs_ = nowMs;
  }
  SDL_RenderPresent(backglassRenderer_);
}

void MediaPluginHost::Impl::OnGetLoggingApi(unsigned int, void*, void* msgData)
{
  if (msgData != nullptr)
  {
    *static_cast<LoggingPluginAPI**>(msgData) = &loggingApi_;
  }
}

void MediaPluginHost::Impl::OnGetScriptApi(unsigned int, void*, void* msgData)
{
  if (msgData != nullptr)
  {
    *static_cast<ScriptablePluginAPI**>(msgData) = &scriptApi_;
  }
}

void MediaPluginHost::Impl::OnGetVpxApi(unsigned int, void*, void* msgData)
{
  if (msgData != nullptr)
  {
    *static_cast<VPXPluginAPI**>(msgData) = &vpxApi_;
  }
}

void MediaPluginHost::Impl::OnGetAudioSrc(unsigned int, void* userData,
                                          void* msgData)
{
  auto* self = static_cast<Impl*>(userData);
  auto* msg = static_cast<GetAudioSrcMsg*>(msgData);
  if (self == nullptr || msg == nullptr)
  {
    return;
  }

  if (msg->count < msg->maxEntryCount && msg->entries != nullptr)
  {
    msg->entries[msg->count] = self->pinmameAudioSrc_;
  }
  msg->count++;
}

void MediaPluginHost::Impl::OnAudioUpdate(unsigned int, void* userData,
                                          void* msgData)
{
  auto* self = static_cast<Impl*>(userData);
  auto* msg = static_cast<AudioUpdateMsg*>(msgData);
  if (self == nullptr || msg == nullptr || self->audioOutput_ == nullptr ||
      msg->id.endpointId == kHostEndpointId || msg->buffer == nullptr ||
      msg->bufferSize == 0)
  {
    return;
  }

  std::vector<int16_t> samples;
  ConvertAudioUpdateToS16(*msg, &samples);
  if (samples.empty())
  {
    return;
  }

  const int channels =
      msg->type == CTLPI_AUDIO_SRC_BACKGLASS_MONO ? 1 : 2;
  self->audioOutput_->QueuePluginSamples(samples.data(), samples.size(),
                                         static_cast<int>(msg->sampleRate),
                                         channels);
}

void MediaPluginHost::Impl::OnGetBackglassRenderer(unsigned int, void* userData,
                                                   void* msgData)
{
  auto* self = static_cast<Impl*>(userData);
  auto* msg = static_cast<GetAncillaryRendererMsg*>(msgData);
  if (self == nullptr || msg == nullptr || msg->window != VPXWINDOW_Backglass)
  {
    return;
  }

  if (msg->count < msg->maxEntryCount && msg->entries != nullptr)
  {
    msg->entries[msg->count] = AncillaryRendererDef{
        .id = "PPUCBackglass",
        .name = "PPUC Backglass",
        .description = "PPUC SDL backglass renderer",
        .context = self,
        .Render = &RenderBackglass,
    };
  }
  msg->count++;
}

void MediaPluginHost::Impl::Log(const char* source, const char*, int,
                                unsigned int level, const char* message)
{
  const char* levelName = "INFO";
  if (level >= LPI_LVL_ERROR)
  {
    levelName = "ERROR";
  }
  else if (level >= LPI_LVL_WARN)
  {
    levelName = "WARN";
  }
  else if (level <= LPI_LVL_DEBUG)
  {
    levelName = "DEBUG";
  }
  std::printf("[%s:%s] %s\n", source ? source : "plugin", levelName,
              message ? message : "");
}

void MediaPluginHost::Impl::RegisterScriptClass(ScriptClassDef* classDef)
{
  if (instance_ == nullptr || classDef == nullptr || classDef->name.name == nullptr)
  {
    return;
  }
  instance_->scriptClasses_[classDef->name.name] = classDef;
}

void MediaPluginHost::Impl::UnregisterScriptClass(ScriptClassDef* classDef)
{
  if (instance_ == nullptr || classDef == nullptr || classDef->name.name == nullptr)
  {
    return;
  }
  auto it = instance_->scriptClasses_.find(classDef->name.name);
  if (it != instance_->scriptClasses_.end() && it->second == classDef)
  {
    instance_->scriptClasses_.erase(it);
  }
  for (auto overrideIt = instance_->comOverrides_.begin();
       overrideIt != instance_->comOverrides_.end();)
  {
    if (overrideIt->second == classDef)
    {
      overrideIt = instance_->comOverrides_.erase(overrideIt);
    }
    else
    {
      ++overrideIt;
    }
  }
}

void MediaPluginHost::Impl::OnScriptError(unsigned int type,
                                          const char* message)
{
  std::printf("[plugin-script:%u] %s\n", type, message ? message : "");
}

void MediaPluginHost::Impl::SetCOMObjectOverride(
    const char* classname, const ScriptClassDef* classDef)
{
  if (instance_ == nullptr || classname == nullptr || classname[0] == '\0')
  {
    return;
  }
  if (classDef == nullptr)
  {
    instance_->comOverrides_.erase(classname);
    return;
  }
  instance_->comOverrides_[classname] = classDef;
}

ScriptClassDef* MediaPluginHost::Impl::GetClassDef(const char* typeName)
{
  if (instance_ == nullptr || typeName == nullptr)
  {
    return nullptr;
  }
  auto it = instance_->scriptClasses_.find(typeName);
  return it == instance_->scriptClasses_.end() ? nullptr : it->second;
}

void MediaPluginHost::Impl::GetVpxInfo(VPXInfo* info)
{
  if (info == nullptr || instance_ == nullptr)
  {
    return;
  }
  info->path = instance_->pluginDir_.c_str();
  info->prefPath = instance_->prefPath_.c_str();
}

void MediaPluginHost::Impl::GetTableInfo(VPXTableInfo* info)
{
  if (info == nullptr || instance_ == nullptr)
  {
    return;
  }
  info->path = instance_->tablePath_.c_str();
  info->tableWidth = 0.0f;
  info->tableHeight = 0.0f;
}

double MediaPluginHost::Impl::GetGameTime()
{
  return static_cast<double>(SDL_GetTicks()) / 1000.0;
}

VPXTexture MediaPluginHost::Impl::CreateTexture(uint8_t* rawData, int size)
{
  if (rawData == nullptr || size <= 0)
  {
    return nullptr;
  }

  SDL_IOStream* io = SDL_IOFromConstMem(rawData, size);
  if (io == nullptr)
  {
    return nullptr;
  }
  SDL_Surface* surface = IMG_Load_IO(io, true);
  if (surface == nullptr)
  {
    return nullptr;
  }
  SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surface);
  if (converted == nullptr)
  {
    return nullptr;
  }

  auto* hostTexture = new HostTexture();
  hostTexture->width = converted->w;
  hostTexture->height = converted->h;
  hostTexture->format = VPXTEXFMT_sRGBA8;
  hostTexture->pixels.resize(static_cast<size_t>(converted->w) *
                             static_cast<size_t>(converted->h) * 4u);
  for (int y = 0; y < converted->h; ++y)
  {
    const auto* src = static_cast<const uint8_t*>(converted->pixels) +
                      static_cast<size_t>(y) * converted->pitch;
    auto* dst = hostTexture->pixels.data() +
                static_cast<size_t>(y) * static_cast<size_t>(converted->w) * 4u;
    std::memcpy(dst, src, static_cast<size_t>(converted->w) * 4u);
  }
  hostTexture->dirty = true;
  SDL_DestroySurface(converted);
  return hostTexture;
}

void MediaPluginHost::Impl::UpdateTexture(VPXTexture* texture, int width,
                                          int height, VPXTextureFormat format,
                                          const void* image)
{
  if (texture == nullptr || width <= 0 || height <= 0)
  {
    return;
  }

  auto* hostTexture = static_cast<HostTexture*>(*texture);
  if (hostTexture == nullptr)
  {
    hostTexture = new HostTexture();
    *texture = hostTexture;
  }

  std::lock_guard<std::mutex> lock(hostTexture->mutex);
  const size_t size =
      static_cast<size_t>(width) * static_cast<size_t>(height) *
      static_cast<size_t>(BytesPerPixel(format));
  hostTexture->width = width;
  hostTexture->height = height;
  hostTexture->format = format;
  hostTexture->pixels.resize(size);
  if (image != nullptr)
  {
    std::memcpy(hostTexture->pixels.data(), image, size);
  }
  hostTexture->dirty = true;
}

VPXTextureInfo* MediaPluginHost::Impl::GetTextureInfo(VPXTexture texture)
{
  auto* hostTexture = static_cast<HostTexture*>(texture);
  if (hostTexture == nullptr)
  {
    return nullptr;
  }

  static thread_local VPXTextureInfo info;
  std::lock_guard<std::mutex> lock(hostTexture->mutex);
  info.width = static_cast<unsigned int>(hostTexture->width);
  info.height = static_cast<unsigned int>(hostTexture->height);
  info.format = hostTexture->format;
  info.data = hostTexture->pixels.data();
  return &info;
}

void MediaPluginHost::Impl::DeleteTexture(VPXTexture texture)
{
  auto* hostTexture = static_cast<HostTexture*>(texture);
  if (hostTexture == nullptr)
  {
    return;
  }
  if (hostTexture->sdlTexture != nullptr)
  {
    if (hostTexture->sdlRenderer != nullptr)
    {
      SDL_FlushRenderer(hostTexture->sdlRenderer);
    }
    SDL_DestroyTexture(hostTexture->sdlTexture);
    hostTexture->sdlTexture = nullptr;
    hostTexture->sdlRenderer = nullptr;
  }
  delete hostTexture;
}

int MediaPluginHost::Impl::RenderBackglass(VPXRenderContext2D*, void*)
{
  return true;
}

void MediaPluginHost::Impl::DrawImage(VPXRenderContext2D* ctx,
                                      VPXTexture texture, float tintR,
                                      float tintG, float tintB, float alpha,
                                      float texX, float texY, float texW,
                                      float texH, float, float, float,
                                      float srcX, float srcY, float srcW,
                                      float srcH)
{
  auto* self = ctx == nullptr ? nullptr : static_cast<Impl*>(ctx->rendererData);
  auto* hostTexture = static_cast<HostTexture*>(texture);
  if (self == nullptr || self->backglassRenderer_ == nullptr ||
      hostTexture == nullptr)
  {
    return;
  }
  self->backglassFrameDrewImage_ = true;

  std::lock_guard<std::mutex> lock(hostTexture->mutex);
  if (hostTexture->width <= 0 || hostTexture->height <= 0 ||
      hostTexture->pixels.empty())
  {
    return;
  }

  const bool recreateTexture =
      hostTexture->sdlTexture == nullptr ||
      hostTexture->sdlRenderer != self->backglassRenderer_ ||
      hostTexture->sdlTextureWidth != hostTexture->width ||
      hostTexture->sdlTextureHeight != hostTexture->height ||
      hostTexture->sdlTextureFormat != hostTexture->format;
  if (recreateTexture || hostTexture->dirty)
  {
    SDL_FlushRenderer(self->backglassRenderer_);
  }

  if (recreateTexture)
  {
    if (hostTexture->sdlTexture != nullptr)
    {
      SDL_DestroyTexture(hostTexture->sdlTexture);
      hostTexture->sdlTexture = nullptr;
      hostTexture->sdlRenderer = nullptr;
    }
    hostTexture->sdlTexture = SDL_CreateTexture(
        self->backglassRenderer_, SdlFormat(hostTexture->format),
        SDL_TEXTUREACCESS_STATIC, hostTexture->width, hostTexture->height);
    if (hostTexture->sdlTexture == nullptr)
    {
      return;
    }
    hostTexture->sdlRenderer = self->backglassRenderer_;
    hostTexture->sdlTextureWidth = hostTexture->width;
    hostTexture->sdlTextureHeight = hostTexture->height;
    hostTexture->sdlTextureFormat = hostTexture->format;
    SDL_SetTextureBlendMode(hostTexture->sdlTexture, SDL_BLENDMODE_BLEND);
  }
  if (hostTexture->dirty)
  {
    SDL_UpdateTexture(hostTexture->sdlTexture, nullptr,
                      hostTexture->pixels.data(),
                      hostTexture->width * BytesPerPixel(hostTexture->format));
    hostTexture->dirty = false;
  }

  SDL_FRect src{texX, texY, texW, texH};
  SDL_FRect dst = ScaleSourceRectToOutput(ctx, srcX, srcY, srcW, srcH);
  SDL_SetTextureColorMod(hostTexture->sdlTexture, FloatToByte(tintR),
                         FloatToByte(tintG), FloatToByte(tintB));
  SDL_SetTextureAlphaMod(hostTexture->sdlTexture, FloatToByte(alpha));
  SDL_RenderTexture(self->backglassRenderer_, hostTexture->sdlTexture, &src,
                    &dst);
}

void MediaPluginHost::Impl::DrawSegDisplay(
    VPXRenderContext2D* ctx, VPXSegDisplayRenderStyle style,
    VPXSegDisplayHint, VPXTexture, float, float, float, float, float, float,
    float, float, float, float, float, SegElementType type, const float* state,
    float dispTintR, float dispTintG, float dispTintB, float brightness,
    float alpha, float, float, float, float, float srcX, float srcY,
    float srcW, float srcH)
{
  auto* self = ctx == nullptr ? nullptr : static_cast<Impl*>(ctx->rendererData);
  if (self == nullptr || self->backglassRenderer_ == nullptr ||
      state == nullptr || alpha <= 0.0f)
  {
    return;
  }

  SDL_FRect dst = ScaleSourceRectToOutputBottomOrigin(ctx, srcX, srcY, srcW,
                                                      srcH);
  if (dst.w <= 0.0f || dst.h <= 0.0f)
  {
    return;
  }

  int segmentCount = 16;
  switch (type)
  {
    case CTLPI_SEG_LAYOUT_7:
      segmentCount = 7;
      break;
    case CTLPI_SEG_LAYOUT_7C:
    case CTLPI_SEG_LAYOUT_7D:
      segmentCount = 8;
      break;
    case CTLPI_SEG_LAYOUT_9:
      segmentCount = 9;
      break;
    case CTLPI_SEG_LAYOUT_9C:
      segmentCount = 10;
      break;
    case CTLPI_SEG_LAYOUT_14:
      segmentCount = 14;
      break;
    case CTLPI_SEG_LAYOUT_14D:
      segmentCount = 15;
      break;
    case CTLPI_SEG_LAYOUT_14DC:
    case CTLPI_SEG_LAYOUT_16:
    default:
      segmentCount = 16;
      break;
  }

  SDL_SetRenderDrawBlendMode(self->backglassRenderer_, SDL_BLENDMODE_ADD);
  const float thickness = std::max(1.0f, std::min(dst.w, dst.h) * 0.052f);
  const float glow = std::clamp(self->options_.b2sSegmentGlow, 0.0f, 4.0f);
  const float glowThickness =
      std::max(thickness * (1.8f + glow * 0.75f),
               std::min(dst.w, dst.h) * (0.055f + glow * 0.035f));
  const float segmentAngle =
      std::clamp(self->options_.b2sSegmentAngleDegrees, -30.0f, 30.0f);
  const float skew = -std::tan(segmentAngle * kPi / 180.0f);
  for (int i = 0; i < segmentCount; ++i)
  {
    if (state[i] <= 0.01f)
    {
      continue;
    }
    const SDL_FColor color =
        SegmentColor(style, dispTintR, dispTintG, dispTintB, state[i],
                     brightness, alpha);
    if (glow > 0.0f)
    {
      DrawSegmentByIndex(self->backglassRenderer_, i, dst, glowThickness,
                         ScaleColorAlpha(color, 0.11f * glow), skew);
    }
    if (self->options_.b2sSegmentSmoothing)
    {
      DrawSegmentByIndex(self->backglassRenderer_, i, dst, thickness * 1.35f,
                         ScaleColorAlpha(color, 0.32f), skew);
    }
    DrawSegmentByIndex(self->backglassRenderer_, i, dst, thickness, color,
                       skew);
  }
  SDL_SetRenderDrawBlendMode(self->backglassRenderer_, SDL_BLENDMODE_BLEND);
  self->backglassFrameDrewImage_ = true;
}

void MediaPluginHost::Impl::LoadPluginById(const std::string& id)
{
  auto plugin = pluginManager_.GetPlugin(id);
  if (plugin == nullptr)
  {
    std::printf("Media plugin not found: %s\n", id.c_str());
    return;
  }
  pluginManager_.LoadPlugin(*plugin);
  if (plugin->IsLoaded())
  {
    loadedPlugins_.push_back(plugin);
  }
  else
  {
    std::printf("Media plugin failed to load: %s\n", id.c_str());
  }
}

bool MediaPluginHost::Impl::EnsureB2SServer()
{
  if (!options_.enableB2S)
  {
    return false;
  }
  if (b2sServer_ != nullptr)
  {
    return true;
  }

  auto it = comOverrides_.find("B2S.Server");
  if (it == comOverrides_.end() || it->second == nullptr ||
      it->second->CreateObject == nullptr)
  {
    if (!loggedB2SServerUnavailable_)
    {
      std::printf("B2S server is not available from plugin script API\n");
      loggedB2SServerUnavailable_ = true;
    }
    return false;
  }

  b2sServerClass_ = it->second;
  b2sServer_ = b2sServerClass_->CreateObject();
  if (b2sServer_ == nullptr)
  {
    std::printf("B2S server object creation failed\n");
    b2sServerClass_ = nullptr;
    return false;
  }
  return true;
}

void MediaPluginHost::Impl::ReleaseB2SServer()
{
  if (b2sServer_ == nullptr || b2sServerClass_ == nullptr)
  {
    b2sServer_ = nullptr;
    b2sServerClass_ = nullptr;
    return;
  }

  if (auto member = FindScriptMember(b2sServerClass_, "Release", {}))
  {
    b2sServerClass_->members[*member].Call(b2sServer_, static_cast<int>(*member),
                                           nullptr, nullptr);
  }
  b2sServer_ = nullptr;
  b2sServerClass_ = nullptr;
}

std::optional<unsigned int> MediaPluginHost::Impl::FindScriptMember(
    const ScriptClassDef* classDef, const char* name,
    std::initializer_list<const char*> argTypes) const
{
  if (classDef == nullptr || name == nullptr)
  {
    return std::nullopt;
  }
  for (unsigned int i = 0; i < classDef->nMembers; ++i)
  {
    const ScriptClassMemberDef& member = classDef->members[i];
    if (member.name.name == nullptr || std::strcmp(member.name.name, name) != 0 ||
        member.nArgs != argTypes.size() || member.Call == nullptr)
    {
      continue;
    }

    bool argsMatch = true;
    unsigned int argIndex = 0;
    for (const char* expectedType : argTypes)
    {
      const char* actualType = member.callArgType[argIndex].name;
      if (expectedType == nullptr || actualType == nullptr ||
          std::strcmp(actualType, expectedType) != 0)
      {
        argsMatch = false;
        break;
      }
      ++argIndex;
    }
    if (argsMatch)
    {
      return i;
    }
  }
  return std::nullopt;
}

bool MediaPluginHost::Impl::CallB2SMember(
    const char* name, std::initializer_list<const char*> argTypes,
    ScriptVariant* args)
{
  if (!EnsureB2SServer())
  {
    return false;
  }
  auto member = FindScriptMember(b2sServerClass_, name, argTypes);
  if (!member)
  {
    return false;
  }
  b2sServerClass_->members[*member].Call(b2sServer_, static_cast<int>(*member),
                                         args, nullptr);
  return true;
}

void MediaPluginHost::Impl::DispatchB2SEvent(const PUPQueueEventMsg& event)
{
  if (!options_.enableB2S)
  {
    return;
  }
  if (event.source != 'L' && event.source != 'S' && event.source != 'G')
  {
    return;
  }

  ScriptVariant args[2] = {};
  args[0].vInt = event.id;
  args[1].vInt = event.value;
  CallB2SMember("B2SSetData", {"int", "int"}, args);
}

void MediaPluginHost::Impl::DispatchB2SSegmentDigit(
    const B2SSegmentDigitMsg& digit)
{
  if (!options_.enableB2S)
  {
    return;
  }
  if (options_.debug)
  {
    auto it = debugLastB2SSegmentDigit_.find(digit.digit);
    if (it == debugLastB2SSegmentDigit_.end() || it->second != digit.value)
    {
      debugLastB2SSegmentDigit_[digit.digit] = digit.value;
      std::printf("B2S dispatch score digit: digit=%d value=%d\n",
                  digit.digit, digit.value);
    }
  }
  ScriptVariant args[2] = {};
  args[0].vInt = digit.digit;
  args[1].vInt = digit.value;
  CallB2SMember("B2SSetScoreDigit", {"int", "int"}, args);
}

void MediaPluginHost::Impl::DispatchB2SPlayerScore(
    const B2SPlayerScoreMsg& score)
{
  if (!options_.enableB2S)
  {
    return;
  }
  ScriptVariant args[2] = {};
  args[0].vInt = score.player;
  args[1].vInt = score.score;
  CallB2SMember("B2SSetScorePlayer", {"int", "int"}, args);
}

void MediaPluginHost::Impl::ConfigureSetting(
    const std::string& pluginId,
    MsgPI::MsgPluginManager::SettingAction action, MsgSettingDef* settingDef)
{
  if (action != MsgPI::MsgPluginManager::SettingAction::Load ||
      settingDef == nullptr)
  {
    return;
  }

  if (settingDef->type == MSGPI_SETTING_TYPE_STRING)
  {
    const char* value = settingDef->stringDef.defVal;
    if (pluginId == "PUP" && std::strcmp(settingDef->propId, "PUPFolder") == 0)
    {
      value = pupFolder_.c_str();
    }
    else if (pluginId == "AltSound" &&
             std::strcmp(settingDef->propId, "Folder") == 0)
    {
      value = altSoundFolder_.c_str();
    }
    if (settingDef->stringDef.Set != nullptr)
    {
      settingDef->stringDef.Set(value ? value : "");
    }
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_BOOL &&
           settingDef->boolDef.Set != nullptr)
  {
    settingDef->boolDef.Set(settingDef->boolDef.defVal);
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_INT &&
           settingDef->intDef.Set != nullptr)
  {
    settingDef->intDef.Set(settingDef->intDef.defVal);
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_FLOAT &&
           settingDef->floatDef.Set != nullptr)
  {
    settingDef->floatDef.Set(settingDef->floatDef.defVal);
  }
}

bool MediaPluginHost::Impl::EnsureBackglassWindow()
{
  if (backglassWindow_ != nullptr && backglassRenderer_ != nullptr)
  {
    return true;
  }
  if (!sdlVideoInitialized_)
  {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
      std::printf("SDL video init failed for media backglass: %s\n",
                  SDL_GetError());
      return false;
    }
    sdlVideoInitialized_ = true;
  }

  const int width =
      options_.backglassWidth > 0 ? options_.backglassWidth
                                  : kDefaultBackglassWidth;
  const int height =
      options_.backglassHeight > 0 ? options_.backglassHeight
                                   : kDefaultBackglassHeight;
  backglassWindow_ =
      SDL_CreateWindow("PPUC Backglass", width, height, SDL_WINDOW_HIDDEN);
  if (backglassWindow_ == nullptr)
  {
    std::printf("Media backglass window creation failed: %s\n", SDL_GetError());
    return false;
  }
  if (options_.backglassScreen >= 0)
  {
    SDL_SetWindowFullscreenMode(backglassWindow_, nullptr);
  }
  SDL_ShowWindow(backglassWindow_);

  const char* rendererName = nullptr;
#if defined(__APPLE__)
  rendererName = "opengl";
#endif
  backglassRenderer_ = SDL_CreateRenderer(backglassWindow_, rendererName);
  if (backglassRenderer_ == nullptr && rendererName != nullptr)
  {
    std::printf("Media backglass %s renderer creation failed: %s\n",
                rendererName, SDL_GetError());
    backglassRenderer_ = SDL_CreateRenderer(backglassWindow_, nullptr);
  }
  if (backglassRenderer_ == nullptr)
  {
    std::printf("Media backglass renderer creation failed: %s\n",
                SDL_GetError());
    SDL_DestroyWindow(backglassWindow_);
    backglassWindow_ = nullptr;
    return false;
  }
  std::printf("Media backglass renderer: %s\n",
              SDL_GetRendererName(backglassRenderer_));
  return true;
}

void MediaPluginHost::Impl::DestroyBackglassWindow()
{
  if (backglassRenderer_ != nullptr)
  {
    SDL_DestroyRenderer(backglassRenderer_);
    backglassRenderer_ = nullptr;
  }
  if (backglassWindow_ != nullptr)
  {
    SDL_DestroyWindow(backglassWindow_);
    backglassWindow_ = nullptr;
  }
}

MediaPluginHost::MediaPluginHost(AudioOutput* audioOutput)
    : impl_(std::make_unique<Impl>(audioOutput))
{
}

MediaPluginHost::~MediaPluginHost() = default;

bool MediaPluginHost::Initialize(const Options& options,
                                 std::string* errorMessage)
{
  return impl_->Initialize(options, errorMessage);
}

void MediaPluginHost::Shutdown() { impl_->Shutdown(); }

void MediaPluginHost::SetGameInfo(const char* gameId, uint64_t hardwareGen)
{
  impl_->SetGameInfo(gameId, hardwareGen);
}

void MediaPluginHost::OnGameStart() { impl_->OnGameStart(); }

void MediaPluginHost::OnGameEnd() { impl_->OnGameEnd(); }

void MediaPluginHost::QueueEvent(char source, int id, int value)
{
  impl_->QueueEvent(source, id, value);
}

void MediaPluginHost::QueueSegmentDisplay(int digit, int value)
{
  impl_->QueueSegmentDisplay(digit, value);
}

void MediaPluginHost::QueuePlayerScore(int player, int score)
{
  impl_->QueuePlayerScore(player, score);
}

void MediaPluginHost::QueueDmdTrigger(uint16_t id)
{
  impl_->QueueDmdTrigger(id);
}

void MediaPluginHost::OnSoundCommand(int boardNo, int cmd)
{
  impl_->OnSoundCommand(boardNo, cmd);
}

void MediaPluginHost::Process() { impl_->Process(); }
