#include "CoinDoor.h"

#include <cstdlib>
#include <sstream>
#include <vector>

// libppuc's platform numbering (PLATFORM_WPC in io-boards' PPUCPlatforms.h).
static constexpr uint8_t kPlatformWpc = 1;

int CoinDoorButtons::ButtonFor(int number) const
{
  if (number == 0) return -1;
  for (int i = 0; i < Count; ++i)
  {
    if (numbers[i] == number) return i;
  }
  return -1;
}

CoinDoorButtons CoinDoorButtons::ForPlatform(uint8_t platform)
{
  CoinDoorButtons buttons;
  if (platform == kPlatformWpc)
  {
    buttons.numbers = {5, 6, 7, 8};
  }
  return buttons;
}

bool CoinDoorButtons::Parse(const std::string& text, CoinDoorButtons* out, std::string* error)
{
  std::vector<int> values;
  std::stringstream in(text);
  std::string item;
  while (std::getline(in, item, ','))
  {
    size_t begin = item.find_first_not_of(" \t");
    size_t end = item.find_last_not_of(" \t");
    if (begin == std::string::npos)
    {
      if (error) *error = "an empty entry in '" + text + "'";
      return false;
    }
    item = item.substr(begin, end - begin + 1);
    char* parsedEnd = nullptr;
    const long value = std::strtol(item.c_str(), &parsedEnd, 10);
    if (parsedEnd == item.c_str() || *parsedEnd != '\0' || value < 0 || value > 255)
    {
      if (error) *error = "'" + item + "' is not a switch number";
      return false;
    }
    values.push_back(static_cast<int>(value));
  }
  if (values.size() != Count)
  {
    if (error) *error = "four switch numbers expected (escape, down, up, enter), got '" + text + "'";
    return false;
  }
  for (int i = 0; i < Count; ++i) out->numbers[i] = values[i];
  return true;
}

std::string CoinDoorButtons::Describe() const
{
  static const char* const kNames[Count] = {"escape", "down", "up", "enter"};
  std::string text;
  for (int i = 0; i < Count; ++i)
  {
    if (i) text += ", ";
    text += kNames[i];
    text += " ";
    text += numbers[i] ? std::to_string(numbers[i]) : std::string("-");
  }
  return text;
}

void CoinDoorRouter::Configure(const CoinDoorButtons& buttons, int doorClosedSwitch)
{
  m_buttons = buttons;
  m_doorClosedSwitch = doorClosedSwitch;
  m_pressRoute.fill(Route::Game);
}

bool CoinDoorRouter::IsDoorOpen() const
{
  return m_doorClosedSwitch == 0 || !m_doorClosed.load(std::memory_order_acquire);
}

CoinDoorRouter::Route CoinDoorRouter::OnSwitch(int number, uint8_t state)
{
  if (m_doorClosedSwitch != 0 && number == m_doorClosedSwitch)
  {
    m_doorClosed.store(state != 0, std::memory_order_release);
    if (state != 0)
    {
      // The ROM's service menu ends with the door: whatever the buttons do
      // next time it is opened, they start out as PPUC's.
      m_passthrough.store(false, std::memory_order_release);
    }
    return Route::Game;
  }

  const int button = m_buttons.ButtonFor(number);
  if (button < 0)
  {
    return Route::Game;
  }

  if (state == 0)
  {
    const Route route = m_pressRoute[button];
    m_pressRoute[button] = Route::Game;
    return route;
  }

  Route route = Route::Game;
  if (!m_passthrough.load(std::memory_order_acquire))
  {
    if (m_toolsOpen.load(std::memory_order_acquire))
    {
      route = Route::Tools;
    }
    else if (button == CoinDoorButtons::Enter && IsDoorOpen() && m_toolsAvailable.load(std::memory_order_acquire))
    {
      route = Route::Tools;
    }
  }
  m_pressRoute[button] = route;
  return route;
}
