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

  // The playlist, for anything that lets a player choose from it.
  //
  // SelectMusicTrack starts the track straight away when the music is playing,
  // because a chooser you cannot hear is not a chooser.
  // The title and credit the config tool exported for a track, if it did.
  // Matched by filename, because that is the only thing the folder and the
  // sidecar have in common.
  void SetMusicTrackInfo(const std::string& fileName, const std::string& title, const std::string& attribution);
  std::string GetMusicTrackTitle(size_t index) const;
  std::string GetMusicTrackAttribution(size_t index) const;

  size_t GetMusicTrackCount() const;
  size_t GetMusicTrackIndex() const;
  std::string GetMusicTrackName(size_t index) const;
  void SelectMusicTrack(size_t index);

  // Step to the next track without playing it.
  //
  // Called when a game ends, and deliberately not from SetMusicEnabled(false):
  // a service test also silences the music, and coming back from a test must
  // return to the song that was playing rather than skip it. Only the end of a
  // game moves the playlist on, which is what stops every game opening with the
  // same song -- a machine with four tracks used to play the first one, and only
  // the first one, until somebody restarted it.
  void AdvanceMusicTrack();
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

  // Per-source levels, each 0.0 to 1.0, master applied on top of the others.
  //
  // What "game" covers is everything the emulator and its packs produce: the
  // ROM stream, AltSound replacing it, and today PUP video audio as well. They
  // are one control because they are one thing to a player -- the sound of the
  // game -- and because the override chain already treats a pack and the ROM it
  // replaces as interchangeable. A pack that wants its own control would need a
  // lane category of its own, which is a change to the lane table rather than
  // to this.
  //
  // Speech is PPUC's own text-to-speech, music the background tracks. Neither
  // comes from the game, and both are routinely wanted at a different level
  // from it, which is the whole reason this exists.
  void SetVolumes(float master, float game, float speech, float music);

  // How loud the music sits under the game's own sound, 0..1 of its normal
  // level. The default is deep -- the music is background -- but how deep is a
  // matter of taste and of the cabinet's speakers, and it is the one number
  // that decides whether the music can be heard at all while a ball is in play.
  void SetMusicDuck(float duck);

 public:
  struct MusicTrack
  {
    std::string path;
    // From music/tracks.yaml, where the config tool writes what it knows. Empty
    // when there is no entry for this file, and the filename stands in.
    std::string title;
    std::string attribution;
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
  // Everything full up, so a host that never calls SetVolumes sounds exactly as
  // it did before there were levels at all.
  float masterVolume_ = 1.0f;
  float gameVolume_ = 1.0f;
  float speechVolume_ = 1.0f;
  float musicVolume_ = 1.0f;
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
  // True only while a track is deliberately being swapped in. SDL_mixer reports
  // the outgoing track as stopped when that happens, and the stopped callback's
  // job is to move the playlist on -- which would step past the very track being
  // started. The callback ignores a stop it caused itself.
  bool musicRestarting_ = false;
  bool musicTrackStartPending_ = false;
  Uint64 musicTrackStartTickMs_ = 0;
  Uint64 musicTrackGapMs_ = 2000;
#endif
  size_t musicTrackIndex_ = 0;
  bool musicEnabled_ = false;
  // How far the music drops under the game's own sound, as a fraction of its
  // normal level.
  //
  // This started as an absolute gain of 0.08 against a base of 0.28, which is
  // 0.286 of normal -- so that is the default, and the machine sounds exactly
  // as it did. Expressed as a fraction because that is the question somebody
  // at the machine is actually asking: how much quieter should the music be
  // while a ball is in play.
  float musicDuck_ = 0.286f;
  float musicGain_ = 0.0f;
};
