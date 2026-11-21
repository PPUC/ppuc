// Tests for the queues between the switch thread and the main thread.
//
// What matters is that nothing is lost or reordered between the two, that a
// full queue drops instead of blocking the producer, and that a task posting a
// task does not run it in the same drain (which would let a task that reposts
// itself keep the main thread in RunAll() forever).

#include <thread>
#include <vector>

#include "ThreadHandoff.h"
#include "doctest.h"

TEST_CASE("HandoffQueue keeps order and hands over everything once")
{
  HandoffQueue<int> queue;
  for (int i = 0; i < 5; ++i)
  {
    CHECK(queue.Push(i));
  }
  std::vector<int> out;
  queue.TakeAll(out);
  CHECK(out == std::vector<int>{0, 1, 2, 3, 4});
  queue.TakeAll(out);
  CHECK(out.empty());
}

TEST_CASE("HandoffQueue drops what does not fit instead of waiting")
{
  HandoffQueue<int> queue(2);
  CHECK(queue.Push(1));
  CHECK(queue.Push(2));
  CHECK_FALSE(queue.Push(3));
  CHECK(queue.Dropped() == 1);
  std::vector<int> out;
  queue.TakeAll(out);
  CHECK(out == std::vector<int>{1, 2});
  CHECK(queue.Push(4));
}

TEST_CASE("HandoffQueue loses nothing between a producer and a consumer thread")
{
  HandoffQueue<int> queue(1 << 20);
  constexpr int kCount = 200000;
  std::thread producer(
      [&]()
      {
        for (int i = 0; i < kCount; ++i)
        {
          queue.Push(i);
        }
      });
  std::vector<int> batch;
  int expected = 0;
  bool inOrder = true;
  // The capacity holds everything, so every value arrives; the loop ends when
  // the last one has.
  while (expected < kCount)
  {
    queue.TakeAll(batch);
    for (int value : batch)
    {
      inOrder = inOrder && value == expected;
      ++expected;
    }
  }
  producer.join();
  CHECK(inOrder);
  CHECK(expected == kCount);
  CHECK(queue.Dropped() == 0);
}

TEST_CASE("TaskQueue runs tasks in order, and a task posted by a task on the next drain")
{
  TaskQueue tasks;
  std::vector<int> ran;
  tasks.Post([&]() { ran.push_back(1); });
  tasks.Post(
      [&]()
      {
        ran.push_back(2);
        tasks.Post([&]() { ran.push_back(3); });
      });
  tasks.RunAll();
  CHECK(ran == std::vector<int>{1, 2});
  tasks.RunAll();
  CHECK(ran == std::vector<int>{1, 2, 3});
}
