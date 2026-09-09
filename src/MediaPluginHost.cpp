#include "MediaPluginHost.h"

#include "PluginBus.h"
#include "ScriptObject.h"

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

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"

#include "pinmame/PinMAMEPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/MsgPluginManager.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/VPXPlugin.h"

namespace
{
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

// One machine-state change on its way to the plugins. Matches the payload
// B2SPluginEventStream expects on "B2S"/"OnStateChange:1" byte for byte, which
// is the only un-gated way into PUP's and DOF's event streams.
struct PpucPluginEvent
{
  uint8_t type;
  int32_t index;
  int32_t value;
};

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

  const float volume = std::isfinite(msg.volume)
                           ? std::clamp(msg.volume, 0.0f, 1.0f)
                           : 0.0f;
  if (msg.sampleFormat == CTLPI_AUDIO_FORMAT_SAMPLE_FLOAT)
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

  if (msg.sampleFormat == CTLPI_AUDIO_FORMAT_SAMPLE_INT16)
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
    SetDllDirectoryA(directory.c_str());
#else
    (void)directory;
#endif
    void* module = SDL_LoadObject(file.c_str());
#if defined(_WIN32) || defined(__MINGW32__)
    SetDllDirectoryA(nullptr);
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
  Impl(AudioOutput* audioOutput, PluginBus& bus) : audioOutput_(audioOutput), bus_(bus) {}
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

