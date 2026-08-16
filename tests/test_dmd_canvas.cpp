// Tests for the DMD framebuffer, text layout and the built-in score screen.
//
// Assertions on rendered output compare an ASCII-art dump against a raw string
// literal, so a failure prints a picture of what went wrong instead of a byte
// offset. That is the difference between a test someone fixes and a test
// someone deletes.

#include <string>

#include "GameFixture.h"
#include "dmd/DmdCanvas.h"
#include "dmd/DmdDefaultScreen.h"
#include "doctest.h"

namespace
{
using Align = DmdTextOptions::Align;

// Extracts a rectangle of the canvas as ASCII art, so a test can assert on one
// glyph without pinning the whole 128x32 frame.
std::string Crop(const DmdCanvas& canvas, int x, int y, int w, int h)
{
  std::string out;
  for (int row = y; row < y + h; ++row)
  {
    for (int column = x; column < x + w; ++column)
    {
      out.push_back(canvas.GetPixel(column, row) == 0 ? '.' : '#');
    }
    out.push_back('\n');
  }
  return out;
}

int LitPixels(const DmdCanvas& canvas)
{
  int count = 0;
  for (int y = 0; y < canvas.Height(); ++y)
  {
    for (int x = 0; x < canvas.Width(); ++x)
    {
      if (canvas.GetPixel(x, y) != 0) ++count;
    }
  }
  return count;
}
}  // namespace

TEST_CASE("a new canvas is blank and sized as asked")
{
  DmdCanvas canvas(128, 32);
  CHECK(canvas.Width() == 128);
  CHECK(canvas.Height() == 32);
  CHECK(canvas.Size() == 128u * 32u);
  CHECK(LitPixels(canvas) == 0);
}

TEST_CASE("pixels clip silently at every edge")
{
  DmdCanvas canvas(16, 8);
  canvas.Pixel(-1, 4, 15);
  canvas.Pixel(16, 4, 15);
  canvas.Pixel(8, -1, 15);
  canvas.Pixel(8, 8, 15);
  canvas.Pixel(-100, -100, 15);
  CHECK(LitPixels(canvas) == 0);

  canvas.Pixel(0, 0, 15);
  canvas.Pixel(15, 7, 15);
  CHECK(LitPixels(canvas) == 2);
}

TEST_CASE("a rect that starts off-canvas still draws the part that fits")
{
  DmdCanvas canvas(8, 4);
  canvas.Fill(-2, -1, 4, 3, 15);
  CHECK(Crop(canvas, 0, 0, 8, 4) ==
        "##......\n"
        "##......\n"
        "........\n"
        "........\n");
}

TEST_CASE("an outlined rect draws only its border")
{
  DmdCanvas canvas(6, 5);
  canvas.Rect(0, 0, 6, 5, 15, false);
  CHECK(canvas.Dump() ==
        "######\n"
        "#....#\n"
        "#....#\n"
        "#....#\n"
        "######\n");
}

TEST_CASE("a line is drawn between its endpoints inclusive")
{
  DmdCanvas canvas(5, 5);
  canvas.Line(0, 0, 4, 4, 15);
  CHECK(canvas.Dump() ==
        "#....\n"
        ".#...\n"
        "..#..\n"
        "...#.\n"
        "....#\n");
}

TEST_CASE("a glyph renders as the art in mkfont.py describes it")
{
  DmdCanvas canvas(5, 7);
  canvas.Text(0, 0, "1");
  CHECK(canvas.Dump() ==
        "..#..\n"
        ".##..\n"
        "..#..\n"
        "..#..\n"
        "..#..\n"
        "..#..\n"
        ".###.\n");
}

TEST_CASE("scale multiplies every pixel of a glyph")
{
  DmdCanvas canvas(10, 14);
  DmdTextOptions options;
  options.scale = 2;
  canvas.Text(0, 0, "1", options);
  CHECK(canvas.Dump() ==
        "....##....\n"
        "....##....\n"
        "..####....\n"
        "..####....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "....##....\n"
        "..######..\n"
        "..######..\n");
}

TEST_CASE("lower case folds to upper case rather than rendering blanks")
{
  DmdCanvas upper(5, 7);
  DmdCanvas lower(5, 7);
  upper.Text(0, 0, "A");
  lower.Text(0, 0, "a");
  CHECK(upper.Dump() == lower.Dump());
}

