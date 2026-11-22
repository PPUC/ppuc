#include "RulesFixture.h"
#include "doctest.h"
#include "scoreboard/BurnEffect.h"
#include "scoreboard/ScoreboardModel.h"

using namespace scoreboard;

namespace
{

Layout MakeLayout()
{
  Layout layout;
  layout.font = "font.ttf";
  layout.lists["ranks"] = {"Rookie", "Officer", "Sergeant"};
  Field rank;
  rank.text = "{player.rank:ranks}";
  layout.fields.push_back(rank);
  Field bullets;
  bullets.type = Field::Type::Icons;
  Source::Parse("player.bullets", &bullets.source);
  bullets.mask = true;
  bullets.count = 8;
  bullets.on = "on.png";
  layout.fields.push_back(bullets);
  Field stars;
  stars.type = Field::Type::Icons;
  Source::Parse("player.badges", &stars.source);
  stars.count = 5;
  stars.on = "star.png";
  layout.fields.push_back(stars);
  Field hits;
  hits.text = "{ball.bullets_hit} hits";
  layout.fields.push_back(hits);
  layout.Finish();
  return layout;
}

Snapshot MakeSnapshot(const Layout& layout)
{
  Snapshot snapshot;
  snapshot.facts.hasGame = true;
  snapshot.facts.inGame = true;
  snapshot.facts.players = 3;
  snapshot.facts.current = 2;
  snapshot.facts.ball = 2;
  snapshot.facts.credits = 4;
  snapshot.facts.scores = {1000, 38720500, 500, 0};
  snapshot.facts.playerVars.assign(layout.playerVarNames.size(), 0);
  snapshot.facts.ballVars.assign(layout.ballVarNames.size(), 0);
  return snapshot;
}

void SetVar(const Layout& layout, Snapshot* snapshot, const std::string& name, int64_t value)
{
  for (size_t i = 0; i < layout.playerVarNames.size(); ++i)
    if (layout.playerVarNames[i] == name) snapshot->facts.playerVars[i] = value;
  for (size_t i = 0; i < layout.ballVarNames.size(); ++i)
    if (layout.ballVarNames[i] == name) snapshot->facts.ballVars[i] = value;
}

}  // namespace

TEST_CASE("scoreboard: sources parse")
{
  Source source;
  CHECK(Source::Parse("ball", &source));
  CHECK(source.kind == Source::Kind::Ball);
  CHECK(Source::Parse("score3", &source));
  CHECK(source.kind == Source::Kind::PlayerScore);
  CHECK(source.index == 3);
  CHECK(Source::Parse("player.rank", &source));
  CHECK(source.kind == Source::Kind::PlayerVar);
  CHECK(source.name == "rank");
  CHECK(Source::Parse("value.mode", &source));
  CHECK(source.kind == Source::Kind::Value);
  CHECK_FALSE(Source::Parse("score5", &source));
  CHECK_FALSE(Source::Parse("player.", &source));
  CHECK_FALSE(Source::Parse("bogus", &source));
}

TEST_CASE("scoreboard: the layout collects the variables its fields read")
{
  const Layout layout = MakeLayout();
  CHECK(layout.playerVarNames == std::vector<std::string>{"rank", "bullets", "badges"});
  CHECK(layout.ballVarNames == std::vector<std::string>{"bullets_hit"});
  CHECK(layout.styles.count("burnt") == 1);
  CHECK(layout.styles.count("ember") == 1);
  CHECK(layout.Validate().empty());
}

TEST_CASE("scoreboard: validation names what is wrong")
{
  Layout layout = MakeLayout();
  Field bad;
  bad.text = "{player.rank:titles} {nonsense}";
  bad.style = "chalk";
  layout.fields.push_back(bad);
  layout.logoSize = 20;
  layout.windowW = 5000;
  layout.Finish();
  const std::vector<std::string> problems = layout.Validate();
  REQUIRE(problems.size() == 5);
  CHECK(problems[0] == "the window does not fit the size");
  CHECK(problems[1].find("logo size") != std::string::npos);
  CHECK(problems[2] == "field 5: style 'chalk' is not defined");
  CHECK(problems[3] == "field 5: list 'titles' is not defined");
  CHECK(problems[4] == "field 5: '{nonsense}' is not a value");
}