  static void MSGPIAPI OnGetVpxApi(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetAudioSrc(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnAudioUpdate(unsigned int, void*, void* msgData);
  static void MSGPIAPI OnGetBackglassRenderer(unsigned int, void*,
                                              void* msgData);

  void OnAudioSrcChanged();



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

  bool EnsureB2SServer();
  void ReleaseB2SServer();
  bool CallB2SMember(const char* name, std::initializer_list<const char*> argTypes,
                     ScriptVariant* args);
  void DispatchB2SEvent(const PpucPluginEvent& event);
  void DispatchB2SSegmentDigit(const B2SSegmentDigitMsg& digit);
  void DispatchB2SPlayerScore(const B2SPlayerScoreMsg& score);
  bool EnsureBackglassWindow();
  void DestroyBackglassWindow();

  AudioOutput* audioOutput_ = nullptr;
  bool debugAudio_ = false;
  uint64_t lastAudioDebugMs_ = 0;
  PluginBus& bus_;
  std::mutex pendingMutex_;
  std::vector<PpucPluginEvent> pendingEvents_;
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
  bool loggedRendererContention_ = false;
  bool loggedB2SServerUnavailable_ = false;
  uint64_t lastBackglassDiagnosticMs_ = 0;

  unsigned int getVpxApiId_ = 0;
  unsigned int getAudioSrcId_ = 0;
  unsigned int audioUpdateId_ = 0;
  unsigned int getAuxRendererId_ = 0;
  unsigned int onVpxGameEndId_ = 0;
  unsigned int onAudioCmdId_ = 0;
  std::unique_ptr<PinballPlugin::Controller::CtrlItemProvider<ControllerDef>> controllerProvider_;
  std::unique_ptr<PinballPlugin::Controller::CtrlItemConsumer<AudioSrcId>> audioSources_;
  std::string controllerGameId_;
  unsigned int b2sStateChangeId_ = 0;

  AudioSrcId pinmameAudioSrc_ = {};
  ScriptObject b2sServer_;

  static Impl* instance_;
  static VPXPluginAPI vpxApi_;
};

MediaPluginHost::Impl* MediaPluginHost::Impl::instance_ = nullptr;



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
    candidates.emplace_back("/usr/lib/ppuc/plugins");
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
  debugAudio_ = options.debugAudio;
  SetGameInfo(options.gameId, options.hardwareGen);

  const MsgPluginAPI& api = bus_.Api();
  getVpxApiId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);
  getAudioSrcId_ = api.GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_GET_SRC_MSG);
  audioUpdateId_ = api.GetMsgID(CTLPI_NAMESPACE, CTLPI_AUDIO_ON_UPDATE_MSG);
  getAuxRendererId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_AUX_RENDERER);
  onVpxGameEndId_ = api.GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END);
  onAudioCmdId_ = api.GetMsgID(PMPI_NAMESPACE, PMPI_EVT_ON_AUDIO_CMD);

  if (options.provideController)
  {
    controllerProvider_ = std::make_unique<
        PinballPlugin::Controller::CtrlItemProvider<ControllerDef>>(
        &api, bus_.HostEndpointId(), CTLPI_CONTROLLERS_GET_MSG,
        CTLPI_CONTROLLERS_ON_CHG_MSG);
  }
  // PUPPI_MSG_QUEUE_EVENT was a PPUC-only addition to the fork and no longer
  // exists: PUP discovers controller state from the bus instead of being
  // pushed events. B2SPluginEventStream -- which drives both PUP and DOF --
  // still subscribes unconditionally to this message, so it stays the way to
  // inject the state no controller can know: rules-authored triggers, and the
  // board-local switches PinMAME never sees.
  b2sStateChangeId_ = api.GetMsgID("B2S", "OnStateChange:1");

  // Overriding is declared source to source, so the host has to see the whole
  // published topology to know which lanes AltSound (or anything else) is
  // claiming. Nothing pushes that; it has to be consumed.
  audioSources_ = std::make_unique<PinballPlugin::Controller::CtrlItemConsumer<AudioSrcId>>(
      &api, bus_.HostEndpointId(), CTLPI_AUDIO_GET_SRC_MSG, CTLPI_AUDIO_ON_SRC_CHG_MSG,
      [](std::vector<AudioSrcId>&) {}, []() {}, [this]() { OnAudioSrcChanged(); });
  audioSources_->Subscribe();

  api.SubscribeMsg(bus_.HostEndpointId(), getVpxApiId_, OnGetVpxApi, this);
  api.SubscribeMsg(bus_.HostEndpointId(), getAudioSrcId_, OnGetAudioSrc, this);
  api.SubscribeMsg(bus_.HostEndpointId(), audioUpdateId_, OnAudioUpdate, this);
  api.SubscribeMsg(bus_.HostEndpointId(), getAuxRendererId_, OnGetBackglassRenderer,
                   this);

  if (options.enablePup)
  {
    // The plugin reads PUPFolder at load and expects <PUPFolder>/pupvideos.
    // Without it, it falls back to a per-table pupvideos folder that PPUC's
    // layout does not have, logs "No global PUP folder configured", and plays
    // nothing -- while libdmdutil, which gets the same path directly, happily
    // loads the pack's DMD triggers. Half the pack working is worse than none.
    if (!pupFolder_.empty())
    {
      bus_.SetSettingOverride("PUP", "PUPFolder", pupFolder_);
    }
    bus_.LoadPluginById("PUP");
  }
  if (options.enableAltSound)
  {
    bus_.LoadPluginById("AltSound");
  }
  if (options.enableB2S)
  {
    bus_.LoadPluginById("B2S");
    EnsureB2SServer();
  }

  pinmameAudioSrc_.id.endpointId = bus_.HostEndpointId();
  // resId 0 by convention: an overrider names its target as {endpointId, 0},
  // which is what AltSound publishes and what libpinmame uses for the ROM
  // stream. Numbering from 1 here would make PPUC's own audio unoverridable.
  pinmameAudioSrc_.id.resId = 0;
  pinmameAudioSrc_.overrideId.id = 0;
  pinmameAudioSrc_.name = "PPUC";
  pinmameAudioSrc_.desc = "PPUC game audio";
  pinmameAudioSrc_.target = CTLPI_AUDIO_TARGET_BACKGLASS;

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

  // PluginBus unloads the plugins and clears the script registries; it
  // outlives this object precisely so that happens after every client is gone.
  const MsgPluginAPI& api = bus_.Api();

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
  // Mandatory: CtrlItemConsumer's destructor asserts it is not still subscribed.
  if (audioSources_)
  {
    audioSources_->Unsubscribe();
    audioSources_.reset();
  }
  // The provider must go before the endpoint that owns it.
  controllerProvider_.reset();
  if (onAudioCmdId_ != 0)
  {
    api.ReleaseMsgID(onAudioCmdId_);
    onAudioCmdId_ = 0;
  }
  if (onVpxGameEndId_ != 0)
  {
    api.ReleaseMsgID(onVpxGameEndId_);
    onVpxGameEndId_ = 0;
  }
  if (b2sStateChangeId_ != 0)
  {
    api.ReleaseMsgID(b2sStateChangeId_);
    b2sStateChangeId_ = 0;
  }

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

  // A controller appearing IS the game-start signal now; there is no start
  // event any more. AltSound, PUP, B2S, DOF and Serum all bind by consuming
  // CTLPI_CONTROLLERS_GET_MSG and filtering on the gameId prefix.
  //
  // For a ROM-less game nothing else publishes one, so this is what makes them
  // activate at all. When PinMAME runs as a plugin libpinmame publishes its own
  // and PPUC stays out of the way rather than making the list ambiguous -- see
  // Options::provideController.
  //
  // The prefix is "pinmame::" rather than something PPUC-specific because
  // gameId names *what is emulated and how it is exposed*, not who is
  // emulating it (ControllerPlugin.h). PPUC runs the same ROMs and its
  // AltSound/PUP/B2S assets are keyed by the same names, so this is accurate.
  controllerGameId_ = std::string(PMPI_GAMEID_PREFIX) + gameId_;
  if (!controllerProvider_)
  {
    return;
  }
  controllerProvider_->SetItem({
      .endpointId = bus_.HostEndpointId(),
      .gameId = controllerGameId_.c_str(),
  });
}

