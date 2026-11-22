#include "ScoreboardModel.h"

#include <algorithm>
#include <cmath>
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
  if (spots.count("center") == 0)
  {
    Spot center;
    center.x = windowX + windowW / 2;
    center.y = windowY + windowH / 2;
    center.size = std::max(24, windowH / 6);
    spots["center"] = center;
  }
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
  for (const auto& [name, spot] : spots)
  {
    if (styles.count(spot.style) == 0)
      problems.push_back("callout spot '" + name + "': style '" + spot.style + "' is not defined");
    if (spot.size <= 0) problems.push_back("callout spot '" + name + "': size must be positive");
  }

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
  return hasGame == other.hasGame && inGame == other.inGame && players == other.players && current == other.current &&
         ball == other.ball && credits == other.credits && scores == other.scores && playerVars == other.playerVars &&
         ballVars == other.ballVars;
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
  if (m_observer) m_observer("value." + name, value);
}

void Feed::PushCallout(const Callout& callout)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  static constexpr size_t kMaxQueued = 8;
  if (m_callouts.size() >= kMaxQueued) m_callouts.erase(m_callouts.begin());
  m_callouts.push_back(callout);
  if (m_observer) m_observer("callout", FormatTimelineCallout(callout));
}

static const char* EffectName(Effect effect)
{
  switch (effect)
  {
    case Effect::Pop:
      return "pop";
    case Effect::Fade:
      return "fade";
    case Effect::Slide:
      return "slide";
    case Effect::Shake:
      return "shake";
  }
  return "pop";
}

std::string FormatTimelineCallout(const Callout& callout)
{
  // '|' separates the fields; a text cannot carry one.
  std::string text = callout.text;
  for (char& c : text)
    if (c == '|' || c == '\n') c = ' ';
  std::string out = "text=" + text + "|effect=" + EffectName(callout.effect) + "|spot=" + callout.spot +
                    "|priority=" + std::to_string(callout.priority) + "|ms=" + std::to_string(callout.durationMs);
  if (!callout.style.empty()) out += "|style=" + callout.style;
  if (callout.size > 0) out += "|size=" + std::to_string(callout.size);
  return out;
}

bool ParseTimelineCallout(const std::string& spec, Callout* out)
{
  Callout callout;
  size_t start = 0;
  while (start <= spec.size())
  {
    const size_t bar = spec.find('|', start);
    const std::string part = spec.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    start = bar == std::string::npos ? spec.size() + 1 : bar + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = part.substr(0, eq);
    const std::string value = part.substr(eq + 1);
    if (key == "text")
      callout.text = value;
    else if (key == "spot")
      callout.spot = value;
    else if (key == "style")
      callout.style = value;
    else if (key == "size")
      callout.size = std::atoi(value.c_str());
    else if (key == "ms")
      callout.durationMs = static_cast<uint32_t>(std::atoi(value.c_str()));
    else if (key == "priority")
      callout.priority = std::atoi(value.c_str());
    else if (key == "effect")
    {
      if (!Callout::ParseEffect(value, &callout.effect)) return false;
    }
    else
      return false;
  }
  *out = callout;
  return !callout.text.empty();
}

std::vector<std::pair<std::string, std::string>> DiffFacts(const Facts& before, const Facts& after,
                                                           const Layout& layout)
{
  std::vector<std::pair<std::string, std::string>> out;
  auto put = [&out](const std::string& what, int64_t value) { out.emplace_back(what, std::to_string(value)); };
  if (after.inGame != before.inGame) put("ingame", after.inGame ? 1 : 0);
  if (after.players != before.players) put("players", after.players);
  if (after.current != before.current) put("current", after.current);
  if (after.ball != before.ball) put("ball", after.ball);
  if (after.credits != before.credits) put("credits", after.credits);
  for (size_t i = 0; i < after.scores.size(); ++i)
  {
    if (after.scores[i] != before.scores[i])
      out.emplace_back("score" + std::to_string(i + 1), std::to_string(after.scores[i]));
  }
  for (size_t i = 0; i < after.playerVars.size() && i < layout.playerVarNames.size(); ++i)
  {
    if (i >= before.playerVars.size() || after.playerVars[i] != before.playerVars[i])
      put("var." + layout.playerVarNames[i], after.playerVars[i]);
  }
  for (size_t i = 0; i < after.ballVars.size() && i < layout.ballVarNames.size(); ++i)
  {
    if (i >= before.ballVars.size() || after.ballVars[i] != before.ballVars[i])
      put("ballvar." + layout.ballVarNames[i], after.ballVars[i]);
  }
  return out;
}

