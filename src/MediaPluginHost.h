#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AudioOutput.h"

namespace vpxhost
{
class PluginBus;
}
using PluginBus = vpxhost::PluginBus;

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
    // Prints the published audio-source topology and, once a second, per-lane
    // buffered depth. The depth is the gating measurement for this whole
    // design: every plugin audio buffer is marshalled through
    // ProcessAsyncCallbacks on the main loop, and a depth that only grows means
    // that drain cannot keep up with the producers.
    bool debugAudio = false;
    // Whether PPUC declares itself the controller for this game.
    //
    // Only true for ROM-less games. When PinMAME runs as a plugin, libpinmame
    // publishes its own ControllerDef with the identical "pinmame::<rom>" game
    // id, and a second one from PPUC does not add information -- it adds a
    // coin flip. AltSound, PUP, DOF and B2S all bind with items.front() and no
    // tie-break, and AltSound derives the audio source it overrides from
    // whichever it got, so the wrong pick silently leaves the ROM audible
    // underneath the pack.
    bool provideController = true;
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
    // Set from translite_window: the display runs in a window on a desktop
    // rather than owning a cabinet's screen. The only thing it decides here is
    // the mouse cursor, which a desktop user still wants and a cabinet has no
    // use for.
    bool windowed = false;
    float b2sSegmentAngleDegrees = 9.0f;
    float b2sSegmentGlow = 1.4f;
    bool b2sSegmentSmoothing = true;
  };

  MediaPluginHost(AudioOutput* audioOutput, PluginBus& bus);
  ~MediaPluginHost();

  bool Initialize(const Options& options, std::string* errorMessage);
  void Shutdown();
  void SetGameInfo(const char* gameId, uint64_t hardwareGen);
  void OnGameStart();
  void OnGameEnd();
  void QueueEvent(char source, int id, int value);

  // Drawn on top of the backglass, in the same frame, by whoever set it.
  //
  // The service overlay used to open a fullscreen window of its own. On KMSDRM
  // there is no compositor and no overlapping: two windows both presenting
  // means the panel alternates between them as fast as they draw, which is
  // exactly what it looked like. One window, one present, and the overlay is
  // simply the last thing drawn into it.
  using OverlayDraw = std::function<void(SDL_Renderer*, int, int)>;
  void SetOverlayDraw(OverlayDraw draw);
  // Whether there is a backglass window to draw into at all. A machine with a
  // translite, or with no video, has none and the overlay opens its own.
  bool HasBackglass() const;
  // The window the backglass is presented in, or null. Only for asking where
  // it is: whoever else draws to the same screen has to stand aside while the
  // overlay is up, and that is decided by whether the windows overlap.
  SDL_Window* BackglassWindow() const;
  void QueueSegmentDisplay(int digit, int value);
  void QueuePlayerScore(int player, int score);
  void QueueDmdTrigger(uint16_t id);
  // Asks a Serum colorizer to play a scene, on its own message.
  void TriggerSerumScene(uint16_t id);
  // One raw byte from a sound board, passed on to AltSound as it is.
  void OnSoundCommand(int boardNo, int cmd);
  // A whole sound command from a rule (ppuc.soundCommand), with the number a
  // pack is keyed by. It is split into the bytes a ROM of the game's hardware
  // generation would write -- two for WPC DCS / WPC-S / WPC95 -- so AltSound
  // reassembles exactly that number. With a ROM running, a rule's pair waits
  // while the ROM is between the two bytes of its own. Returns false, with the
  // reason, when the generation cannot express the number.
  bool SendSoundCommand(int boardNo, uint32_t cmd, std::string* error);
  void Process();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