TEST_CASE("scoreboard: scores get thousands separators")
{
  CHECK(FormatScore(0) == "0");
  CHECK(FormatScore(999) == "999");
  CHECK(FormatScore(1000) == "1,000");
  CHECK(FormatScore(38720500) == "38,720,500");
}

TEST_CASE("scoreboard: text fields")
{
  const Layout layout = MakeLayout();
  Snapshot snapshot = MakeSnapshot(layout);
  SetVar(layout, &snapshot, "rank", 2);
  SetVar(layout, &snapshot, "bullets_hit", 7);
  snapshot.values["mode"] = "Barroom Brawl";

  CHECK(ExpandText("Ball {ball}", layout, snapshot) == "Ball 2");
  CHECK(ExpandText("Player {player} of {players}", layout, snapshot) == "Player 2 of 3");
  CHECK(ExpandText("{score}", layout, snapshot) == "38,720,500");
  CHECK(ExpandText("{score1}", layout, snapshot) == "1,000");
  CHECK(ExpandText("{player.rank:ranks}", layout, snapshot) == "Sergeant");
  CHECK(ExpandText("{player.rank}", layout, snapshot) == "2");
  CHECK(ExpandText("{ball.bullets_hit} hits", layout, snapshot) == "7 hits");
  CHECK(ExpandText("{value.mode}", layout, snapshot) == "Barroom Brawl");
  CHECK(ExpandText("Credits {credits}", layout, snapshot) == "Credits 4");
  // A typo is a gap, not braces on a machine's screen.
  CHECK(ExpandText("a{typo}b", layout, snapshot) == "ab");
  CHECK(ExpandText("{value.unset}", layout, snapshot) == "");
  // Past the end of a list: nothing rather than a crash.
  SetVar(layout, &snapshot, "rank", 9);
  CHECK(ExpandText("{player.rank:ranks}", layout, snapshot) == "");
  CHECK(ExpandText("open {brace", layout, snapshot) == "open {brace");
}

TEST_CASE("scoreboard: icons lit by count or by bit")
{
  const Layout layout = MakeLayout();
  Snapshot snapshot = MakeSnapshot(layout);
  SetVar(layout, &snapshot, "bullets", 0b10100101);
  SetVar(layout, &snapshot, "badges", 3);

  CHECK(IconsLit(layout.fields[1], layout, snapshot) ==
        std::vector<bool>{true, false, true, false, false, true, false, true});
  CHECK(IconsLit(layout.fields[2], layout, snapshot) == std::vector<bool>{true, true, true, false, false});
  SetVar(layout, &snapshot, "badges", 99);
  CHECK(IconsLit(layout.fields[2], layout, snapshot) == std::vector<bool>{true, true, true, true, true});
}

TEST_CASE("scoreboard: the featured player and the others")
{
  const Layout layout = MakeLayout();
  Snapshot snapshot = MakeSnapshot(layout);
  CHECK(FeaturedPlayer(snapshot) == 2);
  CHECK(OtherPlayers(snapshot) == std::vector<int>{1, 3});

  // After the game the best score is the big one.
  snapshot.facts.inGame = false;
  snapshot.facts.scores = {5000, 2000, 9000, 0};
  CHECK(FeaturedPlayer(snapshot) == 3);
  CHECK(OtherPlayers(snapshot) == std::vector<int>{1, 2});

  snapshot.facts.players = 0;
  CHECK(FeaturedPlayer(snapshot) == 0);
  CHECK(OtherPlayers(snapshot).empty());
}

TEST_CASE("scoreboard: fields show in game, in attract, or always")
{
  const Layout layout = MakeLayout();
  Snapshot snapshot = MakeSnapshot(layout);
  Field field;
  field.when = When::Game;
  CHECK(FieldVisible(field, snapshot));
  field.when = When::Attract;
  CHECK_FALSE(FieldVisible(field, snapshot));
  snapshot.facts.inGame = false;
  CHECK(FieldVisible(field, snapshot));
}

