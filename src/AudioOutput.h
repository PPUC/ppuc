#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

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
  void QueueSpeechSamples(const int16_t* samples, size_t sampleCount,
                         int frequency, int channels);

private:
  struct PendingBuffer
  {
    std::vector<int16_t> samples;
    size_t offsetSamples = 0;
  };

public:
  struct MusicTrack
  {
    std::string path;
#if defined(PPUC_HAS_SDL3_MIXER)
    MIX_Audio* audio = nullptr;
#endif
  };

private:

  static void SDLCALL OnDeviceNeedsAudio(void* userdata,
                                         SDL_AudioStream* stream,
                                         int additionalAmount,
                                         int totalAmount);
#if defined(PPUC_HAS_SDL3_MIXER)
  static void SDLCALL OnMusicTrackStopped(void* userdata, MIX_Track* track);
#endif

  void EnsureStreamLocked(const SDL_AudioSpec& spec);
  void QueueSamplesLocked(std::deque<PendingBuffer>& queue,
                          const int16_t* samples, size_t sampleCount,
                          int frequency, int channels);
  bool MixQueueLocked(std::deque<PendingBuffer>& queue,
                      int16_t* mixBuffer, size_t sampleCount);
  void MixMusicLocked(int16_t* mixBuffer, size_t sampleCount,
                      bool duckToBackground);
#if defined(PPUC_HAS_SDL3_MIXER)
  bool EnsureMusicMixerLocked(std::string* errorMessage);
  void DestroyMusicTracksLocked();
  bool ReloadMusicTracksLocked(std::string* errorMessage);
  bool StartCurrentMusicTrackLocked();
  void ScheduleNextMusicTrackLocked();
  void AdvanceMusicTrackLocked();
  void HandleMusicTrackStoppedLocked(MIX_Track* track);
#endif

  std::mutex mutex_;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioSpec deviceSpec_{
      .format = SDL_AUDIO_S16LE,
      .channels = 2,
      .freq = 48000,
  };
  int gameFrequency_ = 48000;
  int gameChannels_ = 2;
  std::deque<PendingBuffer> gameQueue_;
  std::deque<PendingBuffer> speechQueue_;
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
