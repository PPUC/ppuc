#include "AudioOutput.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

#include "AudioMixer.h"

#if defined(PPUC_HAS_SDL3_MIXER)
#include "SDL3_mixer/SDL_mixer.h"
#endif

namespace
{
constexpr float kMusicBaseGain = 0.28f;
constexpr float kMusicDuckGain = 0.08f;
constexpr float kMusicAttackPerSample = 0.00012f;
constexpr float kMusicReleasePerSample = 0.00003f;

std::string Trim(const std::string& input)
{
  const size_t first = input.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
  {
    return {};
  }

  const size_t last = input.find_last_not_of(" \t\r\n");
  return input.substr(first, last - first + 1);
}
}  // namespace

AudioOutput::~AudioOutput() { Shutdown(); }

bool AudioOutput::Initialize()
{
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureStreamLocked(deviceSpec_);
  if (stream_ == nullptr)
  {
    return false;
  }

#if defined(PPUC_HAS_SDL3_MIXER)
  std::string errorMessage;
  if (!EnsureMusicMixerLocked(&errorMessage))
  {
    SDL_SetError("%s", errorMessage.c_str());
    return false;
  }
#endif

  return true;
}

void AudioOutput::Shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  gameQueue_.clear();
  DestroyResampler(gameResampler_);
  for (auto& entry : pluginStreams_)
  {
    DestroyResampler(entry.second.resampler);
  }
  pluginStreams_.clear();
  speechQueue_.clear();
  DestroyResampler(speechResampler_);
#if defined(PPUC_HAS_SDL3_MIXER)
  DestroyMusicTracksLocked();
  if (musicTrack_ != nullptr)
  {
    MIX_DestroyTrack(musicTrack_);
    musicTrack_ = nullptr;
  }
  if (musicMixer_ != nullptr)
  {
    MIX_DestroyMixer(musicMixer_);
    musicMixer_ = nullptr;
  }
  MIX_Quit();
#else
  musicTracks_.clear();
