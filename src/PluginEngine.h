#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GameEngine.h"
#include "PinmameNvramTracking.h"
#include "ScriptObject.h"

class PluginBus;

// The PinMAME-backed game engine, driven as a plugin on the message bus.
//
// Replaces PinmameEngine. libpinmame no longer runs inside this process: it
// runs behind plugin-pinmame, which publishes machine state on the bus. The
// change-notification hook the old API had is gone, so state is polled.
//
// THREADING. Two threads touch PinMAME state, and which one touches what is a
// correctness constraint, not a preference:
//
//  * A dedicated poll thread reads ONLY the solenoid group, at kCoilPollHz.
//    Coil edges are the one latency-critical path, and GameEngine.h requires
//    them to be pushed rather than wait for a host tick.
//  * The main thread reads everything else -- lamps, GI, displays, segments --
//    and is the only one that calls SetState.
//
// They must not overlap. libpinmame integrates PWM output on read, mutating
// per-output state with no lock of its own (wpc/core.c: the load is moved to
// the caller deliberately), so two threads reading the same output corrupts it.
// "GetState is thread safe" in ControllerPlugin.h means safe against the
// emulation thread, not against two concurrent readers of one output.
class PluginEngine final : public GameEngine
{
 public:
  struct Options
  {
    std::string rom;
    std::string pinmamePath;
    std::string pluginId = "PinMAME";
    uint8_t platform = 0;
    uint8_t gameOnSolenoid = 0;
    bool noSound = false;
    bool debug = false;
    bool debugCoils = false;
    bool debugSegments = false;
    bool debugSoundCommands = false;
    int coilPollHz = 1000;
    int outputPollHz = 120;
  };

  PluginEngine(PluginBus& bus, Options options);
  ~PluginEngine() override;

  PluginEngine(const PluginEngine&) = delete;
  PluginEngine& operator=(const PluginEngine&) = delete;

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

 private:
  // One output, resolved to the provider's own accessor. Borrowed from the
  // provider and valid only while the quiesce gate is open.
  struct OutputEntry
  {
    uint16_t number = 0;
    void* context = nullptr;
    void (*Get)(void*, void*) = nullptr;
  };

  struct SwitchEntry
  {
    void* context = nullptr;
    void (*Set)(void*, const void*) = nullptr;
  };

  // The solenoid accessors the poll thread reads, republished as a unit
  // whenever the provider's source list changes.
  struct CoilPlan
  {
    uint32_t generation = 0;
    std::vector<OutputEntry> entries;
  };

  void OnStateSrcAboutToChange();
  void OnStateSrcChanged();
  void OnSegSrcChanged();
  void SampleSegments();
  void OnControllersChanged();
  void PollThreadMain();
  void SampleOutputs();
  void PollTrackedState();

  PluginBus& m_bus;
  Options m_options;
  GameEngineHost* m_pHost = nullptr;
  bool m_started = false;
  bool m_stopping = false;

  uint32_t m_pinmameEndpoint = 0;
  unsigned int m_getMachineStateId = 0;
  unsigned int m_readMemoryId = 0;
  unsigned int m_onAudioCmdId = 0;

  ScriptObject m_controller;

  // Main-thread output maps, rebuilt on every source change.
  std::vector<OutputEntry> m_lamps;
  std::vector<OutputEntry> m_gis;
  std::unordered_map<int, SwitchEntry> m_switchesByNumber;
  std::vector<uint8_t> m_lastLamp;
  std::vector<uint8_t> m_lastGi;
  std::vector<GameEngineOutputChange> m_lampChanges;
  std::vector<GameEngineOutputChange> m_giChanges;
  uint64_t m_nextOutputSampleMs = 0;

  // Segment displays live in Impl, not here: their accessor is a
  // SegSrcId::GetState function pointer returning a struct by value, and
  // retyping that to keep the plugin SDK out of this header would be an ABI
  // gamble for no benefit.
  //
  // They are polled from Update() on the main thread, and are safe there
  // without the coil gate: libpinmame publishes its sources from OnGameStart
  // via RunOnMainThread, so a source change and a poll cannot overlap. That is
  // the invariant this borrows -- if segment polling ever moves off the main
  // thread it needs a gate of its own, exactly like the solenoids.
  uint64_t m_nextSegmentSampleMs = 0;

  // The coil plan and the gate that keeps the poll thread out of provider
  // memory while the provider is rebuilding it.
  std::unique_ptr<CoilPlan> m_coilPlan;
  std::atomic<bool> m_gateOpen{false};
  std::atomic<unsigned> m_gateActive{0};
  std::atomic<bool> m_stopRequested{false};
  std::thread m_pollThread;

  std::atomic<int> m_runState{0};
  mutable bool m_identityKnown = false;
  mutable Identity m_identity;

  PinmameTrackingConfig m_tracking;
  bool m_triedLoadingTracking = false;
  uint64_t m_nextTrackedPollMs = 0;
  uint8_t m_lastBall = 0;
  uint8_t m_lastPlayer = 0;
  bool m_hasLastBall = false;
  bool m_hasLastPlayer = false;

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
