#include "ScoreboardYaml.h"

#include <yaml-cpp/yaml.h>

#include <cstdlib>

namespace scoreboard
{

namespace
{

struct LayoutError
{
  std::string message;
};

[[noreturn]] void Fail(const YAML::Node& node, const std::string& message)
{
  const YAML::Mark mark = node.Mark();
  if (mark.line >= 0)
  {
    throw LayoutError{"line " + std::to_string(mark.line + 1) + ": " + message};
  }
  throw LayoutError{message};
}

int ReadInt(const YAML::Node& node, const char* key, int fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  try
  {
    return value.as<int>();
  }
  catch (const std::exception&)
  {
    Fail(value, std::string(key) + " must be a whole number");
  }
}

float ReadFloat(const YAML::Node& node, const char* key, float fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  try
  {
    return value.as<float>();
  }
  catch (const std::exception&)
  {
    Fail(value, std::string(key) + " must be a number");
  }
}

std::string ReadString(const YAML::Node& node, const char* key, const std::string& fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  if (!value.IsScalar()) Fail(value, std::string(key) + " must be text");
  return value.as<std::string>();
}

uint32_t ReadColor(const YAML::Node& node, const char* key, uint32_t fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  std::string text = value.as<std::string>();
  if (!text.empty() && text[0] == '#') text = text.substr(1);
  char* end = nullptr;
  const unsigned long rgb = std::strtoul(text.c_str(), &end, 16);
  if (text.size() != 6 || end == nullptr || *end != '\0')
  {
    Fail(value, std::string(key) + " must be a colour like \"5a2c0c\"");
  }
  return static_cast<uint32_t>(rgb);
}

void ReadPair(const YAML::Node& node, const char* key, int* a, int* b)
{
  const YAML::Node value = node[key];
  if (!value) return;
  if (!value.IsSequence() || value.size() != 2) Fail(value, std::string(key) + " must be [a, b]");
  *a = value[0].as<int>();
  *b = value[1].as<int>();
}

Align ReadAlign(const YAML::Node& node)
{
  const std::string text = ReadString(node, "align", "center");
  if (text == "left") return Align::Left;
  if (text == "right") return Align::Right;
  if (text == "center") return Align::Center;
  Fail(node["align"], "align must be left, center or right");
}

When ReadWhen(const YAML::Node& node)
{
  const std::string text = ReadString(node, "when", "always");
  if (text == "always") return When::Always;
  if (text == "game") return When::Game;
  if (text == "attract") return When::Attract;
  Fail(node["when"], "when must be always, game or attract");
}

Corner ReadCorner(const YAML::Node& node, const char* key, Corner fallback)
{
  const YAML::Node value = node[key];
  if (!value) return fallback;
  const std::string text = value.as<std::string>();
  if (text == "top-left") return Corner::TopLeft;
  if (text == "top-right") return Corner::TopRight;
  if (text == "bottom-left") return Corner::BottomLeft;
  if (text == "bottom-right") return Corner::BottomRight;
  Fail(value, "corner must be top-left, top-right, bottom-left or bottom-right");
}

Field ReadField(const YAML::Node& node)
{
  if (!node.IsMap()) Fail(node, "a field must be a map");
  Field field;
  const std::string type = ReadString(node, "type", "");
  if (type == "text")
    field.type = Field::Type::Text;
  else if (type == "current_score")
    field.type = Field::Type::CurrentScore;
  else if (type == "other_scores")
    field.type = Field::Type::OtherScores;
  else if (type == "icons")
    field.type = Field::Type::Icons;
  else
    Fail(node, "type must be text, current_score, other_scores or icons, not '" + type + "'");

  field.when = ReadWhen(node);
  field.x = ReadInt(node, "x", 0);
  field.y = ReadInt(node, "y", 0);
  field.width = ReadInt(node, "width", 0);
  field.size = ReadInt(node, "size", 40);
  field.align = ReadAlign(node);
  field.style = ReadString(node, "style", "burnt");
  field.text = ReadString(node, "text", "");
  field.format = ReadString(node, "format", field.format);

  if (field.type == Field::Type::Icons)
  {
    const std::string source = ReadString(node, "source", "");
    if (!Source::Parse(source, &field.source)) Fail(node, "'" + source + "' is not a value to light icons with");
    const std::string lit = ReadString(node, "lit", "count");
    if (lit != "count" && lit != "mask") Fail(node["lit"], "lit must be count or mask");
    field.mask = lit == "mask";
    field.count = ReadInt(node, "count", 0);
    field.dx = ReadInt(node, "dx", 0);
    field.dy = ReadInt(node, "dy", 0);
    field.columns = ReadInt(node, "columns", 0);
    field.on = ReadString(node, "on", "");
    field.off = ReadString(node, "off", "");
    ReadPair(node, "icon_size", &field.iconWidth, &field.iconHeight);
  }
  return field;
}

Style ReadStyle(const YAML::Node& node, Style style)
{
  style.core = ReadColor(node, "core", style.core);
  style.scorch = ReadColor(node, "scorch", style.scorch);
  style.scorchStrength = ReadFloat(node, "scorch_strength", style.scorchStrength);
  style.scorchReach = ReadFloat(node, "scorch_reach", style.scorchReach);
  if (node["glow"]) style.glow = node["glow"].as<bool>();
  return style;
}

}  // namespace

bool LoadLayout(const std::string& path, Layout* pLayout, std::string* pError)
{
  Layout layout;
  try
  {
    const YAML::Node file = YAML::LoadFile(path);
    const YAML::Node root = file["scoreboard"];
    if (!root || !root.IsMap()) Fail(file, "no 'scoreboard:' block");

    ReadPair(root, "size", &layout.width, &layout.height);
    layout.frame = ReadString(root, "frame", "");
    layout.idle = ReadString(root, "idle", "");
    layout.font = ReadString(root, "font", "");
    if (const YAML::Node window = root["window"])
    {
      if (!window.IsSequence() || window.size() != 4) Fail(window, "window must be [x, y, width, height]");
      layout.windowX = window[0].as<int>();
      layout.windowY = window[1].as<int>();
      layout.windowW = window[2].as<int>();
      layout.windowH = window[3].as<int>();
    }
    if (const YAML::Node logo = root["logo"])
    {
      layout.logoCorner = ReadCorner(logo, "corner", layout.logoCorner);
      layout.logoSize = ReadInt(logo, "size", layout.logoSize);
      layout.logoMargin = ReadInt(logo, "margin", layout.logoMargin);
      layout.logoStyle = ReadString(logo, "style", layout.logoStyle);
    }

    // The two built-in styles first, so a layout can change one colour of
    // "ember" without restating the rest.
    layout.Finish();
    if (const YAML::Node styles = root["styles"])
    {
      if (!styles.IsMap()) Fail(styles, "styles must be a map of names");
      for (const auto& entry : styles)
      {
        const std::string name = entry.first.as<std::string>();
        const auto existing = layout.styles.find(name);
        layout.styles[name] = ReadStyle(entry.second, existing != layout.styles.end() ? existing->second : Style{});
      }
    }
    if (const YAML::Node lists = root["lists"])
    {
      if (!lists.IsMap()) Fail(lists, "lists must be a map of names");
      for (const auto& entry : lists)
      {
        if (!entry.second.IsSequence()) Fail(entry.second, "a list must be a sequence");
        std::vector<std::string> items;
        for (const auto& item : entry.second) items.push_back(item.as<std::string>());
        layout.lists[entry.first.as<std::string>()] = std::move(items);
      }
    }
    if (const YAML::Node callouts = root["callouts"])
    {
      if (!callouts.IsMap()) Fail(callouts, "callouts must be a map");
      if (const YAML::Node spots = callouts["spots"])
      {
        if (!spots.IsMap()) Fail(spots, "spots must be a map of names");
        for (const auto& entry : spots)
        {
          const std::string name = entry.first.as<std::string>();
          const auto existing = layout.spots.find(name);
          Spot spot = existing != layout.spots.end() ? existing->second : Spot{};
          spot.x = ReadInt(entry.second, "x", spot.x);
          spot.y = ReadInt(entry.second, "y", spot.y);
          spot.size = ReadInt(entry.second, "size", spot.size);
          spot.style = ReadString(entry.second, "style", spot.style);
          layout.spots[name] = spot;
        }
      }
      if (const YAML::Node words = callouts["words"])
      {
        if (!words.IsSequence()) Fail(words, "words must be a list");
        for (const auto& word : words) layout.calloutWords.push_back(word.as<std::string>());
      }
    }
    if (const YAML::Node fields = root["fields"])
    {
      if (!fields.IsSequence()) Fail(fields, "fields must be a list");
      for (const auto& node : fields) layout.fields.push_back(ReadField(node));
    }
    layout.Finish();
  }
  catch (const LayoutError& error)
  {
    if (pError) *pError = path + ": " + error.message;
    return false;
  }
  catch (const std::exception& ex)
  {
    if (pError) *pError = path + ": " + ex.what();
    return false;
  }

  const std::vector<std::string> problems = layout.Validate();
  if (!problems.empty())
  {
    if (pError)
    {
      *pError = path + ":";
      for (const std::string& problem : problems) *pError += "\n  " + problem;
    }
    return false;
  }
  *pLayout = std::move(layout);
  return true;
}

}  // namespace scoreboard