#endif
  musicTrackIndex_ = 0;
  musicEnabled_ = false;
  musicGain_ = 0.0f;
  if (stream_ != nullptr)
  {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void AudioOutput::ConfigureGameFormat(int frequency, int channels)
{
  if (frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  gameFrequency_ = frequency;
  gameChannels_ = channels;
}

bool AudioOutput::LoadMusicFilesCsv(const char* csv, std::string* errorMessage)
{
  if (errorMessage != nullptr)
  {
    errorMessage->clear();
  }

  std::vector<std::string> trackPaths;
  if (csv && csv[0] != '\0')
  {
    std::stringstream stream(csv);
    std::string token;
    while (std::getline(stream, token, ','))
    {
      const std::string path = Trim(token);
      if (path.empty())
      {
        continue;
      }
      trackPaths.push_back(path);
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  musicTracks_.clear();
  musicTracks_.reserve(trackPaths.size());
  for (const std::string& path : trackPaths)
  {
    MusicTrack track;
    track.path = path;
    musicTracks_.push_back(std::move(track));
  }
  musicTrackIndex_ = 0;
  musicGain_ = 0.0f;
  musicEnabled_ = false;
#if defined(PPUC_HAS_SDL3_MIXER)
  musicTrackStartPending_ = false;
  musicTrackStartTickMs_ = 0;
#endif

#if defined(PPUC_HAS_SDL3_MIXER)
  return ReloadMusicTracksLocked(errorMessage);
#else
  if (!musicTracks_.empty() && errorMessage != nullptr)
  {
    *errorMessage = "background music requires a build with SDL3_mixer";
  }
  return musicTracks_.empty();
#endif
}

void AudioOutput::SetMusicEnabled(bool enabled)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const bool wasEnabled = musicEnabled_;
  musicEnabled_ = enabled;

#if defined(PPUC_HAS_SDL3_MIXER)
  if (musicTrack_ == nullptr)
  {
    return;
  }

  if (!enabled)
  {
    MIX_PauseTrack(musicTrack_);
    return;
  }

  if (!wasEnabled)
  {
    musicTrackStartPending_ = false;
    musicTrackStartTickMs_ = 0;
    StartCurrentMusicTrackLocked();
  }
  else if (MIX_TrackPaused(musicTrack_))
  {
    MIX_ResumeTrack(musicTrack_);
  }
  else if (!MIX_TrackPlaying(musicTrack_))
  {
    StartCurrentMusicTrackLocked();
  }
#endif
}

void AudioOutput::SetMusicTrackGapMs(Uint64 gapMs)
{
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(PPUC_HAS_SDL3_MIXER)
  musicTrackGapMs_ = gapMs;
#else
  (void)gapMs;
#endif
}

void AudioOutput::QueueGameFrames(const int16_t* samples, size_t frameCount)
{
  if (samples == nullptr || frameCount == 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(gameQueue_, gameResampler_, samples, frameCount * gameChannels_, gameFrequency_, gameChannels_);
}

void AudioOutput::QueuePluginSamples(uint64_t sourceId, uint64_t streamId, const int16_t* samples, size_t sampleCount,
                                     int frequency, int channels)
{
  if (samples == nullptr || sampleCount == 0 || frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  PluginStream& stream = pluginStreams_[streamId];
  stream.sourceId = sourceId;
  QueueSamplesLocked(stream.queue, stream.resampler, samples, sampleCount, frequency, channels);
}

void AudioOutput::StopPluginStream(uint64_t streamId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = pluginStreams_.find(streamId);
  if (it != pluginStreams_.end())
  {
    DestroyResampler(it->second.resampler);
    pluginStreams_.erase(it);
  }
}

void AudioOutput::QueuePluginSamples(const int16_t* samples, size_t sampleCount, int frequency, int channels)
{
  QueuePluginSamples(0, 0, samples, sampleCount, frequency, channels);
}

void AudioOutput::SetAudioSources(const std::vector<AudioLanes::Source>& sources)
{
  std::lock_guard<std::mutex> lock(mutex_);
  lanes_.SetSources(sources, SDL_GetTicks());
}

void AudioOutput::SetOverrideMode(AudioLanes::OverrideMode mode)
{
  std::lock_guard<std::mutex> lock(mutex_);
  lanes_.SetMode(mode);
}

void AudioOutput::SetFallbackHoldMs(uint64_t holdMs)
{
  std::lock_guard<std::mutex> lock(mutex_);
  lanes_.SetFallbackHoldMs(holdMs);
}

std::string AudioOutput::DescribeLanes() const
{
  std::lock_guard<std::mutex> lock(mutex_);

  // Depth in milliseconds rather than samples: the point of watching it is to
  // see whether the bus drain keeps up with the producers, and only the time
  // form answers that independently of format.
  const double samplesPerMs =
      static_cast<double>(deviceSpec_.freq) * static_cast<double>(deviceSpec_.channels) / 1000.0;
  const uint64_t nowMs = SDL_GetTicks();

  std::string out;
  for (const AudioLanes::Lane& lane : lanes_.Lanes())
  {
    size_t buffered = 0;
    unsigned int streams = 0;
    for (const auto& entry : pluginStreams_)
    {
      if (entry.second.sourceId == lane.id)
      {
        buffered += AudioMixer::BufferedSamples(entry.second.queue);
        ++streams;
      }
    }
    out += "  " + (lane.name.empty() ? std::string("(unnamed)") : lane.name);
    out += lane.overridden ? " [overridden]" : "";
    out += lanes_.ShouldPlay(lane.id, nowMs) ? " heard" : " muted";
    // Whether the lane is unmuted and whether it carries any signal are
    // different questions, and only the second one answers "why can I not hear
    // anything". A lane that is heard but has not been audible for seconds is
    // producing silence, not being suppressed.
    const uint64_t silentForMs = nowMs > lane.lastAudibleMs ? nowMs - lane.lastAudibleMs : 0;
    out += silentForMs < 500 ? ", signal" : ", silent for " + std::to_string(silentForMs / 1000) + "s";
    out += ", " + std::to_string(streams) + " stream(s), ";
    out += std::to_string(samplesPerMs > 0.0 ? static_cast<int>(buffered / samplesPerMs) : 0);
    out += " ms buffered\n";
  }

  const size_t gameBuffered = AudioMixer::BufferedSamples(gameQueue_);
  if (gameBuffered != 0)
  {
    out += "  PPUC game audio, " +
           std::to_string(samplesPerMs > 0.0 ? static_cast<int>(gameBuffered / samplesPerMs) : 0) + " ms buffered\n";
  }
  return out;
}

void AudioOutput::QueueSpeechSamples(const int16_t* samples, size_t sampleCount, int frequency, int channels)
{
  if (samples == nullptr || sampleCount == 0 || frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(speechQueue_, speechResampler_, samples, sampleCount, frequency, channels);
}

void SDLCALL AudioOutput::OnDeviceNeedsAudio(void* userdata, SDL_AudioStream* stream, int additionalAmount,
                                             int /*totalAmount*/)
{
  auto* self = static_cast<AudioOutput*>(userdata);
  if (self == nullptr || additionalAmount <= 0)
  {
    return;
  }

  const size_t sampleCount = static_cast<size_t>(additionalAmount) / sizeof(int16_t);
  std::vector<int16_t> mixBuffer(sampleCount, 0);

  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    const uint64_t nowMs = SDL_GetTicks();
    const bool gameActive = AudioMixer::Mix(self->gameQueue_, mixBuffer.data(), sampleCount);
    bool pluginActive = false;

    // Lanes come back ordered so that an overrider is always decided before
    // whatever it overrides: ShouldPlay for a lower lane reads audibility that
    // is already current for this block.
    for (const AudioLanes::Lane& lane : self->lanes_.Lanes())
    {
      const bool play = self->lanes_.ShouldPlay(lane.id, nowMs);
      bool laneActive = false;
      for (auto& entry : self->pluginStreams_)
      {
        if (entry.second.sourceId != lane.id)
        {
          continue;
        }
        // A silenced lane is drained, never stalled. Its producer runs at the
        // emulator's rate regardless of who is listening, so holding the queue
        // back would fill it to the overflow cap and then dump stale audio the
        // moment the override lifted.
        laneActive = (play ? AudioMixer::Mix(entry.second.queue, mixBuffer.data(), sampleCount)
                           : AudioMixer::Discard(entry.second.queue, sampleCount)) ||
                     laneActive;
      }
      self->lanes_.NoteMixed(lane.id, laneActive, nowMs);
      pluginActive = pluginActive || (play && laneActive);
    }

    // Streams whose source has not been published yet. Audio can arrive before
    // the source list that describes it; dropping it over that ordering would
    // be worse than briefly ignoring an override.
    for (auto& entry : self->pluginStreams_)
    {
      if (self->lanes_.Knows(entry.second.sourceId))
      {
        continue;
      }
      pluginActive = AudioMixer::Mix(entry.second.queue, mixBuffer.data(), sampleCount) || pluginActive;
    }

    for (auto it = self->pluginStreams_.begin(); it != self->pluginStreams_.end();)
    {
      if (!it->second.queue.empty())
      {
        ++it;
        continue;
      }
      // Keep the entry while its resampler still holds a partial frame:
      // dropping it here would throw that fraction away every time a stream
      // momentarily drains, reintroducing the drift this exists to prevent.
      if (it->second.resampler.stream != nullptr && SDL_GetAudioStreamAvailable(it->second.resampler.stream) > 0)
      {
        ++it;
        continue;
      }
      DestroyResampler(it->second.resampler);
      it = self->pluginStreams_.erase(it);
    }

    const bool speechActive = AudioMixer::Mix(self->speechQueue_, mixBuffer.data(), sampleCount);
    self->MixMusicLocked(mixBuffer.data(), sampleCount, gameActive || pluginActive || speechActive);
  }

  SDL_PutAudioStreamData(stream, mixBuffer.data(), additionalAmount);
}

void AudioOutput::EnsureStreamLocked(const SDL_AudioSpec& spec)
{
  if (stream_ != nullptr && deviceSpec_.freq == spec.freq && deviceSpec_.channels == spec.channels &&
      deviceSpec_.format == spec.format)
  {
    return;
  }

  if (stream_ != nullptr)
  {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }

  deviceSpec_ = spec;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec_, &AudioOutput::OnDeviceNeedsAudio,
                                      this);
  if (stream_ != nullptr)
  {
    SDL_ResumeAudioStreamDevice(stream_);
  }
}

void AudioOutput::DestroyResampler(Resampler& resampler)
{
  if (resampler.stream != nullptr)
  {
    SDL_DestroyAudioStream(resampler.stream);
    resampler.stream = nullptr;
  }
  resampler.frequency = 0;
  resampler.channels = 0;
}

void AudioOutput::QueueSamplesLocked(AudioMixer::Queue& queue, Resampler& resampler, const int16_t* samples,
                                     size_t sampleCount, int frequency, int channels)
{
  if (stream_ == nullptr)
  {
    EnsureStreamLocked(deviceSpec_);
    if (stream_ == nullptr)
    {
      return;
    }
  }

  // A stream is created even when the formats already match. It is nearly free
  // at a 1:1 ratio, and it is what the rate control below steers -- a producer
  // that happens to share the device's rate still drifts against its clock.
  //
  // Rebuilt only when the producer's format changes, so the resampler keeps its
  // fractional position for the whole of a stream.
  if (resampler.stream == nullptr || resampler.frequency != frequency || resampler.channels != channels)
  {
    DestroyResampler(resampler);
    const SDL_AudioSpec sourceSpec{
        .format = SDL_AUDIO_S16LE,
        .channels = static_cast<Uint8>(channels),
        .freq = frequency,
    };
    resampler.stream = SDL_CreateAudioStream(&sourceSpec, &deviceSpec_);
    if (resampler.stream == nullptr)
    {
      return;
    }
    resampler.frequency = frequency;
    resampler.channels = channels;
  }

  if (!SDL_PutAudioStreamData(resampler.stream, samples, static_cast<int>(sampleCount * sizeof(int16_t))))
  {
    return;
  }

  const int available = SDL_GetAudioStreamAvailable(resampler.stream);
  if (available <= 0)
  {
    // Normal: the resampler is holding a partial frame back rather than
    // rounding it away, which is the entire point.
    return;
  }

  std::vector<int16_t> converted(static_cast<size_t>(available) / sizeof(int16_t));
  const int read = SDL_GetAudioStreamData(resampler.stream, converted.data(), available);
  if (read <= 0)
  {
    return;
  }
  converted.resize(static_cast<size_t>(read) / sizeof(int16_t));
  AudioMixer::Enqueue(queue, std::move(converted));
  TrimQueueLocked(queue);
  SteerQueueLocked(queue, resampler);
}

void AudioOutput::SteerQueueLocked(const AudioMixer::Queue& queue, Resampler& resampler)
{
  if (resampler.stream == nullptr)
  {
    return;
  }
  const size_t target = TargetBufferedSamplesLocked();
  if (target == 0)
  {
    return;
  }
  const float ratio = AudioMixer::RateRatioFor(AudioMixer::BufferedSamples(queue), target);
  // Only when it actually moves: the call takes a stream mutex, and this runs
  // for every buffer every producer sends.
  if (std::abs(ratio - resampler.ratio) > 0.00005f)
  {
    if (SDL_SetAudioStreamFrequencyRatio(resampler.stream, ratio))
    {
      resampler.ratio = ratio;
    }
  }
}

size_t AudioOutput::TargetBufferedSamplesLocked() const
{
  return static_cast<size_t>(deviceSpec_.freq) * deviceSpec_.channels * AudioMixer::kTargetBufferedMs / 1000u;
}

void AudioOutput::TrimQueueLocked(AudioMixer::Queue& queue)
{
  const size_t samplesPerMs = static_cast<size_t>(deviceSpec_.freq) * deviceSpec_.channels / 1000u;
  const size_t target = TargetBufferedSamplesLocked();
  const size_t highWater = samplesPerMs * AudioMixer::kHighWaterBufferedMs;
  if (target == 0)
  {
    return;
  }
  const size_t dropped = AudioMixer::TrimToTarget(queue, target, highWater);
  if (dropped != 0 && debugAudio_)
  {
    const double samplesPerMs = static_cast<double>(deviceSpec_.freq) * deviceSpec_.channels / 1000.0;
    std::printf("Audio: dropped %d ms to catch up\n",
                samplesPerMs > 0.0 ? static_cast<int>(dropped / samplesPerMs) : 0);
  }
}

void AudioOutput::MixMusicLocked(int16_t* mixBuffer, size_t sampleCount, bool duckToBackground)
{
  if (!mixBuffer || musicTracks_.empty())
  {
    musicGain_ = 0.0f;
    return;
  }

#if defined(PPUC_HAS_SDL3_MIXER)
  if (musicMixer_ == nullptr || musicTrack_ == nullptr)
  {
    musicGain_ = 0.0f;
    return;
  }

  if (musicTrackStartPending_)
  {
    if (SDL_GetTicks() < musicTrackStartTickMs_)
    {
      return;
    }

    musicTrackStartPending_ = false;
    musicTrackStartTickMs_ = 0;
    if (!StartCurrentMusicTrackLocked())
    {
      musicGain_ = 0.0f;
      return;
    }
  }

  std::vector<int16_t> musicBuffer(sampleCount, 0);
  const int generatedBytes =
      MIX_Generate(musicMixer_, musicBuffer.data(), static_cast<int>(sampleCount * sizeof(int16_t)));
  if (generatedBytes < 0)
  {
    musicGain_ = 0.0f;
    return;
  }
#endif

  const float targetGain = musicEnabled_ ? (duckToBackground ? kMusicDuckGain : kMusicBaseGain) : 0.0f;
  const float gainStep = targetGain > musicGain_ ? kMusicAttackPerSample : kMusicReleasePerSample;

  for (size_t i = 0; i < sampleCount; ++i)
  {
    if (musicGain_ < targetGain)
    {
      musicGain_ = std::min(targetGain, musicGain_ + gainStep);
    }
    else if (musicGain_ > targetGain)
    {
      musicGain_ = std::max(targetGain, musicGain_ - gainStep);
    }

    if (musicGain_ <= 0.0f)
    {
      continue;
    }

#if defined(PPUC_HAS_SDL3_MIXER)
    const int16_t sample = musicBuffer[i];
    const int mixedValue =
        static_cast<int>(mixBuffer[i]) + static_cast<int>(std::lround(static_cast<float>(sample) * musicGain_));
    mixBuffer[i] = AudioMixer::ClampSample(mixedValue);
#endif
  }
}

#if defined(PPUC_HAS_SDL3_MIXER)
void SDLCALL AudioOutput::OnMusicTrackStopped(void* userdata, MIX_Track* track)
{
  auto* self = static_cast<AudioOutput*>(userdata);
  if (self != nullptr)
  {
    self->HandleMusicTrackStoppedLocked(track);
  }
}

bool AudioOutput::EnsureMusicMixerLocked(std::string* errorMessage)
{
  if (!MIX_Init())
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = SDL_GetError();
    }
    return false;
  }

  if (musicMixer_ == nullptr)
  {
    musicMixer_ = MIX_CreateMixer(&deviceSpec_);
    if (musicMixer_ == nullptr)
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = SDL_GetError();
      }
      return false;
    }
  }

  if (musicTrack_ == nullptr)
  {
    musicTrack_ = MIX_CreateTrack(musicMixer_);
    if (musicTrack_ == nullptr)
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = SDL_GetError();
      }
      return false;
    }
    MIX_SetTrackStoppedCallback(musicTrack_, &AudioOutput::OnMusicTrackStopped, this);
  }

  return true;
}

