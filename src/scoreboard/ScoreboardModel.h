#pragma once

#include <array>
#include <cstdint>
#include <functional>
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
    Player,   // the current player's number, 1-4
    Players,  // how many are playing
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
  // Text: shown instead of `text` while the machine is on free play, where a
  // credit count means nothing ("Free Play" rather than "Credits 0").
  std::string freePlayText;
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
  // A soft shadow under the letters, so they stand off the wood: 0 for none,
  // up to 1 for black. Offset down and right and softness are fractions of the
  // font size, like the scorch.
  float shadow = 0.0f;
  float shadowOffset = 0.05f;
  float shadowSoftness = 0.04f;
  uint32_t shadowColor = 0x000000;
};

enum class Corner
{
  TopLeft,
  TopRight,
  BottomLeft,
  BottomRight,
};

// A callout: a few words or a number over the scene for a moment --
// "10,000,000" for a shot, "SILVER BULLET" for an award. Made of the same
// burnt glyphs as the rest of the board and moved by the GPU alone: a zoom,
// a fade, a slide or a shake is only a different place, size and opacity for
// glyphs that already exist.
enum class Effect
{
  Pop,    // zooms in from large and settles, then fades
  Fade,   // fades in and out
  Slide,  // rises into place, then fades
  Shake,  // pops, then shakes: the big ones
  Brand,  // burnt in letter by letter, each flaring like a hot iron, then fades
};

// Where callouts go, by name. The layout's "center" is the window's middle
// unless the layout says otherwise.
struct Spot
{
  int x = 0;
  int y = 0;  // the middle of the text, unlike a text field's top
  int size = 120;
  std::string style = "ember";
};

struct Callout
{
  std::string text;
  std::string spot = "center";
  std::string style;  // empty: the spot's
  int size = 0;       // 0: the spot's
  uint32_t durationMs = 1500;
  Effect effect = Effect::Pop;
  // A callout replaces the one showing unless that one's priority is higher,
  // in which case the new one is dropped: a jackpot is not cut short by the
  // points of the next switch.
  int priority = 0;

  // "pop", "fade", "slide", "shake".
  static bool ParseEffect(const std::string& text, Effect* out);
};

// Where a callout is at `elapsedMs`: how big, how opaque, how far off its
// spot. Pure, so the motion is tested rather than looked at.
struct CalloutPose
{
  bool visible = false;
  float scale = 1.0f;
  float alpha = 1.0f;
  float dx = 0.0f;  // in fractions of the text height
  float dy = 0.0f;
};
CalloutPose PoseAt(const Callout& callout, uint64_t elapsedMs);
// One letter of a callout, for effects that move letters one by one (Brand):
// `index` of `count` letters. Scale is around the letter's own middle; flare
// 0..1 is extra heat drawn over it. Every other effect: scale 1, alpha 1.
struct LetterPose
{
  float scale = 1.0f;
  float alpha = 1.0f;
  float flare = 0.0f;
};
LetterPose LetterAt(const Callout& callout, uint64_t elapsedMs, int index, int count);
// Whether `incoming` takes the place of `showing`, which started `elapsedMs`
// ago: always once that one is over, otherwise unless it has the higher
// priority.
bool CalloutReplaces(const Callout& showing, uint64_t elapsedMs, const Callout& incoming);

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
  // The layout's words in other languages, by language code, chosen by the
  // value `language` the rules set (ppuc.scoreboardSet("language", "de")).
  // Without one, or for a language not here, the layout's own words show.
  struct Translation
  {
    std::string thousands = ",";               // 10.000.000 in German
    std::map<std::string, std::string> texts;  // the layout's text -> this language's
  };
  std::map<std::string, Translation> translations;
  std::vector<Field> fields;
  std::map<std::string, Spot> spots;
  // Words callouts will show, burnt at load so the first one does not wait.
  // Digits and the comma are always ready.
  std::vector<std::string> calloutWords;

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
  bool freePlay = false;  // no credits needed: "Credits 0" would mislead
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

  // Machine thread (the rules). Queued; the drawing thread takes them.
  void PushCallout(const Callout& callout);
  // Drawing thread: every callout pushed since the last call, oldest first.
  void TakeCallouts(std::vector<Callout>* out);
  bool HasCallouts() const;

  // Drawing thread: true, and a copy, when something changed since `version`.
  bool ReadIfNewer(uint64_t version, Snapshot* out) const;
  uint64_t Version() const;

  // Told about every value the rules change and every callout, as timeline
  // changes ("value.mode", "Car Chase"; "callout", "text=..."), on the thread
  // that made them. Set before the rules run, for --scoreboard-record.
  void SetObserver(std::function<void(const std::string& what, const std::string& value)> observer)
  {
    m_observer = std::move(observer);
  }

 private:
  std::function<void(const std::string&, const std::string&)> m_observer;
  std::vector<std::string> m_playerVarNames;
  std::vector<std::string> m_ballVarNames;
  mutable std::mutex m_mutex;
  Snapshot m_snapshot;
  // Bounded: a rule calling out every switch of a frantic multiball must
  // not grow a queue nobody shows.
  std::vector<Callout> m_callouts;
};

// A recorded game, as ppuc-pinmame --scoreboard-record writes it and
// ppuc-scoreboard-preview --replay draws it: one change per line,
// "<ms> <what> <value>". A callout's value is its fields:
// "text=SILVER BULLET|effect=slide|spot=low|priority=2|ms=1500".
std::string FormatTimelineCallout(const Callout& callout);
bool ParseTimelineCallout(const std::string& spec, Callout* out);
// What changed from `before` to `after`, as (what, value) pairs: players,
// current, ball, credits, ingame, score1..4, var.<name>, ballvar.<name>.
std::vector<std::pair<std::string, std::string>> DiffFacts(const Facts& before, const Facts& after,
                                                           const Layout& layout);

// What a Text field says now, before its {placeholders} are filled in.
const std::string& FieldText(const Field& field, const Snapshot& snapshot);

// "12450000" -> "12,450,000"; with "." as the separator "12.450.000".
std::string FormatScore(uint64_t score, const std::string& separator = ",");

// The translation the snapshot's language picks, or none.
const Layout::Translation* TranslationFor(const Layout& layout, const Snapshot& snapshot);
// A layout text in the snapshot's language: its translation, or itself.
const std::string& Translate(const Layout& layout, const Snapshot& snapshot, const std::string& text);
// The thousands separator of the snapshot's language.
std::string ThousandsSeparator(const Layout& layout, const Snapshot& snapshot);
// A callout the rules sent as a number ("10,000,000") with the language's
// separator; any other text as it is. The rules translate their own words.
std::string LocalizeNumber(const Layout& layout, const Snapshot& snapshot, const std::string& text);
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
// window, as PUP's CustomPos wants it ("screen,x%,y%,w%,h%"). PUP counts in
// parts of the whole screen, so on a screen of another shape than the board
// (screenW x screenH, a 21:9 monitor) the window sits elsewhere: the board is
// drawn as large as fits, in the middle. 0 x 0: a screen of the board's shape.
std::string PupCustomPos(const Layout& layout, int pupScreen, int screenW = 0, int screenH = 0);

}  // namespace scoreboard
