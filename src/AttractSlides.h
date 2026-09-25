#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How-to-play slides, shown on the backbox screen while a machine stands idle
// in attract.
//
// A machine in a bar has a screen doing nothing and a crowd who have never
// played it. After a minute of nothing happening this puts a few slides up --
// a photograph of the playfield with the shots numbered and a line of text --
// and takes them down the instant anyone touches the machine.
//
// The timing lives here, separate from the drawing, because it is the part
// that can be wrong in ways nobody notices: slides that never start, or that
// will not go away when a player walks up.
namespace AttractSlides
{

// A point on the photograph the machine wants to draw attention to.
//
// x and y run from 0 to 1 across the picture rather than in pixels, so a slide
// authored once survives being scaled to whatever screen the machine has.
struct Marker
{
  float x = 0.0f;
  float y = 0.0f;
  // 0 when the marker is just a pointer with nothing to count.
  int number = 0;
  // Which side the arrow comes in from, so it never covers what it points at.
  //
  // The diagonals are not decoration: an arrow that comes from the lower left
  // is the line a ball takes off the left flipper, and one that comes straight
  // down is a ball draining. Four directions could only ever say "this thing
  // here"; eight can say how you get to it.
  enum class Pointer
  {
    Left,
    Right,
    Above,
    Below,
    AboveLeft,
    AboveRight,
    BelowLeft,
    BelowRight
  };
  Pointer pointer = Pointer::Left;
};

struct Slide
{
  std::string title;
  std::string text;
  std::string imagePath;
  uint32_t durationMs = 0;
  std::vector<Marker> markers;
};

// Reads slides.yaml from a game folder's slides directory. Returns false and
// fills `error` when the file is there but unreadable; an absent folder is not
// an error, it is a game with no slides.
//
// Defined in AttractSlidesLoader.cpp, which is the only part of this that needs
// yaml-cpp; the timing in AttractSlides.cpp links without it, and so can be
// unit tested.
bool Load(const std::string& gameFolder, std::vector<Slide>* slides, std::string* error);

// How much black to lay over a slide, 0 (none) to 255 (all), so that slides
// fade in and out of each other rather than cutting.
//
// Here rather than in the drawing because it is arithmetic with edge cases --
// a slide shorter than two fades, a held slide that must not fade out while
// somebody is reading it -- and those are cheap to test and tedious to check
// by staring at a screen.
uint8_t FadeAlpha(uint64_t elapsedMs, uint32_t durationMs, uint32_t fadeMs, bool paused);

// When the slideshow runs, and which slide is up.
//
// Nothing here draws or touches SDL: it answers "should something be on the
// screen, and which" from the clock and from what the machine has been doing.
class Show
{
 public:
  Show(uint32_t idleMs, uint32_t defaultDurationMs) : m_idleMs(idleMs), m_defaultDurationMs(defaultDurationMs) {}

  void SetSlides(std::vector<Slide> slides);
  bool Empty() const { return m_slides.empty(); }
  const std::vector<Slide>& Slides() const { return m_slides; }

  // Any switch on the machine, deliberately including the flipper buttons and
  // the coin door -- unlike the ball search, which ignores them. Those are the
  // first things a curious passer-by touches, and someone who has just pressed
  // a flipper button is someone who has started reading.
  void NoteActivity(uint64_t nowMs);

  // Called every pass. `attract` is false the moment a game is running, which
  // takes the slides down without needing to hear about a switch.
  void Update(bool attract, uint64_t nowMs);

  // The two navigation buttons, and the cursor keys that stand in for them.
  //
  // These are the one thing that does not count as activity: pressing them is
  // somebody reading, not somebody walking up, so they move the show along
  // instead of ending it. From hidden they start it, which is how a player who
  // wants to know the rules asks for them rather than waiting out the minute.
  void Next(uint64_t nowMs);
  void Previous(uint64_t nowMs);

  // Both buttons together, or ENTER. Holds the current slide until it is
  // pressed again -- for the player who is still reading when the eight
  // seconds are up.
  void TogglePause();
  bool Paused() const { return m_paused; }

  bool Visible() const { return m_visible; }
  // Only meaningful while Visible().
  const Slide& Current() const { return m_slides[m_index]; }
  size_t CurrentIndex() const { return m_index; }
  // When the current slide came up, for animations that should start with it.
  uint64_t CurrentSinceMs() const { return m_slideStartedMs; }
  // What the current slide's duration works out to, its own or the default.
  uint32_t CurrentDurationMs() const { return DurationOf(m_slides[m_index]); }

 private:
  void ShowAt(size_t index, uint64_t nowMs);

  uint32_t DurationOf(const Slide& slide) const
  {
    return slide.durationMs != 0 ? slide.durationMs : m_defaultDurationMs;
  }

  std::vector<Slide> m_slides;
  uint32_t m_idleMs;
  uint32_t m_defaultDurationMs;
  // A separate flag rather than m_lastActivityMs == 0 meaning "never": SDL's
  // tick count starts at zero, so zero is a time the machine really sees.
  bool m_clockStarted = false;
  bool m_paused = false;
  uint64_t m_lastActivityMs = 0;
  uint64_t m_slideStartedMs = 0;
  size_t m_index = 0;
  bool m_visible = false;
};

}  // namespace AttractSlides
