#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A "game engine" is whatever decides what the machine does: PinMAME running an
// original ROM, or the ROM-less game core that drives an electro-mechanical
// machine from configuration and Lua rules. Everything else in ppuc-pinmame --
// libppuc I/O, LuaRulesEngine, the interceptor, ball search, media plugins,
// audio, DMD, translite -- is engine-neutral and sits on the host side of this
// seam.
//
// This header deliberately depends on nothing but the standard library. No
// libpinmame, no PPUC.h, no DMDUtil. That is what lets ppuc_tests link against
// engine-facing code without dragging in the hardware stack.
//
// THREADING CONTRACT. Read this before implementing either side.
//
//  * The host owns the main loop and calls SetHost/Start/Stop/Update/Poll*/
//    SendSwitch/IsReady/TryGetIdentity/HasCapability from the main thread only.
//  * GameEngineHost sinks MAY be invoked from a thread the engine owns.
//    PinmameEngine invokes them from the libpinmame game thread. ScriptEngine
//    invokes them from inside Update(), i.e. on the main thread. Host
//    implementations must therefore be safe against invocation concurrent with
//    the main loop; the ScriptEngine case is strictly weaker, so host code
//    written for the PinMAME contract is automatically correct for the ROM-less
//    engine.
//  * Sinks must not block and must not call back into the engine.
//  * The engine must not emit machine outputs synchronously from inside
//    Start(); the host has not necessarily called PPUC::StartUpdates() yet.
//    Emit from Update() or from the engine's own thread once Start() returned.

struct GameEngineOutputChange
{
  uint16_t number = 0;
  uint8_t value = 0;
};

class GameEngineHost
{
 public:
  virtual ~GameEngineHost() = default;

  // ---- Machine outputs --------------------------------------------------
  // PUSH. A coil edge must reach the boards with the latency of the producing
  // thread, not the latency of the next host tick. Queueing it onto the main
  // loop would add one MAIN_LOOP_SLEEP_US plus a full iteration of SDL polling
  // and MediaPluginHost::Process -- not acceptable on a playfield.
  //
  // The host implementation is the single place that fans this out to libppuc,
  // the coil/GI mappings, MediaPluginHost and LuaRulesEngine.
  virtual void OnCoilChanged(uint16_t number, uint8_t state) = 0;

  // PUSH, and deliberately separate from OnCoilChanged(gameOnSolenoid, ...).
  // The game-on solenoid means two different things that PinMAME conflates:
  // "high power is live on the boards" (a hardware gate) and "a game is in
  // progress" (attract vs play: music, translite, ball search, Lua attract
  // mode). PinmameEngine emits both together, exactly as it does today.
  // ScriptEngine needs them apart, because an electro-mechanical machine wants
  // high power live during attract so the attract lamps and GI work.
  //
  // When one coil edge carries both meanings, this is emitted BEFORE the
  // matching OnCoilChanged, so that a rules handler reacting to the coil already
  // sees the new attract state.
  virtual void OnGameRunningChanged(bool running) = 0;

  // ---- Run state --------------------------------------------------------
  // PUSH. state == 0 means the engine has stopped and the host should shut
  // down; anything else means the engine is up.
  virtual void OnRunStateChanged(int state) = 0;

  // ---- Displays ---------------------------------------------------------
  // PUSH. Frames go straight into DMDUtil, which owns its own queueing.
  // Segment displays are decoded on the engine side -- the 16-bit segment word
  // layout is PinMAME's data format, not a host concern -- and cross the seam
  // already reduced to "digit N shows value V".
  virtual void OnDmdFrame(const uint8_t* pData, int depth, int width, int height) = 0;
  virtual void OnSegmentDigit(int digit, int value) = 0;
  virtual void OnPlayerScore(int player, int score) = 0;

  // ---- Audio ------------------------------------------------------------
  // PUSH. libpinmame's audio callback contract is "return the number of samples
  // per frame you want", so the format sink has to be able to answer. Returns
  // samplesPerFrame.
  virtual int OnAudioFormat(int sampleRate, int channels, int samplesPerFrame) = 0;
  virtual void OnAudioFrames(const int16_t* pSamples, int samples) = 0;

