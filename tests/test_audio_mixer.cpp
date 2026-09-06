// Tests for the sample-mixing core.
//
// This logic used to be private to AudioOutput and therefore only reachable
// through an initialised SDL audio device, which meant it had no coverage at
// all. Three behaviours here are load-bearing and each fails silently when
// wrong: the overflow trim (a producer outrunning the device must lose its
// oldest audio, not grow without bound), saturation on mix (two loud sources
// must clip rather than wrap to the opposite sign), and the audibility signal
// that drives music ducking -- and that the AltSound mode-1 fallback in
// docs/PLUGIN_MIGRATION.md will use to decide when an overriding source has
// gone quiet.

#include "doctest.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "AudioMixer.h"

namespace
{

std::vector<int16_t> Block(size_t count, int16_t value)
{
  return std::vector<int16_t>(count, value);
}

}  // namespace

TEST_CASE("ClampSample saturates instead of wrapping")
{
  CHECK(AudioMixer::ClampSample(0) == 0);
  CHECK(AudioMixer::ClampSample(1000) == 1000);
  CHECK(AudioMixer::ClampSample(-1000) == -1000);

  CHECK(AudioMixer::ClampSample(std::numeric_limits<int16_t>::max()) == std::numeric_limits<int16_t>::max());
  CHECK(AudioMixer::ClampSample(std::numeric_limits<int16_t>::min()) == std::numeric_limits<int16_t>::min());

  // The case that matters: summing two near-full-scale sources. Wrapping here
  // turns a loud passage into a loud passage of the opposite sign, which is
  // audible as a hard crack.
  CHECK(AudioMixer::ClampSample(40000) == std::numeric_limits<int16_t>::max());
  CHECK(AudioMixer::ClampSample(-40000) == std::numeric_limits<int16_t>::min());
}

TEST_CASE("Enqueue accumulates and BufferedSamples counts what is left")
{
  AudioMixer::Queue queue;
  CHECK(AudioMixer::BufferedSamples(queue) == 0);

  AudioMixer::Enqueue(queue, Block(10, 100));
  AudioMixer::Enqueue(queue, Block(5, 100));
  CHECK(queue.size() == 2);
  CHECK(AudioMixer::BufferedSamples(queue) == 15);
}

TEST_CASE("Enqueue ignores empty blocks")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, {});
  CHECK(queue.empty());
  CHECK(AudioMixer::BufferedSamples(queue) == 0);
}

TEST_CASE("Enqueue drops the oldest audio once over the cap")
{
  AudioMixer::Queue queue;
  const size_t cap = 10;

  AudioMixer::Enqueue(queue, Block(6, 1), cap);
  AudioMixer::Enqueue(queue, Block(6, 2), cap);
  // 12 > 10, so the front block goes and the newest audio survives.
  CHECK(queue.size() == 1);
  CHECK(AudioMixer::BufferedSamples(queue) == 6);
  CHECK(queue.front().samples[0] == 2);
}

TEST_CASE("The overflow trim accounts for already-consumed samples")
{
  AudioMixer::Queue queue;
  const size_t cap = 10;

  AudioMixer::Enqueue(queue, Block(8, 1), cap);

  std::vector<int16_t> mix(6, 0);
  AudioMixer::Mix(queue, mix.data(), mix.size());
  // 2 samples left unconsumed, so a 7-sample block still fits under the cap
  // and must not evict anything.
  CHECK(AudioMixer::BufferedSamples(queue) == 2);

  AudioMixer::Enqueue(queue, Block(7, 2), cap);
  CHECK(queue.size() == 2);
  CHECK(AudioMixer::BufferedSamples(queue) == 9);
}

TEST_CASE("Mix adds into the buffer rather than overwriting it")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 100));

  std::vector<int16_t> mix(4, 50);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  for (int16_t sample : mix)
  {
    CHECK(sample == 150);
  }
  CHECK(queue.empty());
}

TEST_CASE("Mix saturates when summed sources exceed full scale")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 30000));

  std::vector<int16_t> mix(4, 30000);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  for (int16_t sample : mix)
  {
    CHECK(sample == std::numeric_limits<int16_t>::max());
  }
}

