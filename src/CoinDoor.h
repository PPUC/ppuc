#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// The four service buttons inside the coin door, and who they belong to.
//
// With the door open, ENTER opens the PPUC tools, and while the tools are up all
// four buttons drive them instead of the game: ESCAPE goes back, UP and DOWN move
// and change values, ENTER chooses. With the door closed, or the tools closed,
// they are the game's, as they always were -- UP and DOWN are the volume on a
// WPC with the door open, and stay so.
//
// A ROM has a service menu of its own behind the same ENTER. The tools offer
// it as an entry: choosing it hands the buttons back to the ROM, and pressing
// ENTER on its behalf, until the coin door is closed again.
//
// The decision is made on the switch thread, before the rules and the engine
// see the switch, because that is where a switch is either forwarded or not. A
// release always goes where its press went: the ROM must never see a release
// of a press it was not told about, or miss the release of one it was.
//
// Depends on nothing but the standard library.

struct CoinDoorButtons
{
  enum Button
  {
    Escape = 0,
    Down = 1,
    Up = 2,
    Enter = 3,
    Count = 4,
  };

  // Switch numbers, 0 for a button the machine does not have.
  std::array<int, Count> numbers{0, 0, 0, 0};

  bool Any() const { return numbers[0] || numbers[1] || numbers[2] || numbers[3]; }
  // Which button a switch is, or -1.
  int ButtonFor(int number) const;

  // What a platform's coin door has. WPC's dedicated switches 5 to 8 are
  // Escape, Down, Up and Enter (Begin Test). Others have no four-button door or
  // number it differently, and get nothing: their config names the buttons.
  static CoinDoorButtons ForPlatform(uint8_t platform);
  // "escape,down,up,enter" as four numbers, 0 for none.
  static bool Parse(const std::string& text, CoinDoorButtons* out, std::string* error);
  std::string Describe() const;
};

class CoinDoorRouter
{
 public:
  enum class Route
  {
    // To the rules and the engine, as any switch.
    Game,
    // To the tools only.
    Tools,
  };

  // `doorClosedSwitch` 0: the machine has no door switch, and the door counts
  // as open.
  void Configure(const CoinDoorButtons& buttons, int doorClosedSwitch);

  // Main thread: whether a screen for the tools exists at all. Without one,
  // ENTER stays the game's.
  void SetToolsAvailable(bool available) { m_toolsAvailable.store(available, std::memory_order_release); }
  // Main thread: whether the tools are up.
  void SetToolsOpen(bool open) { m_toolsOpen.store(open, std::memory_order_release); }
  // Main thread: the operator chose the ROM's own service menu. Lasts until
  // the door closes.
  void SetPassthrough(bool passthrough) { m_passthrough.store(passthrough, std::memory_order_release); }
  bool IsPassthrough() const { return m_passthrough.load(std::memory_order_acquire); }
  bool IsDoorOpen() const;

  // Switch thread, for every switch change.
  Route OnSwitch(int number, uint8_t state);

  const CoinDoorButtons& Buttons() const { return m_buttons; }

 private:
  CoinDoorButtons m_buttons;
  int m_doorClosedSwitch = 0;
  // Closed until the boards say otherwise. The boards report every closed
  // switch as soon as the bus is up, so a door that is shut says so at once.
  std::atomic<bool> m_doorClosed{false};
  std::atomic<bool> m_toolsAvailable{false};
  std::atomic<bool> m_toolsOpen{false};
  std::atomic<bool> m_passthrough{false};
  // Switch thread only: where each button's press went.
  std::array<Route, CoinDoorButtons::Count> m_pressRoute{Route::Game, Route::Game, Route::Game, Route::Game};
};