  // PUSH. Consumed by MediaPluginHost (AltSound) and the sound-command debug
  // dump. boardNo == -1 means "polled, board unknown".
  virtual void OnSoundCommand(int boardNo, int cmd) = 0;

  // ---- Tracked game state -----------------------------------------------
  // PUSH, edge triggered. Both engines discover ball/player on their own
  // cadence -- PinmameEngine by decoding NVRAM on a timer, ScriptEngine because
  // it owns the value -- and the host does nothing with it but forward to
  // LuaRulesEngine. A pull would force the host to learn each engine's cadence
  // for no benefit. Engines call these only when the value actually changed and
  // only when it is known; on a failed decode the engine stays silent and the
  // last value stands.
  virtual void OnCurrentBallChanged(uint8_t ball) = 0;
  virtual void OnCurrentPlayerChanged(uint8_t player) = 0;

  // ---- Logging ----------------------------------------------------------
  // PUSH. Already formatted; the engine owns the vsnprintf.
  virtual void OnLogMessage(bool error, const char* message) = 0;
};

class GameEngine
{
 public:
  struct Identity
  {
    std::string name;          // ROM name, or the ROM-less game id
    std::string description;   // human readable, for the startup log line
    uint64_t hardwareGen = 0;  // PINMAME_HARDWARE_GEN; 0 when ROM-less
  };

  enum class Capability
  {
    // PollChangedGis returns anything. PinmameEngine reports false for non-WPC
    // platforms, where PinMAME's GI is unusable and libppuc forces GI on
    // instead. This replaces the PLATFORM_WPC test in the main loop.
    ChangedGis,
    // The engine reports ball/player. PinmameEngine only once an NVRAM map was
    // found for the ROM; ScriptEngine always.
    TracksBallAndPlayer,
    // The engine produces an audio stream through OnAudioFormat/OnAudioFrames.
    AudioStream,
    // The engine drives segment displays through OnSegmentDigit.
    SegmentDisplays,
  };

  virtual ~GameEngine() = default;

  // Called exactly once before Start(). The host guarantees *pHost outlives the
  // engine.
  virtual void SetHost(GameEngineHost* pHost) = 0;

  // Starts the engine, spawning whatever thread it needs. Returns false and
  // fills `error` on failure; the host then aborts startup.
  virtual bool Start(std::string& error) = 0;

  // Stops the engine and joins any thread it owns. Must be safe to call when
  // Start() failed or was never called, and must be called by the host BEFORE
  // it tears down anything the sinks touch (libppuc, DMD, audio, media).
  virtual void Stop() = 0;

  // The main-loop gate. Until this is true the host must not read switches or
  // forward them to the engine; it still runs its own engine-neutral services
  // (rules, interceptor, media).
  virtual bool IsReady() const = 0;

  // Fills *pIdentity and returns true once the engine knows what it is running.
  // Deliberately separate from IsReady(): under PinMAME, the hardware
  // generation becoming known and the run state going non-zero are two
  // different events, and the main loop treats them independently. Idempotent;
  // the host calls it until it returns true, then stops.
  virtual bool TryGetIdentity(Identity* pIdentity) const = 0;

  // One host tick. Everything the engine must do on the main thread lives here:
  // PinmameEngine polls NVRAM tracking and drains sound commands; ScriptEngine
  // runs its whole game core. Called in BOTH main-loop phases, before and after
  // IsReady() turns true.
  virtual void Update() = 0;

  // PULL, once per host tick. PinMAME exposes lamps and GI only as "what
  // changed since you last asked", so a pull keeps that call inside the host's
  // existing cadence instead of inventing a second one; and lamps are not
  // latency critical the way coils are. Implementations clear and refill the
  // vector, which the host reuses across ticks so there is no steady-state
  // allocation.
  virtual void PollChangedLamps(std::vector<GameEngineOutputChange>& changes) = 0;
  virtual void PollChangedGis(std::vector<GameEngineOutputChange>& changes) = 0;

  // The only host -> engine path. `number` is the PPUC switch number from the
  // game YAML; the engine applies its own numbering convention. State is 0 or 1.
  virtual void SendSwitch(int number, uint8_t state) = 0;

  virtual bool HasCapability(Capability capability) const = 0;
};
