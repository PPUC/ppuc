#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameEngine.h"
#include "PinmameNvramTracking.h"

// The PinMAME-backed game engine: an original ROM running under libpinmame.
//
// This is a thin adapter. All of its behaviour is libpinmame's; what lives here
// is the translation from libpinmame's callback shapes to the GameEngine seam,
// plus the two things the host used to do inline and that are really engine
// concerns: decoding segment-display words into digits, and polling NVRAM for
// the current ball and player.
//
// ONE INSTANCE PER PROCESS. libpinmame keeps its config and user-data pointer in
// file-static globals (PinmameSetUserData is process-wide, not per callback), so
// a second live PinmameEngine would silently steal the first one's callbacks.
// Start() refuses rather than letting that happen.

struct PinmameEngineOptions
{
  std::string rom;
  std::string pinmamePath;      // empty means the per-user PinMAME directory
  uint8_t platform = 0;         // PLATFORM_WPC etc., from PPUC::GetPlatform()
  uint8_t gameOnSolenoid = 0;   // from PPUC::GetGameOnSolenoid()
  bool altsound = false;
  bool noSound = false;
  bool debug = false;
  bool debugCoils = false;
  bool debugSoundCommands = false;
  bool debugErrors = false;
};

// Resolves the PinMAME base directory, with a trailing separator, exactly as
// libpinmame expects it in PinmameConfig::vpmPath. An empty `pinmamePath` means
// the per-user default. Shared because the host needs the same string to locate
// the Serum altcolor folder.
std::string ResolveVpmPath(const std::string& pinmamePath);

class PinmameEngine final : public GameEngine
{
 public:
  explicit PinmameEngine(PinmameEngineOptions options);
  ~PinmameEngine() override;

  PinmameEngine(const PinmameEngine&) = delete;
  PinmameEngine& operator=(const PinmameEngine&) = delete;

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

  // Called by the file-static callback trampolines in PinmameEngine.cpp. Public
  // because the trampolines have C linkage and cannot be friends; not part of
  // the GameEngine contract and not to be called by the host.
  void HandleStateUpdated(int state);
  void HandleSolenoidUpdated(int solNo, int state);
  void HandleDisplayAvailable(int index, int displayCount, const void* pLayout);
  void HandleDisplayUpdated(int index, const void* pData, const void* pLayout);
  int HandleAudioAvailable(const void* pAudioInfo);
  int HandleAudioUpdated(const void* pBuffer, int samples);
  void HandleSoundCommand(int boardNo, int cmd);
  // The log trampoline formats the message itself: libpinmame's log argument is
  // a va_list on some builds and a char* on others, and that distinction cannot
  // survive being erased to void*.
  void EmitLog(bool error, const char* message);
  void HandleMechAvailable(int mechNo, const void* pMechInfo);
  void HandleMechUpdated(int mechNo, const void* pMechInfo);
  void HandleConsoleDataUpdated(int size);

 private:
  void PollTrackedState();
  void PollSoundCommands();
  int GetSegmentDisplayDigitBase(int index, int length);
  void DebugSegmentDisplayUpdate(int index, int type, int base, const uint16_t* segments, int length);

  PinmameEngineOptions m_options;
  GameEngineHost* m_pHost = nullptr;
  bool m_started = false;

  std::atomic<int> m_runState{0};

  // Segment-display bookkeeping. Touched only from the libpinmame thread.
  std::unordered_map<int, int> m_segmentDisplayDigitBases;
  std::unordered_map<int, std::string> m_lastSegmentDisplayDebugLine;
  int m_nextSegmentDisplayDigitBase = 1;

  // Scratch buffers for the pull methods, reused across ticks.
  std::vector<uint8_t> m_lampScratch;
  std::vector<uint8_t> m_giScratch;
  std::vector<uint8_t> m_soundCommandScratch;

  // NVRAM ball/player tracking. Main thread only, driven from Update().
  PinmameTrackingConfig m_tracking;
  bool m_triedLoadingTracking = false;
  uint64_t m_nextTrackedPollMs = 0;
  uint8_t m_lastBall = 0;
  uint8_t m_lastPlayer = 0;
  bool m_hasLastBall = false;
  bool m_hasLastPlayer = false;

  mutable bool m_identityKnown = false;
  mutable Identity m_identity;
};
