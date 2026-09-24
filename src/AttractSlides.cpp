#include "AttractSlides.h"

namespace AttractSlides
{

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
      m_visible = true;
      m_index = 0;
      m_slideStartedMs = nowMs;
    }
    return;
  }

  if (nowMs - m_slideStartedMs >= DurationOf(m_slides[m_index]))
  {
    m_index = (m_index + 1) % m_slides.size();
    m_slideStartedMs = nowMs;
  }
}

}  // namespace AttractSlides