TEST_CASE("scoreboard: the feed moves on only when something changed")
{
  Feed feed;
  Facts facts;
  facts.players = 1;
  feed.Publish(facts);
  const uint64_t v1 = feed.Version();
  feed.Publish(facts);
  CHECK(feed.Version() == v1);
  facts.scores[0] = 10;
  feed.Publish(facts);
  CHECK(feed.Version() == v1 + 1);

  feed.SetValue("mode", "Stakeout");
  CHECK(feed.Version() == v1 + 2);
  feed.SetValue("mode", "Stakeout");
  CHECK(feed.Version() == v1 + 2);

  Snapshot snapshot;
  CHECK(feed.ReadIfNewer(0, &snapshot));
  CHECK(snapshot.values.at("mode") == "Stakeout");
  CHECK(snapshot.facts.scores[0] == 10);
  CHECK_FALSE(feed.ReadIfNewer(snapshot.version, &snapshot));

  feed.SetValue("mode", "");
  CHECK(feed.ReadIfNewer(snapshot.version, &snapshot));
  CHECK(snapshot.values.count("mode") == 0);
  feed.SetValue("mode", "");
  CHECK_FALSE(feed.ReadIfNewer(snapshot.version, &snapshot));
}

TEST_CASE("scoreboard: PUP CustomPos for the window")
{
  Layout layout;
  layout.windowX = 320;
  layout.windowY = 54;
  layout.windowW = 1280;
  layout.windowH = 720;
  CHECK(PupCustomPos(layout, 2) == "2,16.667,5.000,66.667,66.667");
  CHECK(PupCustomPos(layout, 2, 1920, 1080) == "2,16.667,5.000,66.667,66.667");
  // A 21:9 screen: the board is 1920 wide in the middle of 2560, so the
  // window moves right by 320 and is narrower in parts of the screen.
  CHECK(PupCustomPos(layout, 2, 2560, 1080) == "2,25.000,5.000,50.000,66.667");
}

TEST_CASE("scoreboard: a field can say Free Play instead of a credit count")
{
  Field credits;
  credits.type = Field::Type::Text;
  credits.text = "Credits {credits}";
  credits.freePlayText = "Free Play";
  Layout layout;
  Snapshot snapshot;
  snapshot.facts.credits = 0;
  CHECK(ExpandText(FieldText(credits, snapshot), layout, snapshot) == "Credits 0");
  snapshot.facts.freePlay = true;
  CHECK(ExpandText(FieldText(credits, snapshot), layout, snapshot) == "Free Play");
  // A field without a free play text says what it always says.
  Field ball;
  ball.text = "Ball {ball}";
  snapshot.facts.ball = 2;
  CHECK(ExpandText(FieldText(ball, snapshot), layout, snapshot) == "Ball 2");

  // It is a fact like any other: recorded, and a change is a change.
  Facts before, after;
  after.freePlay = true;
  CHECK(before != after);
  const auto changes = DiffFacts(before, after, layout);
  REQUIRE(changes.size() == 1);
  CHECK(changes[0] == std::make_pair(std::string("freeplay"), std::string("1")));
}

TEST_CASE("scoreboard: the layout's words in the rules' language")
{
  Layout layout;
  layout.lists["ranks"] = {"Rookie", "Officer"};
  Layout::Translation de;
  de.thousands = ".";
  de.texts = {{"Player {player}", "Spieler {player}"}, {"Officer", "Polizist"}, {"P{n}  {score}", "S{n}  {score}"}};
  layout.translations["de"] = de;
  layout.playerVarNames = {"rank"};

  Snapshot snapshot;
  snapshot.facts.players = 2;
  snapshot.facts.current = 2;
  snapshot.facts.scores = {1234567, 0, 0, 0};
  snapshot.facts.playerVars = {1};

  // No language set: the layout's own words, commas.
  CHECK(ExpandText(Translate(layout, snapshot, "Player {player}"), layout, snapshot) == "Player 2");
  CHECK(ExpandText("{player.rank:ranks}", layout, snapshot) == "Officer");
  CHECK(ExpandText("{score1}", layout, snapshot) == "1,234,567");
  CHECK(LocalizeNumber(layout, snapshot, "10,000,000") == "10,000,000");

  snapshot.values["language"] = "de";
  CHECK(ExpandText(Translate(layout, snapshot, "Player {player}"), layout, snapshot) == "Spieler 2");
  CHECK(ExpandText("{player.rank:ranks}", layout, snapshot) == "Polizist");
  CHECK(ExpandText("{score1}", layout, snapshot) == "1.234.567");
  CHECK(FormatScore(1234567, ThousandsSeparator(layout, snapshot)) == "1.234.567");
  CHECK(Translate(layout, snapshot, "P{n}  {score}") == "S{n}  {score}");
  // A word without a translation stays; a callout number gets dots, words not.
  CHECK(Translate(layout, snapshot, "Bonus") == "Bonus");
  CHECK(LocalizeNumber(layout, snapshot, "10,000,000") == "10.000.000");
  CHECK(LocalizeNumber(layout, snapshot, "SHERIFFSTERN") == "SHERIFFSTERN");

  // A language the layout does not know: its own words.
  snapshot.values["language"] = "fr";
  CHECK(Translate(layout, snapshot, "Player {player}") == "Player {player}");
  CHECK(ExpandText("{score1}", layout, snapshot) == "1,234,567");
}

