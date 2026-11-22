#include "ScoreboardModel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace scoreboard
{

static bool ParseIndex(const std::string& text, int* out)
{
  if (text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<int>(value);
  return true;
}

bool Source::Parse(const std::string& text, Source* out)
{
  Source source;
  if (text == "ball")
    source.kind = Kind::Ball;
  else if (text == "player")
    source.kind = Kind::Player;
  else if (text == "players")
    source.kind = Kind::Players;
  else if (text == "credits")
    source.kind = Kind::Credits;
  else if (text == "score")
    source.kind = Kind::Score;
  else if (text.size() == 6 && text.compare(0, 5, "score") == 0 && text[5] >= '1' && text[5] <= '4')
  {
    source.kind = Kind::PlayerScore;
    source.index = text[5] - '0';
  }
  else if (text.compare(0, 7, "player.") == 0 && text.size() > 7)
  {
    source.kind = Kind::PlayerVar;
    source.name = text.substr(7);
  }
  else if (text.compare(0, 5, "ball.") == 0 && text.size() > 5)
  {
    source.kind = Kind::BallVar;
    source.name = text.substr(5);
  }
  else if (text.compare(0, 6, "value.") == 0 && text.size() > 6)
  {
    source.kind = Kind::Value;
    source.name = text.substr(6);
  }
  else
  {
    return false;
  }
  *out = source;
  return true;
}

void Layout::Finish()
{
  CollectVars(&playerVarNames, &ballVarNames);
  if (styles.count("burnt") == 0)
  {
    styles["burnt"] = Style{};
  }
  if (styles.count("ember") == 0)
  {
    Style ember;
    ember.core = 0xffc23c;
    ember.scorch = 0xd2500a;
    ember.scorchStrength = 0.75f;
    ember.scorchReach = 0.10f;
    ember.glow = true;
    styles["ember"] = ember;
  }
}

// Every {placeholder} of a text, without the braces.
static std::vector<std::string> Placeholders(const std::string& text)
{
  std::vector<std::string> names;
  size_t pos = 0;
  while ((pos = text.find('{', pos)) != std::string::npos)
  {
    const size_t close = text.find('}', pos + 1);
    if (close == std::string::npos) break;
    names.push_back(text.substr(pos + 1, close - pos - 1));
    pos = close + 1;
  }
  return names;
}

// "player.rank:ranks" -> source "player.rank", list "ranks".
static void SplitPlaceholder(const std::string& placeholder, std::string* source, std::string* list)
{
  const size_t colon = placeholder.find(':');
  *source = placeholder.substr(0, colon);
  *list = colon == std::string::npos ? std::string() : placeholder.substr(colon + 1);
}

std::vector<std::string> Layout::Validate() const
{
  std::vector<std::string> problems;
  if (width <= 0 || height <= 0) problems.push_back("size must be two positive numbers");
  if (font.empty()) problems.push_back("no font");
  if (windowW <= 0 || windowH <= 0 || windowX < 0 || windowY < 0 || windowX + windowW > width ||
      windowY + windowH > height)
  {
    problems.push_back("the window does not fit the size");
  }
  if (logoSize < 48) problems.push_back("logo size below 48: the PPUC brand has to be readable");
  if (styles.count(logoStyle) == 0) problems.push_back("logo style '" + logoStyle + "' is not defined");

  for (size_t i = 0; i < fields.size(); ++i)
  {
    const Field& field = fields[i];
    const std::string where = "field " + std::to_string(i + 1) + ": ";
    if (styles.count(field.style) == 0) problems.push_back(where + "style '" + field.style + "' is not defined");
    if (field.size <= 0) problems.push_back(where + "size must be positive");
    switch (field.type)
    {
      case Field::Type::Text:
        for (const std::string& placeholder : Placeholders(field.text))
        {
          std::string sourceText;
          std::string list;
          SplitPlaceholder(placeholder, &sourceText, &list);
          Source source;
          if (!Source::Parse(sourceText, &source))
            problems.push_back(where + "'{" + placeholder + "}' is not a value");
          else if (!list.empty() && lists.count(list) == 0)
            problems.push_back(where + "list '" + list + "' is not defined");
        }
        break;
      case Field::Type::OtherScores:
        if (field.width <= 0) problems.push_back(where + "other_scores needs a width");
        break;
      case Field::Type::Icons:
        if (field.source.kind == Source::Kind::None) problems.push_back(where + "icons need a source");
        if (field.count <= 0 || field.count > 64) problems.push_back(where + "icons need a count from 1 to 64");
        if (field.on.empty()) problems.push_back(where + "icons need an 'on' picture");
        break;
      case Field::Type::CurrentScore:
        break;
    }
  }
  return problems;
}

static void AddUnique(std::vector<std::string>* names, const std::string& name)
{
  if (std::find(names->begin(), names->end(), name) == names->end()) names->push_back(name);
}

static void CollectSource(const Source& source, std::vector<std::string>* playerVars,
                          std::vector<std::string>* ballVars)
{
  if (source.kind == Source::Kind::PlayerVar) AddUnique(playerVars, source.name);
  if (source.kind == Source::Kind::BallVar) AddUnique(ballVars, source.name);
}

void Layout::CollectVars(std::vector<std::string>* playerVars, std::vector<std::string>* ballVars) const
{
  playerVars->clear();
  ballVars->clear();
  for (const Field& field : fields)
  {
    if (field.type == Field::Type::Icons) CollectSource(field.source, playerVars, ballVars);
    if (field.type != Field::Type::Text) continue;
    for (const std::string& placeholder : Placeholders(field.text))
    {
      std::string sourceText;
      std::string list;
      SplitPlaceholder(placeholder, &sourceText, &list);
      Source source;
      if (Source::Parse(sourceText, &source)) CollectSource(source, playerVars, ballVars);
    }
  }
}

bool Facts::operator==(const Facts& other) const
{
  return hasGame == other.hasGame && inGame == other.inGame && players == other.players &&
         current == other.current && ball == other.ball && credits == other.credits && scores == other.scores &&
         playerVars == other.playerVars && ballVars == other.ballVars;
}

void Feed::SetVars(std::vector<std::string> playerVars, std::vector<std::string> ballVars)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_playerVarNames = std::move(playerVars);
  m_ballVarNames = std::move(ballVars);
}

void Feed::Publish(const Facts& facts)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_snapshot.facts == facts) return;
  m_snapshot.facts = facts;
  ++m_snapshot.version;
}