TEST_CASE("an unmapped character renders as a space, not as garbage")
{
  DmdCanvas canvas(5, 7);
  canvas.Text(0, 0, "\x01");
  CHECK(LitPixels(canvas) == 0);
}

TEST_CASE("TextWidth matches the pixels actually drawn")
{
  for (const std::string& text : {std::string("0"), std::string("WWW"), std::string(" "), std::string("12345")})
  {
    for (uint8_t scale : {uint8_t{1}, uint8_t{2}, uint8_t{3}})
    {
      DmdTextOptions options;
      options.scale = scale;
      DmdCanvas canvas(256, 64);
      const int drawn = canvas.Text(0, 0, text, options);
      CHECK(drawn == DmdCanvas::TextWidth(text, options));
    }
  }
}

TEST_CASE("empty text draws nothing and measures zero")
{
  DmdCanvas canvas(16, 8);
  CHECK(canvas.Text(0, 0, "") == 0);
  CHECK(DmdCanvas::TextWidth("") == 0);
  CHECK(LitPixels(canvas) == 0);
}

TEST_CASE("right alignment puts the last pixel column at x")
{
  DmdCanvas canvas(32, 7);
  DmdTextOptions options;
  options.align = Align::Right;
  canvas.Text(20, 0, "12", options);

  int rightmost = -1;
  for (int x = 0; x < canvas.Width(); ++x)
  {
    for (int y = 0; y < canvas.Height(); ++y)
    {
      if (canvas.GetPixel(x, y) != 0) rightmost = x;
    }
  }
  CHECK(rightmost == 20);
}

TEST_CASE("centre alignment is symmetric about x")
{
  DmdCanvas canvas(41, 7);
  DmdTextOptions options;
  options.align = Align::Center;
  canvas.Text(20, 0, "111", options);

  int leftmost = canvas.Width();
  int rightmost = -1;
  for (int x = 0; x < canvas.Width(); ++x)
  {
    for (int y = 0; y < canvas.Height(); ++y)
    {
      if (canvas.GetPixel(x, y) != 0)
      {
        leftmost = std::min(leftmost, x);
        rightmost = std::max(rightmost, x);
      }
    }
  }
  CHECK((20 - leftmost) - (rightmost - 20) <= 1);
}

TEST_CASE("number formatting handles commas, padding and sign")
{
  DmdTextOptions plain;
  CHECK(DmdCanvas::FormatNumber(0, plain) == "0");
  CHECK(DmdCanvas::FormatNumber(1234567, plain) == "1234567");

  DmdTextOptions commas;
  commas.commas = true;
  CHECK(DmdCanvas::FormatNumber(1234567, commas) == "1,234,567");
  CHECK(DmdCanvas::FormatNumber(100, commas) == "100");
  CHECK(DmdCanvas::FormatNumber(1000, commas) == "1,000");

  DmdTextOptions padded;
  padded.digits = 3;
  padded.pad = '0';
  CHECK(DmdCanvas::FormatNumber(5, padded) == "005");
  padded.pad = ' ';
  CHECK(DmdCanvas::FormatNumber(5, padded) == "  5");

  CHECK(DmdCanvas::FormatNumber(-42, plain) == "-42");
}

TEST_CASE("padding is not mistaken for a digit when grouping")
{
  DmdTextOptions options;
  options.commas = true;
  options.digits = 8;
  options.pad = ' ';
  // The two leading spaces must not attract a comma of their own.
  CHECK(DmdCanvas::FormatNumber(123456, options) == "  123,456");
}

TEST_CASE("digits are fixed width so a rolling score does not jitter")
{
  // Every digit occupies the same cell, so 111 and 888 are the same width. With
  // proportional digits a 1 rolling to a 2 would shift the whole score left.
  CHECK(DmdCanvas::TextWidth("111") == DmdCanvas::TextWidth("888"));
  CHECK(DmdCanvas::TextWidth("000") == DmdCanvas::TextWidth("123"));
}

TEST_CASE("a score far wider than the canvas clips instead of crashing")
{
  DmdCanvas canvas(128, 32);
  DmdTextOptions options;
  options.scale = 4;
  options.commas = true;
  canvas.Number(2, 2, 999999999999LL, options);
  CHECK(LitPixels(canvas) > 0);  // drew something, did not fall over
}