TEST_CASE("Mix leaves the tail untouched when the queue runs dry")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(2, 100));

  std::vector<int16_t> mix(5, 7);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  CHECK(mix[0] == 107);
  CHECK(mix[1] == 107);
  // Not zeroed: the caller mixes several queues into this same buffer, so
  // clearing the remainder would silence every source mixed after this one.
  CHECK(mix[2] == 7);
  CHECK(mix[3] == 7);
  CHECK(mix[4] == 7);
}

TEST_CASE("Mix spans block boundaries and consumes exactly what it used")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(3, 10));
  AudioMixer::Enqueue(queue, Block(3, 20));

  std::vector<int16_t> mix(4, 0);
  AudioMixer::Mix(queue, mix.data(), mix.size());

  CHECK(mix[0] == 10);
  CHECK(mix[1] == 10);
  CHECK(mix[2] == 10);
  CHECK(mix[3] == 20);
  CHECK(AudioMixer::BufferedSamples(queue) == 2);
}

TEST_CASE("A partially consumed block resumes where it left off")
{
  AudioMixer::Queue queue;
  std::vector<int16_t> block = {1, 2, 3, 4};
  AudioMixer::Enqueue(queue, block);

  std::vector<int16_t> first(2, 0);
  AudioMixer::Mix(queue, first.data(), first.size());
  CHECK(first[0] == 1);
  CHECK(first[1] == 2);

  std::vector<int16_t> second(2, 0);
  AudioMixer::Mix(queue, second.data(), second.size());
  CHECK(second[0] == 3);
  CHECK(second[1] == 4);
  CHECK(queue.empty());
}

TEST_CASE("Mix reports audibility against the threshold")
{
  SUBCASE("silence is not audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, 0));
    std::vector<int16_t> mix(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("just below the threshold is not audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, AudioMixer::kAudibleSampleThreshold - 1));
    std::vector<int16_t> mix(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("the threshold itself is audible")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, AudioMixer::kAudibleSampleThreshold));
    std::vector<int16_t> mix(4, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("negative samples count on magnitude, not sign")
  {
    AudioMixer::Queue queue;
    AudioMixer::Enqueue(queue, Block(4, -AudioMixer::kAudibleSampleThreshold));
    std::vector<int16_t> mix(4, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("one loud sample in an otherwise quiet block is audible")
  {
    AudioMixer::Queue queue;
    std::vector<int16_t> block(8, 0);
    block[5] = 20000;
    AudioMixer::Enqueue(queue, block);
    std::vector<int16_t> mix(8, 0);
    CHECK(AudioMixer::Mix(queue, mix.data(), mix.size()));
  }

  SUBCASE("audibility describes what was consumed, not what remains")
  {
    AudioMixer::Queue queue;
    std::vector<int16_t> block(8, 0);
    block[6] = 20000;
    AudioMixer::Enqueue(queue, block);

    std::vector<int16_t> quiet(4, 0);
    CHECK_FALSE(AudioMixer::Mix(queue, quiet.data(), quiet.size()));

    std::vector<int16_t> loud(4, 0);
    CHECK(AudioMixer::Mix(queue, loud.data(), loud.size()));
  }
}

TEST_CASE("Mixing an empty queue is a silent no-op")
{
  AudioMixer::Queue queue;
  std::vector<int16_t> mix(4, 9);
  CHECK_FALSE(AudioMixer::Mix(queue, mix.data(), mix.size()));
  for (int16_t sample : mix)
  {
    CHECK(sample == 9);
  }
}

TEST_CASE("Mix tolerates a null buffer")
{
  AudioMixer::Queue queue;
  AudioMixer::Enqueue(queue, Block(4, 100));
  CHECK_FALSE(AudioMixer::Mix(queue, nullptr, 4));
  // The queue must not have been consumed by a call that mixed nothing.
  CHECK(AudioMixer::BufferedSamples(queue) == 4);
}