void AudioOutput::DestroyMusicTracksLocked()
{
  for (MusicTrack& track : musicTracks_)
  {
    if (track.audio != nullptr)
    {
      MIX_DestroyAudio(track.audio);
      track.audio = nullptr;
    }
  }
  musicTracks_.clear();
}

bool AudioOutput::ReloadMusicTracksLocked(std::string* errorMessage)
{
  if (!EnsureMusicMixerLocked(errorMessage))
  {
    return false;
  }

  if (musicTrack_ != nullptr)
  {
    MIX_DestroyTrack(musicTrack_);
    musicTrack_ = nullptr;
  }

  if (!EnsureMusicMixerLocked(errorMessage))
  {
    return false;
  }

  for (MusicTrack& track : musicTracks_)
  {
    if (track.audio != nullptr)
    {
      MIX_DestroyAudio(track.audio);
      track.audio = nullptr;
    }

    if (track.path.empty())
    {
      continue;
    }

    track.audio = MIX_LoadAudio(musicMixer_, track.path.c_str(), false);
    if (track.audio == nullptr)
    {
      if (errorMessage != nullptr)
      {
        *errorMessage = "Unable to load music file '" + track.path + "': " + SDL_GetError();
      }
      DestroyMusicTracksLocked();
      return false;
    }
  }

  if (!musicTracks_.empty() && musicEnabled_)
  {
    return StartCurrentMusicTrackLocked();
  }

  return true;
}