TEST_CASE("scoreboard: a recorded game reads back")
{
  Callout callout;
  callout.text = "SILVER | BULLET";
  callout.effect = Effect::Slide;
  callout.spot = "low";
  callout.priority = 2;
  callout.durationMs = 2000;
  const std::string line = FormatTimelineCallout(callout);
  CHECK(line == "text=SILVER   BULLET|effect=slide|spot=low|priority=2|ms=2000");
  Callout back;
  REQUIRE(ParseTimelineCallout(line, &back));
  CHECK(back.text == "SILVER   BULLET");
  CHECK(back.effect == Effect::Slide);
  CHECK(back.spot == "low");
  CHECK(back.priority == 2);
  CHECK(back.durationMs == 2000);
  CHECK_FALSE(ParseTimelineCallout("text=X|colour=red", &back));

  Layout layout;
  layout.playerVarNames = {"rank"};
  layout.ballVarNames = {"bonus_x"};
  Facts before;
  Facts after;
  after.inGame = true;
  after.players = 3;
  after.current = 2;
  after.ball = 1;
  after.scores = {100, 2500, 0, 0};
  after.playerVars = {4};
  after.ballVars = {2};
  const auto changes = DiffFacts(before, after, layout);
  std::map<std::string, std::string> got(changes.begin(), changes.end());
  CHECK(got.size() == 8);
  CHECK(got["ingame"] == "1");
  CHECK(got["players"] == "3");
  CHECK(got["current"] == "2");
  CHECK(got["score2"] == "2500");
  CHECK(got["var.rank"] == "4");
  CHECK(got["ballvar.bonus_x"] == "2");
  CHECK(DiffFacts(after, after, layout).empty());

  // The feed tells an observer what the rules set and call out.
  Feed feed;
  std::vector<std::string> seen;
  feed.SetObserver([&seen](const std::string& what, const std::string& value) { seen.push_back(what + " " + value); });
  feed.SetValue("mode", "Car Chase");
  feed.SetValue("mode", "Car Chase");  // no change, nothing to record
  feed.SetValue("mode", "");
  feed.PushCallout(callout);
  REQUIRE(seen.size() == 3);
  CHECK(seen[0] == "value.mode Car Chase");
  CHECK(seen[1] == "value.mode ");
  CHECK(seen[2].rfind("callout text=SILVER", 0) == 0);
}

TEST_CASE("scoreboard: UTF-8")
{
  CHECK(DecodeUtf8("A1") == std::vector<uint32_t>{'A', '1'});
  CHECK(DecodeUtf8("\xc3\xa9") == std::vector<uint32_t>{0xe9});
  CHECK(DecodeUtf8("\xe2\x98\x85") == std::vector<uint32_t>{0x2605});
  CHECK(DecodeUtf8("\xe2\x98") == std::vector<uint32_t>{'?', '?'});
  CHECK(DecodeUtf8("\xff") == std::vector<uint32_t>{'?'});
}

