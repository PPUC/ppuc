#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// What one thread hands another without either waiting on the other.
//
// ppuc-pinmame runs the machine -- switches, the rules, the coils -- on a
// thread of its own, and everything that draws, plays or talks to the plugin
// bus on the main thread. The two meet only in queues like these: the producer
// appends under a lock held for the length of a push_back, the consumer swaps
// the whole batch out under the same lock and works on it afterwards. Neither
// side ever holds the lock while it does the actual work, so a slow consumer
// (a sound file being loaded, a frame being presented) never holds up the
// producer.
//
// Bounded, because a consumer that stops consuming must not turn into memory
// growth on the machine thread. What overflows is dropped and counted.

template <typename T>
class HandoffQueue
{
 public:
  explicit HandoffQueue(size_t capacity = 4096) : m_capacity(capacity) {}

  // False when the queue was full and the item was dropped.
  bool Push(T item)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_items.size() >= m_capacity)
    {
      ++m_dropped;
      return false;
    }
    m_items.push_back(std::move(item));
    return true;
  }

  // Everything queued so far, oldest first. `out` is cleared first and its
  // capacity reused, so a consumer that keeps one vector allocates nothing in
  // the steady state.
  void TakeAll(std::vector<T>& out)
  {
    out.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_items);
  }

  size_t Dropped() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
  }

 private:
  mutable std::mutex m_mutex;
  std::vector<T> m_items;
  size_t m_capacity;
  size_t m_dropped = 0;
};

// Work for the thread that runs RunAll(), posted from any thread.
class TaskQueue
{
 public:
  explicit TaskQueue(size_t capacity = 1024) : m_queue(capacity) {}

  bool Post(std::function<void()> task) { return m_queue.Push(std::move(task)); }

  // Runs what was posted before the call, in order. A task that posts another
  // one has it run on the next call, not this one.
  void RunAll()
  {
    m_queue.TakeAll(m_batch);
    for (std::function<void()>& task : m_batch)
    {
      task();
    }
    m_batch.clear();
  }

  size_t Dropped() const { return m_queue.Dropped(); }

 private:
  HandoffQueue<std::function<void()>> m_queue;
  std::vector<std::function<void()>> m_batch;
};