bool AudioOutput::StartCurrentMusicTrackLocked()
{
  if (musicTrack_ == nullptr || musicTracks_.empty())
  {
    return true;
  }

  musicTrackStartPending_ = false;
  musicTrackStartTickMs_ = 0;

  for (size_t attempts = 0; attempts < musicTracks_.size(); ++attempts)
  {
    MusicTrack& currentTrack = musicTracks_[musicTrackIndex_];
    if (currentTrack.audio != nullptr)
    {
      if (!MIX_SetTrackAudio(musicTrack_, currentTrack.audio))
      {
        return false;
      }
      if (!MIX_PlayTrack(musicTrack_, 0))
      {
        return false;
      }
      return true;
    }
    AdvanceMusicTrackLocked();
  }

  return true;
}

void AudioOutput::ScheduleNextMusicTrackLocked()
{
  musicTrackStartPending_ = true;
  musicTrackStartTickMs_ = SDL_GetTicks() + musicTrackGapMs_;
}

void AudioOutput::AdvanceMusicTrackLocked()
{
  if (!musicTracks_.empty())
  {
    musicTrackIndex_ = (musicTrackIndex_ + 1) % musicTracks_.size();
  }
}

void AudioOutput::HandleMusicTrackStoppedLocked(MIX_Track* track)
{
  if (track != musicTrack_ || musicTracks_.empty())
  {
    return;
  }

  AdvanceMusicTrackLocked();
  if (musicEnabled_)
  {
    ScheduleNextMusicTrackLocked();
  }
}
#endif
