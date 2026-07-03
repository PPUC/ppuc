#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioOutput.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

class MediaPluginHost
{
public:
  struct Options
  {
    bool enablePup = false;
    bool enableAltSound = false;
    bool enableB2S = false;
    bool debug = false;
    const char* pluginDir = nullptr;
    const char* pupFolder = nullptr;
    const char* altSoundFolder = nullptr;
    const char* tablePath = nullptr;
    const char* prefPath = nullptr;
    const char* gameId = nullptr;
    uint64_t hardwareGen = 0;
    int backglassWidth = 1920;
    int backglassHeight = 1080;
    int backglassScreen = 0;
    float b2sSegmentAngleDegrees = 9.0f;
    float b2sSegmentGlow = 1.4f;
    bool b2sSegmentSmoothing = true;
  };

  explicit MediaPluginHost(AudioOutput* audioOutput);
  ~MediaPluginHost();

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
  class Impl;
  std::unique_ptr<Impl> impl_;
};
