#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AudioLanes.h"
#include "AudioMixer.h"
#include "SDL3/SDL.h"

#if defined(PPUC_HAS_SDL3_MIXER)
struct MIX_Audio;
struct MIX_Mixer;
struct MIX_Track;
#endif

class AudioOutput
{
 public:
  AudioOutput() = default;
  ~AudioOutput();

  bool Initialize();
  void Shutdown();

  void ConfigureGameFormat(int frequency, int channels);
  bool LoadMusicFilesCsv(const char* csv, std::string* errorMessage);
  void SetMusicTrackGapMs(Uint64 gapMs);
  void SetMusicEnabled(bool enabled);
  void QueueGameFrames(const int16_t* samples, size_t frameCount);
  void QueuePluginSamples(uint64_t sourceId, uint64_t streamId, const int16_t* samples, size_t sampleCount,
                          int frequency, int channels);
  void StopPluginStream(uint64_t streamId);

  // Replaces the published audio-source topology. Called from the plugin bus
  // whenever OnAudioSrcChanged fires.
  void SetAudioSources(const std::vector<AudioLanes::Source>& sources);
  void SetOverrideMode(AudioLanes::OverrideMode mode);
  void SetFallbackHoldMs(uint64_t holdMs);
  // One line per lane: buffered depth and whether it is currently heard. A
  // depth that grows without bound means the bus callbacks are draining slower
  // than the producers fill them.
  std::string DescribeLanes() const;
  // Reports trimming, which is otherwise invisible.
  void SetDebugAudio(bool debug) { debugAudio_ = debug; }
  void QueuePluginSamples(const int16_t* samples, size_t sampleCount, int frequency, int channels);
  void QueueSpeechSamples(const int16_t* samples, size_t sampleCount, int frequency, int channels);

 public:
  struct MusicTrack
  {
    std::string path;
#if defined(PPUC_HAS_SDL3_MIXER)
    MIX_Audio* audio = nullptr;
#endif
  };

 private:
  static void SDLCALL OnDeviceNeedsAudio(void* userdata, SDL_AudioStream* stream, int additionalAmount,
                                         int totalAmount);
#if defined(PPUC_HAS_SDL3_MIXER)
  static void SDLCALL OnMusicTrackStopped(void* userdata, MIX_Track* track);
#endif

  void EnsureStreamLocked(const SDL_AudioSpec& spec);
  // Converts to the device format if needed, then hands off to AudioMixer.
  // Format conversion state for one producer.
  //
  // Stateful on purpose. SDL_ConvertAudioSamples is a one-shot: it resamples a
  // buffer with no memory of the last one, so a producer sending small buffers
  // at a rate that does not divide the device rate loses the fraction on every
  // call and the error accumulates. AltSound sends 128 frames at 44100 Hz,
  // which is 139.32 frames at 48000 -- rounded up on every buffer, that is
  // half a percent of surplus audio forever, and its queue grows without bound
  // until the overflow cap starts dropping sound. PinMAME never showed it
  // because its 735-frame buffers convert to exactly 800.
  //
  // An SDL_AudioStream carries the resampler's fractional position across
  // calls, so nothing accumulates.
  struct Resampler
  {
    SDL_AudioStream* stream = nullptr;
    int frequency = 0;
    int channels = 0;
    float ratio = 1.0f;
  };
  // `latencyManaged` trims and rate-steers the queue to hold latency down.
  // Right for a continuous stream whose producer runs in real time - ROM audio,
  // AltSound - where a backlog is latency and dropping it is the lesser harm.
  //
  // Wrong for speech. An utterance is synthesised ahead of time and queued in
  // one block, seconds of it at once, so trimming to the high-water mark throws
  // away everything after the first fraction of a second and leaves a noise
  // where a sentence should be. There is no latency to manage either: nothing
  // is waiting on it to stay in sync.
  void QueueSamplesLocked(AudioMixer::Queue& queue, Resampler& resampler, const int16_t* samples, size_t sampleCount,
                          int frequency, int channels, bool latencyManaged);
  static void DestroyResampler(Resampler& resampler);
  // Keeps a lane from sitting further ahead of the device than
  // kTargetBufferedMs once a stall has pushed it there.
  void TrimQueueLocked(AudioMixer::Queue& queue);
  // Nudges a lane's resampling ratio so its depth converges on the target,
  // rather than drifting with the producer's clock or staying wherever a stall
  // left it.
  void SteerQueueLocked(const AudioMixer::Queue& queue, Resampler& resampler);
  size_t TargetBufferedSamplesLocked() const;
  void MixMusicLocked(int16_t* mixBuffer, size_t sampleCount, bool duckToBackground);
#if defined(PPUC_HAS_SDL3_MIXER)
  bool EnsureMusicMixerLocked(std::string* errorMessage);
  void DestroyMusicTracksLocked();
  bool ReloadMusicTracksLocked(std::string* errorMessage);
  bool StartCurrentMusicTrackLocked();
  void ScheduleNextMusicTrackLocked();
  void AdvanceMusicTrackLocked();
  void HandleMusicTrackStoppedLocked(MIX_Track* track);
#endif

  mutable std::mutex mutex_;
  bool debugAudio_ = false;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioSpec deviceSpec_{
      .format = SDL_AUDIO_S16LE,
      .channels = 2,
      .freq = 48000,
  };
  int gameFrequency_ = 48000;
  int gameChannels_ = 2;
  // PPUC's own audio, from GameEngine::OnAudioFrames. Only ScriptEngine feeds
  // it: under PluginEngine the ROM stream arrives over the bus like any other
  // plugin's, so it is a lane rather than this queue. Deliberately not part of
  // the lane table -- nothing publishes an override against it.
  AudioMixer::Queue gameQueue_;
  Resampler gameResampler_;
  // One entry per CTLPI audio stream, tagged with the source it belongs to. A
  // source may own several streams; overriding is decided per source.
  struct PluginStream
  {
    uint64_t sourceId = 0;
    AudioMixer::Queue queue;
    Resampler resampler;
    // When this stream was last fed, for the delivery-gap report. A lane that
    // suddenly holds a quarter of a second of audio either ran fast or was
    // starved and then handed the backlog in one go, and only the interval
    // between deliveries tells those apart.
    uint64_t lastQueuedMs = 0;
  };
  std::unordered_map<uint64_t, PluginStream> pluginStreams_;
  AudioLanes::Table lanes_;
  AudioMixer::Queue speechQueue_;
  Resampler speechResampler_;
  std::vector<MusicTrack> musicTracks_;
#if defined(PPUC_HAS_SDL3_MIXER)
  MIX_Mixer* musicMixer_ = nullptr;
  MIX_Track* musicTrack_ = nullptr;
  bool musicTrackStartPending_ = false;
  Uint64 musicTrackStartTickMs_ = 0;
  Uint64 musicTrackGapMs_ = 2000;
#endif
  size_t musicTrackIndex_ = 0;
  bool musicEnabled_ = false;
  float musicGain_ = 0.0f;
};
