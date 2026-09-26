#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
  bool SetPaused(bool paused) override;
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
    // Read back for the audit below. May be null, in which case the audit
    // writes without comparing.
    void (*Get)(void*, void*) = nullptr;
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
  void OnDisplaySrcChanged();
  void SampleDmd();
  void ReportDmdRate();
  void OnControllersChanged();
  void PollThreadMain();
  void SampleOutputs();
  void PollTrackedState();
  void ReportUndecodableTrackedField(const char* what, const PinmameTrackedField& field, const PinmameByteReader& read,
                                     bool& alreadyReported);

  PluginBus& m_bus;
  Options m_options;
  GameEngineHost* m_pHost = nullptr;
  bool m_started = false;
  bool m_stopping = false;

  uint32_t m_pinmameEndpoint = 0;
  unsigned int m_getMachineStateId = 0;
  unsigned int m_readMemoryId = 0;
  unsigned int m_onAudioCmdId = 0;
  bool m_subscribedAudioCmd = false;

  ScriptObject m_controller;

  // Main-thread output maps, rebuilt on every source change.
  std::vector<OutputEntry> m_lamps;
  std::vector<OutputEntry> m_gis;
  std::unordered_map<int, SwitchEntry> m_switchesByNumber;

  // What this engine was last told each switch is, and the periodic check that
  // the engine still agrees.
  //
  // Switch state is forwarded on change only -- libppuc reports a difference
  // against its own bitmap, and we pass that on. So if the engine's view of a
  // switch is ever lost or never arrives, nothing puts it back: our side
  // already believes it has told the ROM. A ball resting on the outhole switch
  // with PinMAME believing it open is a game that will not continue, and the
  // only cure was to lift the ball out and drop it back to make a fresh edge.
  //
  // The boards already repair the host's view this way, re-sending their full
  // switch bitmap even when nothing has changed. This is the same idea one
  // level up.
  std::unordered_map<int, uint8_t> m_sentSwitchValues;
  // Switches the ledger holds that the engine has never been told, because
  // there was no accessor for them yet. The audit delivers these quietly: the
  // engine is not disagreeing with us, it simply has not heard us, and counting
  // that as a correction would bury the divergences worth reading about.
  std::unordered_set<int> m_unsentSwitches;
  uint64_t m_nextSwitchAuditMs = 0;
  uint32_t m_switchCorrections = 0;
  void AuditSwitches();
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
  uint64_t m_nextDmdSampleMs = 0;

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
  // Whether the "this field does not decode" line has already been printed for
  // the current spell. Cleared as soon as the field decodes again, so a ROM that
  // recovers says so the next time it stops.
  bool m_undecodableBall = false;
  bool m_undecodablePlayer = false;
  // Raised by the poll thread when a game ends, so the tracked-state poll
  // announces the ball and the player as zero through the same path as any other
  // change -- keeping the cache and the rules in agreement.
  std::atomic<bool> m_trackingResetPending{false};

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