void Feed::SetValue(const std::string& name, const std::string& value)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_snapshot.values.find(name);
  if (value.empty())
  {
    if (it == m_snapshot.values.end()) return;
    m_snapshot.values.erase(it);
  }
  else
  {
    if (it != m_snapshot.values.end() && it->second == value) return;
    m_snapshot.values[name] = value;
  }
  ++m_snapshot.version;
}

bool Feed::ReadIfNewer(uint64_t version, Snapshot* out) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_snapshot.version == version) return false;
  *out = m_snapshot;
  return true;
}

uint64_t Feed::Version() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_snapshot.version;
}

std::string FormatScore(uint64_t score)
{
  std::string digits = std::to_string(score);
  std::string out;
  const int n = static_cast<int>(digits.size());
  for (int i = 0; i < n; ++i)
  {
    if (i > 0 && (n - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

static int64_t VarValue(const std::vector<std::string>& names, const std::vector<int64_t>& values,
                        const std::string& name)
{
  for (size_t i = 0; i < names.size() && i < values.size(); ++i)
  {
    if (names[i] == name) return values[i];
  }
  return 0;
}

// The machine thread copies the variables in the order the layout collected
// them in Finish().
int64_t SourceValue(const Source& source, const Layout& layout, const Snapshot& snapshot)
{
  const Facts& facts = snapshot.facts;
  switch (source.kind)
  {
    case Source::Kind::None:
      return 0;
    case Source::Kind::Ball:
      return facts.ball;
    case Source::Kind::Player:
      return facts.current;
    case Source::Kind::Players:
      return facts.players;
    case Source::Kind::Credits:
      return facts.credits;
    case Source::Kind::Score:
      return facts.current >= 1 && facts.current <= 4 ? static_cast<int64_t>(facts.scores[facts.current - 1]) : 0;
    case Source::Kind::PlayerScore:
      return source.index >= 1 && source.index <= 4 ? static_cast<int64_t>(facts.scores[source.index - 1]) : 0;
    case Source::Kind::PlayerVar:
    case Source::Kind::BallVar:
      return source.kind == Source::Kind::PlayerVar
                 ? VarValue(layout.playerVarNames, facts.playerVars, source.name)
                 : VarValue(layout.ballVarNames, facts.ballVars, source.name);
    case Source::Kind::Value:
    {
      const auto it = snapshot.values.find(source.name);
      return it == snapshot.values.end() ? 0 : std::strtoll(it->second.c_str(), nullptr, 10);
    }
  }
  return 0;
}

static std::string SourceText(const Source& source, const std::string& list, const Layout& layout,
                              const Snapshot& snapshot)
{
  if (source.kind == Source::Kind::Value && list.empty())
  {
    const auto it = snapshot.values.find(source.name);
    return it == snapshot.values.end() ? std::string() : it->second;
  }
  const int64_t value = SourceValue(source, layout, snapshot);
  if (!list.empty())
  {
    const auto it = layout.lists.find(list);
    if (it == layout.lists.end() || value < 0 || value >= static_cast<int64_t>(it->second.size())) return {};
    return it->second[static_cast<size_t>(value)];
  }
  if (source.kind == Source::Kind::Score || source.kind == Source::Kind::PlayerScore)
  {
    return FormatScore(static_cast<uint64_t>(value < 0 ? 0 : value));
  }
  return std::to_string(value);
}

std::string ExpandText(const std::string& text, const Layout& layout, const Snapshot& snapshot)
{
  std::string out;
  size_t pos = 0;
  while (pos < text.size())
  {
    const size_t open = text.find('{', pos);
    if (open == std::string::npos)
    {
      out += text.substr(pos);
      break;
    }
    out += text.substr(pos, open - pos);
    const size_t close = text.find('}', open + 1);
    if (close == std::string::npos)
    {
      out += text.substr(open);
      break;
    }
    std::string sourceText;
    std::string list;
    SplitPlaceholder(text.substr(open + 1, close - open - 1), &sourceText, &list);
    Source source;
    if (Source::Parse(sourceText, &source)) out += SourceText(source, list, layout, snapshot);
    pos = close + 1;
  }
  return out;
}

bool FieldVisible(const Field& field, const Snapshot& snapshot)
{
  switch (field.when)
  {
    case When::Always:
      return true;
    case When::Game:
      return snapshot.facts.inGame;
    case When::Attract:
      return !snapshot.facts.inGame;
  }
  return true;
}

std::vector<bool> IconsLit(const Field& field, const Layout& layout, const Snapshot& snapshot)
{
  std::vector<bool> lit(static_cast<size_t>(std::max(field.count, 0)), false);
  const int64_t value = SourceValue(field.source, layout, snapshot);
  for (int i = 0; i < field.count; ++i)
  {
    lit[static_cast<size_t>(i)] = field.mask ? ((static_cast<uint64_t>(value) >> i) & 1u) != 0 : i < value;
  }
  return lit;
}

int FeaturedPlayer(const Snapshot& snapshot)
{
  const Facts& facts = snapshot.facts;
  if (facts.players == 0) return 0;
  if (facts.inGame) return facts.current;
  int best = 1;
  for (int p = 2; p <= facts.players && p <= 4; ++p)
  {
    if (facts.scores[p - 1] > facts.scores[best - 1]) best = p;
  }
  return best;
}

std::vector<int> OtherPlayers(const Snapshot& snapshot)
{
  std::vector<int> others;
  const int featured = FeaturedPlayer(snapshot);
  for (int p = 1; p <= snapshot.facts.players && p <= 4; ++p)
  {
    if (p != featured) others.push_back(p);
  }
  return others;
}

std::vector<uint32_t> DecodeUtf8(const std::string& text)
{
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < text.size())
  {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    int extra = 0;
    uint32_t cp = 0;
    if (c < 0x80)
    {
      cp = c;
    }
    else if ((c & 0xe0) == 0xc0)
    {
      cp = c & 0x1f;
      extra = 1;
    }
    else if ((c & 0xf0) == 0xe0)
    {
      cp = c & 0x0f;
      extra = 2;
    }
    else if ((c & 0xf8) == 0xf0)
    {
      cp = c & 0x07;
      extra = 3;
    }
    else
    {
      out.push_back('?');
      ++i;
      continue;
    }
    bool ok = true;
    for (int k = 1; k <= extra; ++k)
    {
      if (i + k >= text.size() || (static_cast<uint8_t>(text[i + k]) & 0xc0) != 0x80)
      {
        ok = false;
        break;
      }
      cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3f);
    }
    if (!ok)
    {
      out.push_back('?');
      ++i;
      continue;
    }
    out.push_back(cp);
    i += 1 + extra;
  }
  return out;
}

std::string PupCustomPos(const Layout& layout, int pupScreen)
{
  char text[96];
  std::snprintf(text, sizeof(text), "%d,%.3f,%.3f,%.3f,%.3f", pupScreen, 100.0 * layout.windowX / layout.width,
                100.0 * layout.windowY / layout.height, 100.0 * layout.windowW / layout.width,
                100.0 * layout.windowH / layout.height);
  return text;
}

}  // namespace scoreboard