TEST_CASE("scoreboard: a burnt letter")
{
  // A 10x10 square of full coverage in a 20x20 box.
  const int n = 20;
  std::vector<uint8_t> coverage(n * n, 0);
  for (int y = 5; y < 15; ++y)
    for (int x = 5; x < 15; ++x) coverage[y * n + x] = 255;

  Style burnt;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  Burn(coverage.data(), n, n, burnt, 40, &rgba, &w, &h);
  const int pad = BurnPad(burnt, 40);
  CHECK(w == n + 2 * pad);
  CHECK(h == n + 2 * pad);
  auto px = [&](int x, int y) { return &rgba[(static_cast<size_t>(y + pad) * w + (x + pad)) * 4]; };

  // The middle of the letter: nearly opaque and dark.
  CHECK(px(10, 10)[3] > 200);
  CHECK(px(10, 10)[0] < 80);
  // Just outside: scorched, partly see-through.
  CHECK(px(16, 10)[3] > 0);
  CHECK(px(16, 10)[3] < px(10, 10)[3]);
  // The far corner of the padding: untouched wood.
  CHECK(rgba[3] == 0);

  // The same glyph burns the same way twice.
  std::vector<uint8_t> again;
  Burn(coverage.data(), n, n, burnt, 40, &again, &w, &h);
  CHECK(again == rgba);

  // A glowing brand is bright in the middle.
  Layout layout;
  layout.Finish();
  std::vector<uint8_t> hot;
  Burn(coverage.data(), n, n, layout.styles.at("ember"), 40, &hot, &w, &h);
  const int hotPad = BurnPad(layout.styles.at("ember"), 40);
  const uint8_t* middle = &hot[(static_cast<size_t>(10 + hotPad) * w + (10 + hotPad)) * 4];
  CHECK(middle[0] > 200);
  CHECK(middle[1] > 150);
}

TEST_CASE("scoreboard: a shadow under the letters")
{
  const int n = 20;
  std::vector<uint8_t> coverage(n * n, 0);
  for (int y = 4; y < 16; ++y)
    for (int x = 4; x < 16; ++x) coverage[y * n + x] = 255;

  Style plain;
  CHECK_FALSE(HasShadow(plain));
  CHECK(ShadowOffset(plain, 100) == 0);

  Style shaded;
  shaded.shadow = 0.5f;
  shaded.shadowOffset = 0.05f;
  shaded.shadowSoftness = 0.03f;
  shaded.shadowColor = 0x102030;
  REQUIRE(HasShadow(shaded));
  CHECK(ShadowOffset(shaded, 100) == 5);
  // A blur wider than the scorch grows the pad, so letter and shadow still
  // line up.
  Style soft = shaded;
  soft.shadowSoftness = 0.2f;
  CHECK(BurnPad(soft, 100) > BurnPad(plain, 100));
  CHECK(BurnPad(shaded, 100) == BurnPad(plain, 100));

  std::vector<uint8_t> letter, shadow;
  int lw = 0, lh = 0, sw = 0, sh = 0;
  Burn(coverage.data(), n, n, shaded, 100, &letter, &lw, &lh);
  BurnShadow(coverage.data(), n, n, shaded, 100, &shadow, &sw, &sh);
  CHECK(sw == lw);
  CHECK(sh == lh);
  const int pad = BurnPad(shaded, 100);
  auto alpha = [&](int x, int y) { return shadow[(static_cast<size_t>(y + pad) * sw + (x + pad)) * 4 + 3]; };
  // Under the letter: as dark as asked, never darker.
  CHECK(alpha(10, 10) > 100);
  CHECK(alpha(10, 10) <= 128);
  // Soft at the edge, gone far away.
  CHECK(alpha(10, 10 + 6 + pad / 2) < alpha(10, 10));
  CHECK(shadow[3] == 0);
  CHECK(shadow[(static_cast<size_t>(pad + 10) * sw + pad + 10) * 4 + 0] == 0x10);
}

TEST_CASE("scoreboard: blur keeps the total")
{
  std::vector<float> values(9 * 9, 0.0f);
  values[4 * 9 + 4] = 81.0f;
  BlurCoverage(&values, 9, 9, 1);
  float sum = 0.0f;
  for (const float v : values) sum += v;
  CHECK(sum == doctest::Approx(81.0f).epsilon(0.05));
  CHECK(values[4 * 9 + 4] < 81.0f);
  CHECK(values[4 * 9 + 5] > 0.0f);
}

TEST_CASE("scoreboard: the logo is scaled by area")
{
  // A one-pixel line in an 8x8 picture must survive halving: a plain resample
  // drops every other row and with it the line.
  std::vector<uint8_t> src(64, 0);
  for (int x = 0; x < 8; ++x) src[3 * 8 + x] = 255;
  const std::vector<uint8_t> out = ScaleCoverage(src.data(), 8, 8, 4, 4);
  REQUIRE(out.size() == 16);
  CHECK(out[1 * 4 + 0] == 127);
  CHECK(out[0] == 0);
  CHECK(ScaleCoverage(src.data(), 8, 8, 0, 4).empty());
}