void MediaPluginHost::Impl::OnGameEnd()
{
  if (!initialized_ || !gameStarted_)
  {
    return;
  }
  // Withdrawing the controller is the game-end signal.
  if (controllerProvider_)
  {
    controllerProvider_->ClearItems();
  }
  controllerGameId_.clear();
  bus_.Api().BroadcastMsg(bus_.HostEndpointId(), onVpxGameEndId_, nullptr);
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
    pendingEvents_.push_back(PpucPluginEvent{static_cast<uint8_t>(source), id, value});
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
  if (!initialized_ || onAudioCmdId_ == 0)
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
  if (debugAudio_ && audioOutput_ != nullptr)
  {
    const uint64_t nowMs = SDL_GetTicks();
    if (nowMs - lastAudioDebugMs_ >= 1000)
    {
      lastAudioDebugMs_ = nowMs;
      const std::string lanes = audioOutput_->DescribeLanes();
      if (!lanes.empty())
      {
        printf("Audio lanes:\n%s", lanes.c_str());
      }
    }
  }

  std::vector<PpucPluginEvent> events;
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
  for (PpucPluginEvent& event : events)
  {
    if (b2sStateChangeId_ != 0)
    {
      bus_.Api().BroadcastMsg(bus_.HostEndpointId(), b2sStateChangeId_, &event);
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
    // CTLPI_EVT_ON_SOUND_COMMAND is gone; sound commands are a PinMAME-level
    // event now, and that is what AltSound subscribes to.
    PinMAMEChildBoardEventMsg msg{
        .boardNo = static_cast<uint32_t>(boardNo),
        .cmd = static_cast<uint32_t>(cmd),
    };
    bus_.Api().BroadcastMsg(bus_.HostEndpointId(), onAudioCmdId_, &msg);
  }
  // PluginBus::Process() drains async callbacks once per main-loop tick.

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
  bus_.Api().BroadcastMsg(bus_.HostEndpointId(), getAuxRendererId_,
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
  // The backglass takes one renderer, and B2S wins when both are on. That may
  // well be the wrong call for a game that ships a PUP pack -- a pack usually
  // provides the whole backglass -- but silently picking one was the real
  // problem: a pack loads, decodes, produces audio, and never appears, with
  // nothing to say why.
  if (options_.enableB2S && options_.enablePup && !loggedRendererContention_)
  {
    loggedRendererContention_ = true;
    std::printf(
        "Media backglass: B2S and PUP are both enabled; B2S renders the backglass "
        "and the PUP pack's video will not be shown. Disable one of them.\n");
  }
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
      msg->sourceId.endpointId == self->bus_.HostEndpointId())
  {
    return;
  }

  if (msg->buffer == nullptr || msg->bufferSize == 0)
  {
    self->audioOutput_->StopPluginStream(msg->streamId.id);
    return;
  }

  std::vector<int16_t> samples;
  ConvertAudioUpdateToS16(*msg, &samples);
  if (samples.empty())
  {
    return;
  }

  // A source may carry several streams -- PUP runs one per media player -- so
  // the mixer is keyed on streamId. The sourceId rides along because overriding
  // is declared between sources, not streams.
  const int channels =
      msg->channelFormat == CTLPI_AUDIO_FORMAT_CHANNEL_MONO ? 1 : 2;
  self->audioOutput_->QueuePluginSamples(
      msg->sourceId.id, msg->streamId.id, samples.data(), samples.size(),
      static_cast<int>(msg->sampleRate), channels);
}

void MediaPluginHost::Impl::OnAudioSrcChanged()
{
  if (audioOutput_ == nullptr || !audioSources_)
  {
    return;
  }

  std::vector<AudioLanes::Source> lanes = audioSources_->With(
      [](const std::vector<AudioSrcId>& items)
      {
        std::vector<AudioLanes::Source> out;
        out.reserve(items.size());
        for (const AudioSrcId& item : items)
        {
          // The name is copied, not borrowed: it points into the publishing
          // plugin and is only guaranteed valid until the next change event.
          out.push_back({item.id.id, item.overrideId.id,
                         item.name != nullptr ? std::string(item.name) : std::string()});
        }
        return out;
      });

  audioOutput_->SetAudioSources(lanes);

  if (debugAudio_)
  {
    printf("Audio sources (%zu):\n", lanes.size());
    for (const AudioLanes::Source& source : lanes)
    {
      // CtlResId packs endpointId in the low word and resId in the high one.
      printf("  %s [endpoint %u, res %u]\n",
             source.name.empty() ? "(unnamed)" : source.name.c_str(),
             static_cast<unsigned int>(source.id),
             static_cast<unsigned int>(source.id >> 32));
      if (source.overrideId != 0)
      {
        printf("    overrides [endpoint %u, res %u]\n",
               static_cast<unsigned int>(source.overrideId),
               static_cast<unsigned int>(source.overrideId >> 32));
      }
    }
  }
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


bool MediaPluginHost::Impl::EnsureB2SServer()
{
  if (!options_.enableB2S)
  {
    return false;
  }
  if (b2sServer_.IsValid())
  {
    return true;
  }

  const ScriptClassDef* classDef = bus_.ComOverride("B2S.Server");
  if (classDef == nullptr || classDef->CreateObject == nullptr)
  {
    if (!loggedB2SServerUnavailable_)
    {
      std::printf("B2S server is not available from plugin script API\n");
      loggedB2SServerUnavailable_ = true;
    }
    return false;
  }

  if (!b2sServer_.Create(classDef))
  {
    std::printf("B2S server object creation failed\n");
    return false;
  }
  return true;
}

void MediaPluginHost::Impl::ReleaseB2SServer() { b2sServer_.Release(); }

bool MediaPluginHost::Impl::CallB2SMember(
    const char* name, std::initializer_list<const char*> argTypes,
    ScriptVariant* args)
{
  return EnsureB2SServer() && b2sServer_.Call(name, argTypes, args);
}

void MediaPluginHost::Impl::DispatchB2SEvent(const PpucPluginEvent& event)
{
  if (!options_.enableB2S)
  {
    return;
  }
  if (event.type != 'L' && event.type != 'S' && event.type != 'G')
  {
    return;
  }

  ScriptVariant args[2] = {};
  args[0].vInt = event.index;
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

MediaPluginHost::MediaPluginHost(AudioOutput* audioOutput, PluginBus& bus)
    : impl_(std::make_unique<Impl>(audioOutput, bus))
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
