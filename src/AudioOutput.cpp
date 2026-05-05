#include "AudioOutput.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#if defined(PPUC_HAS_SDL3_MIXER)
#include "SDL3_mixer/SDL_mixer.h"
#endif

namespace
{
constexpr size_t kMaxBufferedSamples = 22050 * 30;
constexpr float kMusicBaseGain = 0.28f;
constexpr float kMusicDuckGain = 0.08f;
constexpr float kMusicAttackPerSample = 0.00012f;
constexpr float kMusicReleasePerSample = 0.00003f;

static int16_t ClampMixedSample(int value)
{
  if (value > std::numeric_limits<int16_t>::max())
  {
    return std::numeric_limits<int16_t>::max();
  }
  if (value < std::numeric_limits<int16_t>::min())
  {
    return std::numeric_limits<int16_t>::min();
  }
  return static_cast<int16_t>(value);
}

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

AudioOutput::~AudioOutput()
{
  Shutdown();
}

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
  speechQueue_.clear();
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

  if (MIX_TrackPaused(musicTrack_))
  {
    MIX_ResumeTrack(musicTrack_);
  }
  else if (!MIX_TrackPlaying(musicTrack_))
  {
    StartCurrentMusicTrackLocked();
  }
#endif
}

void AudioOutput::QueueGameFrames(const int16_t* samples, size_t frameCount)
{
  if (samples == nullptr || frameCount == 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(gameQueue_, samples, frameCount * gameChannels_,
                     gameFrequency_, gameChannels_);
}

void AudioOutput::QueueSpeechSamples(const int16_t* samples, size_t sampleCount,
                                     int frequency, int channels)
{
  if (samples == nullptr || sampleCount == 0 || frequency <= 0 || channels <= 0)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  QueueSamplesLocked(speechQueue_, samples, sampleCount, frequency, channels);
}

void SDLCALL AudioOutput::OnDeviceNeedsAudio(void* userdata,
                                             SDL_AudioStream* stream,
                                             int additionalAmount,
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
    const bool gameActive = self->MixQueueLocked(self->gameQueue_, mixBuffer.data(), sampleCount);
    const bool speechActive = self->MixQueueLocked(self->speechQueue_, mixBuffer.data(), sampleCount);
    self->MixMusicLocked(mixBuffer.data(), sampleCount, gameActive || speechActive);
  }

  SDL_PutAudioStreamData(stream, mixBuffer.data(), additionalAmount);
}

void AudioOutput::EnsureStreamLocked(const SDL_AudioSpec& spec)
{
  if (stream_ != nullptr && deviceSpec_.freq == spec.freq &&
      deviceSpec_.channels == spec.channels &&
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
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                      &deviceSpec_, &AudioOutput::OnDeviceNeedsAudio,
                                      this);
  if (stream_ != nullptr)
  {
    SDL_ResumeAudioStreamDevice(stream_);
  }
}

void AudioOutput::QueueSamplesLocked(std::deque<PendingBuffer>& queue,
                                     const int16_t* samples,
                                     size_t sampleCount, int frequency,
                                     int channels)
{
  if (stream_ == nullptr)
  {
    EnsureStreamLocked(deviceSpec_);
    if (stream_ == nullptr)
    {
      return;
    }
  }

  PendingBuffer pending;
  const bool requiresConversion = frequency != deviceSpec_.freq ||
                                  channels != deviceSpec_.channels;

  if (!requiresConversion)
  {
    pending.samples.assign(samples, samples + sampleCount);
  }
  else
  {
    const SDL_AudioSpec sourceSpec{
        .format = SDL_AUDIO_S16LE,
        .channels = static_cast<Uint8>(channels),
        .freq = frequency,
    };

    Uint8* convertedData = nullptr;
    int convertedLength = 0;
    if (!SDL_ConvertAudioSamples(
            &sourceSpec, reinterpret_cast<const Uint8*>(samples),
            static_cast<int>(sampleCount * sizeof(int16_t)), &deviceSpec_,
            &convertedData, &convertedLength))
    {
      return;
    }

    const auto* convertedSamples =
        reinterpret_cast<const int16_t*>(convertedData);
    const size_t convertedSampleCount =
        static_cast<size_t>(convertedLength) / sizeof(int16_t);
    pending.samples.assign(convertedSamples,
                           convertedSamples + convertedSampleCount);
    SDL_free(convertedData);
  }

  if (pending.samples.empty())
  {
    return;
  }

  queue.push_back(std::move(pending));

  size_t bufferedSamples = 0;
  for (const auto& entry : queue)
  {
    bufferedSamples += entry.samples.size() - entry.offsetSamples;
  }
  while (bufferedSamples > kMaxBufferedSamples && !queue.empty())
  {
    bufferedSamples -= queue.front().samples.size() - queue.front().offsetSamples;
    queue.pop_front();
  }
}

bool AudioOutput::MixQueueLocked(std::deque<PendingBuffer>& queue,
                                 int16_t* mixBuffer, size_t sampleCount)
{
  size_t mixedSamples = 0;
  bool hadAudibleSamples = false;
  while (mixedSamples < sampleCount && !queue.empty())
  {
    PendingBuffer& front = queue.front();
    const size_t availableSamples = front.samples.size() - front.offsetSamples;
    const size_t chunkSamples =
        std::min(sampleCount - mixedSamples, availableSamples);

    for (size_t i = 0; i < chunkSamples; ++i)
    {
      const int16_t sample = front.samples[front.offsetSamples + i];
      if (!hadAudibleSamples && std::abs(static_cast<int>(sample)) >= 512)
      {
        hadAudibleSamples = true;
      }
      const int mixedValue = static_cast<int>(mixBuffer[mixedSamples + i]) +
                             static_cast<int>(sample);
      mixBuffer[mixedSamples + i] = ClampMixedSample(mixedValue);
    }

    mixedSamples += chunkSamples;
    front.offsetSamples += chunkSamples;
    if (front.offsetSamples >= front.samples.size())
    {
      queue.pop_front();
    }
  }

  return hadAudibleSamples;
}

void AudioOutput::MixMusicLocked(int16_t* mixBuffer, size_t sampleCount,
                                 bool duckToBackground)
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

  std::vector<int16_t> musicBuffer(sampleCount, 0);
  const int generatedBytes =
      MIX_Generate(musicMixer_, musicBuffer.data(),
                   static_cast<int>(sampleCount * sizeof(int16_t)));
  if (generatedBytes < 0)
  {
    musicGain_ = 0.0f;
    return;
  }
#endif

  const float targetGain = musicEnabled_
                               ? (duckToBackground ? kMusicDuckGain : kMusicBaseGain)
                               : 0.0f;
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
    mixBuffer[i] = ClampMixedSample(mixedValue);
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
    StartCurrentMusicTrackLocked();
  }
}
#endif