TEST_CASE("the canvas starts dirty and clears on demand")
{
  DmdCanvas canvas(16, 8);
  CHECK(canvas.IsDirty());
  canvas.ClearDirty();
  CHECK_FALSE(canvas.IsDirty());

  canvas.Pixel(1, 1, 15);
  CHECK(canvas.IsDirty());
}

// ---- Built-in screen ----

TEST_CASE("the attract screen shows the title")
{
  GameHarness h;
  h.Start();

  DmdCanvas canvas(128, 32);
  DmdDefaultScreen screen;
  screen.SetTitle("EMDEMO");
  screen.Draw(canvas, h.core(), 0);
  CHECK(LitPixels(canvas) > 0);
}

TEST_CASE("a one-player game shows the score and the status row")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  h.core().AddScore(12340);

  DmdCanvas canvas(128, 32);
  DmdDefaultScreen screen;
  screen.Draw(canvas, h.core(), 0);

  // The status row is the bottom 8 rows; the score sits above it.
  CHECK(LitPixels(canvas) > 0);
  bool statusRowHasPixels = false;
  for (int y = canvas.Height() - 8; y < canvas.Height(); ++y)
  {
    for (int x = 0; x < canvas.Width(); ++x)
    {
      if (canvas.GetPixel(x, y) != 0) statusRowHasPixels = true;
    }
  }
  CHECK(statusRowHasPixels);
}

TEST_CASE("a four-player game marks the player up with an inverted cell")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  h.PressStart();
  h.PressStart();
  h.PressStart();
  REQUIRE(h.core().GetPlayerCount() == 4);

  DmdCanvas canvas(128, 32);
  DmdDefaultScreen screen;
  screen.Draw(canvas, h.core(), 0);

  // Player 1 is up, so their cell is a filled block in the top-left quadrant.
  int filled = 0;
  for (int y = 0; y < 10; ++y)
  {
    for (int x = 0; x < 60; ++x)
    {
      if (canvas.GetPixel(x, y) != 0) ++filled;
    }
  }
  CHECK(filled > 200);
}

TEST_CASE("the tilt screen replaces the scores entirely")
{
  GameHarness h;
  h.config().freePlay = true;
  h.Start();
  h.StartGameAndServe();
  // Warning counting lives in PlayfieldAssist now; by the time GameCore is
  // involved the decision has been made.
  h.core().Tilt();
  h.Update();
  REQUIRE(h.core().IsTilted(0));

  DmdCanvas canvas(128, 32);
  DmdDefaultScreen screen;
  screen.Draw(canvas, h.core(), 0);

  CHECK(LitPixels(canvas) > 0);

  // It blinks between two levels, so an idle-looking machine reads as tilted
  // rather than as broken.
  DmdCanvas dim(128, 32);
  screen.Draw(dim, h.core(), 300);
  CHECK(canvas.Dump() == dim.Dump());  // same shape...
  bool levelsDiffer = false;
  for (int y = 0; y < 32 && !levelsDiffer; ++y)
  {
    for (int x = 0; x < 128; ++x)
    {
      if (canvas.GetPixel(x, y) != dim.GetPixel(x, y))
      {
        levelsDiffer = true;
        break;
      }
    }
  }
  CHECK(levelsDiffer);  // ...but different brightness
}

TEST_CASE("the built-in screen never draws outside the canvas")
{
  // Exercises every page at both panel sizes. The assertion is simply that
  // nothing crashed and the buffer is intact -- clipping is what keeps a long
  // score or a long title from writing past the end.
  for (auto size : {std::pair<int, int>{128, 32}, std::pair<int, int>{256, 64}})
  {
    GameHarness h;
    h.config().freePlay = true;
    h.Start();

    DmdCanvas canvas(static_cast<uint16_t>(size.first), static_cast<uint16_t>(size.second));
    DmdDefaultScreen screen;
    screen.SetTitle("A VERY LONG MACHINE NAME INDEED");

    for (uint64_t nowMs = 0; nowMs < 20000; nowMs += 700)
    {
      screen.Draw(canvas, h.core(), nowMs);
    }

    h.StartGameAndServe();
    h.core().AddScore(999999);
    for (uint64_t nowMs = 0; nowMs < 20000; nowMs += 700)
    {
      screen.Draw(canvas, h.core(), nowMs);
    }
    CHECK(canvas.Size() == static_cast<size_t>(size.first) * size.second);
  }
}