TEST_CASE("scoreboard: rules set values with ppuc.scoreboardSet")
{
  Feed feed;
  ppuc_test::RulesHarness rules(R"(
    function ppuc.onSwitchChanged(number, state)
      if state == 0 then return end
      if number == 1 then ppuc.scoreboardSet("mode", "Stakeout") end
      if number == 2 then ppuc.scoreboardSet("jackpot", 25000000) end
      if number == 3 then ppuc.scoreboardSet("mode", nil) end
      if number == 4 then ppuc.scoreboardSet("lit", true) end
    end
  )");
  REQUIRE(rules.Load() == "");
  // No scoreboard: the call works and goes nowhere.
  rules.engine().ProcessSwitchState(1, 1);
  rules.engine().ProcessSwitchState(1, 0);
  CHECK(feed.Version() == 0);

  rules.engine().SetScoreboardFeed(&feed);
  rules.engine().ProcessSwitchState(1, 1);
  rules.engine().ProcessSwitchState(2, 1);
  rules.engine().ProcessSwitchState(4, 1);
  Snapshot snapshot;
  REQUIRE(feed.ReadIfNewer(0, &snapshot));
  CHECK(snapshot.values.at("mode") == "Stakeout");
  CHECK(snapshot.values.at("jackpot") == "25000000");
  CHECK(snapshot.values.at("lit") == "1");

  rules.engine().ProcessSwitchState(3, 1);
  REQUIRE(feed.ReadIfNewer(snapshot.version, &snapshot));
  CHECK(snapshot.values.count("mode") == 0);
}

TEST_CASE("scoreboard: a callout pops in, settles, fades out")
{
  Callout pop;
  pop.durationMs = 1500;
  CHECK(PoseAt(pop, 0).visible);
  CHECK(PoseAt(pop, 0).scale == doctest::Approx(1.6f));
  CHECK(PoseAt(pop, 0).alpha == doctest::Approx(0.0f));
  // Landed slightly small, then settled at its size.
  CHECK(PoseAt(pop, 180).scale == doctest::Approx(0.9f));
  CHECK(PoseAt(pop, 400).scale == doctest::Approx(1.0f));
  CHECK(PoseAt(pop, 400).alpha == doctest::Approx(1.0f));
  // Fading over the last 300 ms, gone at the end.
  CHECK(PoseAt(pop, 1350).alpha == doctest::Approx(0.5f));
  CHECK_FALSE(PoseAt(pop, 1500).visible);

  Callout slide = pop;
  slide.effect = Effect::Slide;
  CHECK(PoseAt(slide, 0).dy > 0.5f);
  CHECK(PoseAt(slide, 400).dy == doctest::Approx(0.0f));

  Callout shake = pop;
  shake.effect = Effect::Shake;
  bool moved = false;
  for (uint64_t t = 190; t < 600; t += 10) moved = moved || PoseAt(shake, t).dx != 0.0f;
  CHECK(moved);
  CHECK(PoseAt(shake, 900).dx == doctest::Approx(0.0f));

  // A very short one still comes in and goes out.
  Callout blink;
  blink.durationMs = 200;
  CHECK(PoseAt(blink, 100).visible);
  CHECK(PoseAt(blink, 100).alpha > 0.0f);
}

TEST_CASE("scoreboard: a brand burns in letter by letter")
{
  Callout callout;
  callout.text = "Player 1";
  callout.effect = Effect::Brand;
  callout.durationMs = 2200;
  Effect parsed = Effect::Pop;
  REQUIRE(Callout::ParseEffect("brand", &parsed));
  CHECK(parsed == Effect::Brand);
  CHECK(FormatTimelineCallout(callout).find("effect=brand") != std::string::npos);

  const int count = 7;  // P l a y e r 1: the space has no letter
  // At the start only the first letter is coming, large and hot.
  LetterPose first = LetterAt(callout, 10, 0, count);
  CHECK(first.alpha > 0.0f);
  CHECK(first.scale > 1.2f);
  CHECK(first.flare > 0.9f);
  CHECK(LetterAt(callout, 10, 1, count).alpha == 0.0f);
  // Letter by letter, left to right, at most 110 ms apart.
  CHECK(LetterAt(callout, 115, 1, count).alpha > 0.0f);
  CHECK(LetterAt(callout, 115, 3, count).alpha == 0.0f);
  // Long after: all landed, cooled, at their size.
  const LetterPose last = LetterAt(callout, 1500, count - 1, count);
  CHECK(last.alpha == 1.0f);
  CHECK(last.scale == doctest::Approx(1.0f));
  CHECK(last.flare == 0.0f);
  // The whole fades out at the end, and is gone after.
  CHECK(PoseAt(callout, 1000).alpha == doctest::Approx(1.0f));
  CHECK(PoseAt(callout, 2100).alpha < 0.5f);
  CHECK_FALSE(PoseAt(callout, 2200).visible);
  // Other effects leave letters alone.
  callout.effect = Effect::Pop;
  const LetterPose plain = LetterAt(callout, 10, 5, count);
  CHECK(plain.alpha == 1.0f);
  CHECK(plain.scale == 1.0f);
  CHECK(plain.flare == 0.0f);
}

