#include "AttractSlides.h"

#include <algorithm>

namespace AttractSlides
{

uint8_t FadeAlpha(uint64_t elapsedMs, uint32_t durationMs, uint32_t fadeMs, bool paused)
{
  if (fadeMs == 0)
  {
    return 0;
  }

  // Fading in, from the moment the slide came up.
  float dim = 0.0f;
  if (elapsedMs < fadeMs)
  {
    dim = 1.0f - static_cast<float>(elapsedMs) / static_cast<float>(fadeMs);
  }

  // And out, as its time runs down -- but not while it is held. A slide
  // somebody is reading must not dim underneath them, and a held slide has no
  // end to count back from.
  if (!paused && durationMs > 0 && elapsedMs < durationMs)
  {
    const uint64_t remaining = durationMs - elapsedMs;
    if (remaining < fadeMs)
    {
      dim = std::max(dim, 1.0f - static_cast<float>(remaining) / static_cast<float>(fadeMs));
    }
  }

  // A slide shorter than two fades never reaches full brightness. That is the
  // honest result rather than a bug: the answer is a longer slide.
  return static_cast<uint8_t>(std::clamp(dim, 0.0f, 1.0f) * 255.0f);
}

void Show::SetSlides(std::vector<Slide> slides)
{
  m_slides = std::move(slides);
  m_index = 0;
  m_visible = false;
}

void Show::NoteActivity(uint64_t nowMs)
{
  m_clockStarted = true;
  m_lastActivityMs = nowMs;
  if (m_visible)
  {
    // Down immediately, and the next show starts from the first slide: a
    // player who walks away has not seen the rest of this one.
    m_visible = false;
    m_index = 0;
    // A hold belongs to the show that was running, not to the machine.
    m_paused = false;
  }
}

void Show::ShowAt(size_t index, uint64_t nowMs)
{
  m_clockStarted = true;
  m_visible = true;
  m_index = index;
  m_slideStartedMs = nowMs;
}

void Show::Next(uint64_t nowMs)
{
  if (m_slides.empty())
  {
    return;
  }
  // From hidden, the first press brings up the first slide rather than the
  // second: somebody who has just asked for the rules wants them from the top.
  ShowAt(m_visible ? (m_index + 1) % m_slides.size() : 0, nowMs);
}

void Show::Previous(uint64_t nowMs)
{
  if (m_slides.empty())
  {
    return;
  }
  ShowAt(m_visible ? (m_index + m_slides.size() - 1) % m_slides.size() : 0, nowMs);
}

void Show::TogglePause()
{
  if (m_visible)
  {
    m_paused = !m_paused;
  }
}

void Show::Update(bool attract, uint64_t nowMs)
{
  if (m_slides.empty() || !attract)
  {
    // A game running is activity, whether or not a switch said so. Treating it
    // as such also means the idle countdown restarts when the game ends,
    // rather than the slides appearing the moment the last ball drains.
    if (!attract)
    {
      m_clockStarted = true;
      m_lastActivityMs = nowMs;
      m_visible = false;
      m_index = 0;
      m_paused = false;
    }
    return;
  }

  if (!m_clockStarted)
  {
    // Nothing has happened since the machine came up. Count from the first
    // pass rather than from zero, or a machine that has been up for a while
    // before PPUC starts shows slides immediately.
    m_clockStarted = true;
    m_lastActivityMs = nowMs;
    return;
  }

  if (!m_visible)
  {
    if (nowMs - m_lastActivityMs >= m_idleMs)
    {
      ShowAt(0, nowMs);
    }
    return;
  }

  if (m_paused)
  {
    return;
  }

  if (nowMs - m_slideStartedMs >= DurationOf(m_slides[m_index]))
  {
    m_index = (m_index + 1) % m_slides.size();
    m_slideStartedMs = nowMs;
  }
}

}  // namespace AttractSlides