void Feed::TakeCallouts(std::vector<Callout>* out)
{
  out->clear();
  std::lock_guard<std::mutex> lock(m_mutex);
  out->swap(m_callouts);
}

bool Feed::HasCallouts() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return !m_callouts.empty();
}

bool Callout::ParseEffect(const std::string& text, Effect* out)
{
  if (text == "pop")
    *out = Effect::Pop;
  else if (text == "fade")
    *out = Effect::Fade;
  else if (text == "slide")
    *out = Effect::Slide;
  else if (text == "shake")
    *out = Effect::Shake;
  else
    return false;
  return true;
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// Fast start, soft landing.
static float EaseOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

CalloutPose PoseAt(const Callout& callout, uint64_t elapsedMs)
{
  CalloutPose pose;
  const float total = static_cast<float>(std::max<uint32_t>(callout.durationMs, 1));
  const float t = static_cast<float>(elapsedMs);
  if (t >= total) return pose;
  pose.visible = true;
  // In over the first 180 ms (or a fifth of a short callout), out over the
  // last 300 ms (or a third).
  const float in = std::min(180.0f, total / 5.0f);
  const float out = std::min(300.0f, total / 3.0f);
  const float rise = Clamp01(t / in);
  const float fall = Clamp01((total - t) / out);
  switch (callout.effect)
  {
    case Effect::Fade:
      pose.alpha = std::min(rise, fall);
      break;
    case Effect::Slide:
      pose.alpha = std::min(rise, fall);
      pose.dy = (1.0f - EaseOut(rise)) * 0.8f;
      break;
    case Effect::Pop:
    case Effect::Shake:
      // From 1.6 times its size down to its size, with a small overshoot.
      pose.scale = rise < 1.0f ? 1.6f - 0.7f * EaseOut(rise) : 0.9f;
      if (rise >= 1.0f)
      {
        const float settle = Clamp01((t - in) / 120.0f);
        pose.scale = 0.9f + 0.1f * EaseOut(settle);
      }
      pose.alpha = std::min(Clamp01(rise * 2.0f), fall);
      if (callout.effect == Effect::Shake && rise >= 1.0f)
      {
        // Half a second of shaking after the landing, dying away.
        const float since = t - in;
        const float damp = Clamp01(1.0f - since / 500.0f);
        pose.dx = 0.06f * damp * std::sin(since * 0.09f);
        pose.dy = 0.03f * damp * std::sin(since * 0.13f + 1.0f);
      }
      break;
  }
  return pose;
}

bool CalloutReplaces(const Callout& showing, uint64_t elapsedMs, const Callout& incoming)
{
  return !PoseAt(showing, elapsedMs).visible || incoming.priority >= showing.priority;
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
      return source.kind == Source::Kind::PlayerVar ? VarValue(layout.playerVarNames, facts.playerVars, source.name)
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

std::string PupCustomPos(const Layout& layout, int pupScreen, int screenW, int screenH)
{
  double sw = layout.width, sh = layout.height, fit = 1.0, bx = 0.0, by = 0.0;
  if (screenW > 0 && screenH > 0)
  {
    sw = screenW;
    sh = screenH;
    // The same fit as Board::Draw.
    fit = std::min(sw / layout.width, sh / layout.height);
    bx = std::floor((sw - std::lround(layout.width * fit)) / 2.0);
    by = std::floor((sh - std::lround(layout.height * fit)) / 2.0);
  }
  char text[96];
  std::snprintf(text, sizeof(text), "%d,%.3f,%.3f,%.3f,%.3f", pupScreen, 100.0 * (bx + layout.windowX * fit) / sw,
                100.0 * (by + layout.windowY * fit) / sh, 100.0 * layout.windowW * fit / sw,
                100.0 * layout.windowH * fit / sh);
  return text;
}

}  // namespace scoreboard
