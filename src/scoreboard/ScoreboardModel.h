#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The scoreboard: a game's own full screen around the scene videos -- a frame,
// the scores of up to four players, and whatever the game wants to show at the
// sides -- drawn by PPUC on top of whatever PUP plays, or on its own.
//
// This file is the part that knows nothing about SDL: the layout as read from
// `scoreboard.yaml`, the facts it shows, how they get from the machine thread
// to the thread that draws, and how a text field turns into words. The drawing
// is in Scoreboard.h.
//
// Why PPUC draws this and not a PUP pack: everything on it changes only when
// something happens in the game, so it is drawn from a handful of pictures
// prepared when the game loads. Only the scene in the middle moves every frame.
// Videos for scores would cost a decoder each and still not know the score.

namespace scoreboard
{

// Where a value comes from.
struct Source
{
  enum class Kind
  {
    None,
    Ball,
    Player,       // the current player's number, 1-4
    Players,      // how many are playing
    Credits,
    Score,        // the current player's score
    PlayerScore,  // `index` 1-4
    PlayerVar,    // a GameCore player variable of the current player, `name`
    BallVar,      // a GameCore ball variable, `name`
    Value,        // set by the rules: ppuc.scoreboardSet(`name`, value)
  };
  Kind kind = Kind::None;
  std::string name;
  int index = 0;

  // "ball", "player", "players", "credits", "score", "score3", "player.rank",
  // "ball.bullets_hit", "value.mode". False for anything else.
  static bool Parse(const std::string& text, Source* out);
};

// When a field shows.
enum class When
{
  Always,
  Game,     // a game is running
  Attract,  // no game running
};

enum class Align
{
  Left,
  Center,
  Right,
};

struct Field
{
  enum class Type
  {
    // Text with {placeholders}: "Ball {ball}", "{player.rank:ranks}".
    Text,
    // The current player's score, large. After a game, the high score.
    CurrentScore,
    // Everybody else's score, spread over a row: "P1 12,450,000".
    OtherScores,
    // A row or grid of pictures, lit by a count or a bit mask.
    Icons,
  };
  Type type = Type::Text;
  When when = When::Always;
  // Layout coordinates. Text: x is the anchor given by `align`, y the top.
  int x = 0;
  int y = 0;
  // OtherScores: the row's width, the scores are centred in equal parts of it.
  int width = 0;
  int size = 40;  // font pixel height
  Align align = Align::Center;
  std::string style = "burnt";

  std::string text;  // Text
  // OtherScores: one entry, "{n}" the player, "{score}" the score.
  std::string format = "P{n}  {score}";

  // Icons
  Source source;
  bool mask = false;  // lit by bit i, rather than the first `value` lit
  int count = 0;
  int dx = 0;
  int dy = 0;
  int columns = 0;  // 0: all in one line
  std::string on;   // pictures, relative to the scoreboard folder
  std::string off;
  int iconWidth = 0;  // 0: the picture's own size
  int iconHeight = 0;
};

// How burnt text looks. Every colour is 0xRRGGBB.
struct Style
{
  // The letters themselves.
  uint32_t core = 0x24120a;
  // The scorch around them, darkening the wood.
  uint32_t scorch = 0x4a240c;
  // 0..1: how dark the scorch gets right at the edge of a letter.
  float scorchStrength = 0.55f;
  // How far the scorch reaches, as a fraction of the font size.
  float scorchReach = 0.08f;
  // True: the letters glow like a brand fresh from the fire.
  bool glow = false;
};

enum class Corner
{
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};

struct Layout
{
  int width = 1920;
  int height = 1080;
  // Pictures and the font, relative to the scoreboard folder.
  std::string frame;
  std::string idle;
  std::string font;
  // Where the scene shows through the frame.
  int windowX = 320;
  int windowY = 72;
  int windowW = 1280;
  int windowH = 720;
  // The PPUC brand is on every scoreboard: only where it goes is a choice.
  Corner logoCorner = Corner::TopRight;
  int logoSize = 150;
  int logoMargin = 24;
  std::string logoStyle = "burnt";
  std::map<std::string, Style> styles;
  // Named lists for {player.rank:ranks}: a number picks an entry.
  std::map<std::string, std::vector<std::string>> lists;
  std::vector<Field> fields;