TEST_CASE("scoreboard: a jackpot is not cut short by the next switch")
{
  Callout jackpot;
  jackpot.priority = 10;
  Callout points;
  CHECK_FALSE(CalloutReplaces(jackpot, 500, points));
  CHECK(CalloutReplaces(jackpot, 5000, points));  // it is over
  CHECK(CalloutReplaces(points, 500, jackpot));
  CHECK(CalloutReplaces(points, 500, points));  // equal: the newest wins
}

TEST_CASE("scoreboard: callouts queue for the drawing thread, a few at most")
{
  Feed feed;
  for (int i = 0; i < 20; ++i)
  {
    Callout c;
    c.text = std::to_string(i);
    feed.PushCallout(c);
  }
  CHECK(feed.HasCallouts());
  std::vector<Callout> taken;
  feed.TakeCallouts(&taken);
  REQUIRE(taken.size() == 8);
  CHECK(taken.front().text == "12");
  CHECK(taken.back().text == "19");
  CHECK_FALSE(feed.HasCallouts());
}

TEST_CASE("scoreboard: the window's middle is a callout spot")
{
  Layout layout;
  layout.font = "f.ttf";
  layout.windowX = 320;
  layout.windowY = 120;
  layout.windowW = 1280;
  layout.windowH = 720;
  layout.Finish();
  REQUIRE(layout.spots.count("center") == 1);
  CHECK(layout.spots.at("center").x == 960);
  CHECK(layout.spots.at("center").y == 480);
  layout.spots["low"].style = "chalk";
  CHECK(layout.Validate() == std::vector<std::string>{"callout spot 'low': style 'chalk' is not defined"});
}

TEST_CASE("scoreboard: rules call out with ppuc.scoreboardCallout")
{
  Feed feed;
  ppuc_test::RulesHarness rules(R"(
    function ppuc.onSwitchChanged(number, state)
      if state == 0 then return end
      if number == 1 then ppuc.scoreboardCallout(10000000) end
      if number == 2 then
        ppuc.scoreboardCallout{text = "SILVER BULLET", spot = "low", effect = "shake", ms = 2000, priority = 5}
      end
      if number == 3 then ppuc.scoreboardCallout{text = "x", effect = "wobble"} end
    end
  )");
  rules.engine().SetScoreboardFeed(&feed);
  REQUIRE(rules.Load() == "");
  rules.engine().ProcessSwitchState(1, 1);
  rules.engine().ProcessSwitchState(2, 1);
  std::vector<Callout> taken;
  feed.TakeCallouts(&taken);
  REQUIRE(taken.size() == 2);
  CHECK(taken[0].text == "10,000,000");
  CHECK(taken[0].effect == Effect::Pop);
  CHECK(taken[1].text == "SILVER BULLET");
  CHECK(taken[1].spot == "low");
  CHECK(taken[1].effect == Effect::Shake);
  CHECK(taken[1].durationMs == 2000);
  CHECK(taken[1].priority == 5);
  // A wrong effect is a rules error like any other, named, and shows the
  // first time it fires rather than as a silent pop.
  rules.engine().ProcessSwitchState(3, 1);
  feed.TakeCallouts(&taken);
  CHECK(taken.empty());
  CHECK(rules.engine().HasFatalError());
  CHECK(rules.engine().GetFatalError().find("pop, fade, slide, shake or brand") != std::string::npos);
}
