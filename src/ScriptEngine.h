#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GameEngine.h"
#include "dmd/DmdCanvas.h"
#include "dmd/DmdDefaultScreen.h"
#include "dmd/DmdRenderer.h"
#include "game/GameCore.h"

// The ROM-less engine: an electro-mechanical machine driven by GameCore and the
// DMD layer instead of by a game ROM.
//
// No thread. Start() loads configuration and reports ready but emits nothing;
// everything happens in Update(), on the main thread. That makes every sink
// invocation a strict subset of the contract libpinmame already imposes on the
// host, which is why all the shared subsystems -- the interceptor, ball search,
// the media host, translite -- work unchanged.

class ScriptEngine final : public GameEngine
{
 public:
  struct Options
  {
    std::string gameId;   // asset namespace; the game folder name when no ROM
    uint16_t dmdWidth = 128;
    uint16_t dmdHeight = 32;
    uint8_t dmdColorR = 255;
    uint8_t dmdColorG = 120;
    uint8_t dmdColorB = 0;
    bool debug = false;
  };

  // How GameCore's actions reach the machine. Injected rather than taking a
  // PPUC* so ScriptEngine stays testable and libppuc-free.
  struct MachineIo
  {
    std::function<void(int number, uint32_t durationMs)> pulseCoil;
    std::function<void(int number, uint8_t state)> setCoil;
    std::function<void(int number, uint8_t state)> setLamp;
    std::function<void(int string, uint8_t level)> setGi;
    // Asserts or releases the host-owned tilt inhibit switch. May be absent when
    // the machine has no inhibit switch configured, in which case tilt still
    // suppresses scoring but cannot drop a held flipper.
    std::function<void(int number, uint8_t state)> setTiltInhibit;
    std::function<void()> requestBallSearch;
  };

  ScriptEngine(Options options, GameConfig gameConfig, MachineIo io);
  ~ScriptEngine() override;

  // GameEngine
  void SetHost(GameEngineHost* pHost) override;
  bool Start(std::string& error) override;
  void Stop() override;
  bool IsReady() const override;
  bool TryGetIdentity(Identity* pIdentity) const override;
  void Update() override;
  void PollChangedLamps(std::vector<GameEngineOutputChange>& changes) override;
  void PollChangedGis(std::vector<GameEngineOutputChange>& changes) override;
  void SendSwitch(int number, uint8_t state) override;
  bool HasCapability(Capability capability) const override;

  // Exposed so the Lua layer and the host can reach the game and the display.
  GameCore& Game() { return m_game; }
  DmdCanvas& Canvas() { return m_canvas; }
  DmdDefaultScreen& DefaultScreen() { return m_defaultScreen; }

  // Invoked once per GameCore event, on the main thread, after the event has
  // been applied. This is where ScriptEngine hands events to Lua.
  using EventFn = std::function<void(const GameEvent&)>;
  void SetEventCallback(EventFn callback) { m_onEvent = std::move(callback); }

  // Drawn on top of the built-in screen on ticks that will actually flush.
  void SetDmdDrawCallback(std::function<void()> draw);
  void SetBuiltinScreenEnabled(bool enabled) { m_builtinScreen = enabled; }

 private:
  void ApplyActions();
  void DispatchEvents();

  Options m_options;
  MachineIo m_io;
  GameEngineHost* m_pHost = nullptr;
  GameCore m_game;
  DmdCanvas m_canvas;
  DmdRenderer m_renderer;
  DmdDefaultScreen m_defaultScreen;
  std::unique_ptr<DmdSink> m_sink;
  EventFn m_onEvent;
  std::function<void()> m_luaDraw;
  bool m_builtinScreen = true;
  bool m_ready = false;
  bool m_gameRunning = false;
  uint8_t m_lastBall = 0;
  uint8_t m_lastPlayer = 0;
};