  // The PlayerVar and BallVar names, in the order Facts carries them. Set by
  // Finish().
  std::vector<std::string> playerVarNames;
  std::vector<std::string> ballVarNames;

  // Call once the fields are in: adds the "burnt" and "ember" styles when the
  // layout does not define them, and collects the variables.
  void Finish();
  // Empty when the layout can be drawn; otherwise one line per problem.
  std::vector<std::string> Validate() const;
  // The PlayerVar and BallVar sources any field reads, so the machine thread
  // knows what to copy.
  void CollectVars(std::vector<std::string>* playerVars, std::vector<std::string>* ballVars) const;
};

// What the scoreboard shows at one moment. Copied from the machine thread.
struct Facts
{
  bool hasGame = false;  // there is a game core at all
  bool inGame = false;
  uint8_t players = 0;
  uint8_t current = 0;
  uint8_t ball = 0;
  uint16_t credits = 0;
  std::array<uint64_t, 4> scores{0, 0, 0, 0};
  // In the order of the layout's CollectVars().
  std::vector<int64_t> playerVars;
  std::vector<int64_t> ballVars;

  bool operator==(const Facts& other) const;
  bool operator!=(const Facts& other) const { return !(*this == other); }
};

struct Snapshot
{
  Facts facts;
  std::map<std::string, std::string> values;
  // Changes whenever anything above does.
  uint64_t version = 0;
};

// From the machine thread to the drawing thread. The machine side never waits
// for the drawing: it takes the lock for a copy of a few numbers, and only when
// they changed.
class Feed
{
 public:
  void SetVars(std::vector<std::string> playerVars, std::vector<std::string> ballVars);
  const std::vector<std::string>& PlayerVarNames() const { return m_playerVarNames; }
  const std::vector<std::string>& BallVarNames() const { return m_ballVarNames; }

  // Machine thread.
  void Publish(const Facts& facts);
  // Machine thread (the rules). An empty value removes it.
  void SetValue(const std::string& name, const std::string& value);

  // Drawing thread: true, and a copy, when something changed since `version`.
  bool ReadIfNewer(uint64_t version, Snapshot* out) const;
  uint64_t Version() const;

 private:
  std::vector<std::string> m_playerVarNames;
  std::vector<std::string> m_ballVarNames;
  mutable std::mutex m_mutex;
  Snapshot m_snapshot;
};

// "12450000" -> "12,450,000".
std::string FormatScore(uint64_t score);
// {placeholders} in a Text field filled from a snapshot. Unknown ones are left
// out rather than shown, so a typo shows as a gap and not as braces on a
// machine's screen.
std::string ExpandText(const std::string& text, const Layout& layout, const Snapshot& snapshot);
// A source's number, 0 when there is none.
int64_t SourceValue(const Source& source, const Layout& layout, const Snapshot& snapshot);
bool FieldVisible(const Field& field, const Snapshot& snapshot);
// Which of an Icons field's pictures are lit.
std::vector<bool> IconsLit(const Field& field, const Layout& layout, const Snapshot& snapshot);
// The players an OtherScores field lists: everybody playing but the current
// one, in order. After a game, everybody who played but the high scorer, whose
// score is the big one then.
std::vector<int> OtherPlayers(const Snapshot& snapshot);
// The player whose score is the big one: the current one, or after a game the
// best. 0 when no game was played.
int FeaturedPlayer(const Snapshot& snapshot);

// UTF-8 to code points; bad bytes become '?'.
std::vector<uint32_t> DecodeUtf8(const std::string& text);

// For the PUP pack: the screen position that puts a video exactly in the
// window, as PUP's CustomPos wants it ("screen,x%,y%,w%,h%").
std::string PupCustomPos(const Layout& layout, int pupScreen);

}  // namespace scoreboard
