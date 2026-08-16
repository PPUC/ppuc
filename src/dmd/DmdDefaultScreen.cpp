#include "DmdDefaultScreen.h"

namespace
{
using Align = DmdTextOptions::Align;

DmdTextOptions Label(uint8_t level = 15)
{
  DmdTextOptions options;
  options.level = level;
  options.scale = 1;
  return options;
}

DmdTextOptions Score(uint8_t scale, uint8_t level = 15)
{
  DmdTextOptions options;
  options.level = level;
  options.scale = scale;
  options.commas = true;
  options.align = Align::Right;
  return options;
}

// Scores get whatever scale still fits the available width. A six-digit score
// at scale 2 is 69px, which fits 128; an eight-digit one does not, and shrinking
// beats truncating.
uint8_t FitScale(const std::string& text, int availableWidth, uint8_t preferred)
{
  for (uint8_t scale = preferred; scale > 1; --scale)
  {
    DmdTextOptions probe;
    probe.scale = scale;
    if (DmdCanvas::TextWidth(text, probe) <= availableWidth)
    {
      return scale;
    }
  }
  return 1;
}

bool BlinkOn(uint64_t nowMs, uint32_t periodMs) { return (nowMs / (periodMs / 2)) % 2 == 0; }
}  // namespace

void DmdDefaultScreen::Draw(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const
{
  canvas.Clear(0);

  if (game.IsTilted(0) && game.IsInGame())
  {
    DrawTilt(canvas, nowMs);
    return;
  }

  switch (game.GetState())
  {
    case GameState::Attract:
      DrawAttract(canvas, game, nowMs);
      return;
    case GameState::GameOver:
      DrawGameOver(canvas, game, nowMs);
      return;
    default:
      DrawScores(canvas, game);
      DrawStatusRow(canvas, game);
      return;
  }
}

void DmdDefaultScreen::DrawAttract(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const
{
  const int centerX = canvas.Width() / 2;
  const int page = m_attractPageMs == 0 ? 0 : static_cast<int>((nowMs / m_attractPageMs) % 3);

  DmdTextOptions title = Label();
  title.align = Align::Center;
  title.scale = canvas.Height() >= 64 ? 3 : 2;

  DmdTextOptions line = Label();
  line.align = Align::Center;

  switch (page)
  {
    case 0:
      canvas.Text(centerX, canvas.Height() / 2 - 7, m_title, title);
      break;

    case 1:
    {
      canvas.Text(centerX, 4, "HIGH SCORE", line);
      DmdTextOptions value = Score(2);
      value.align = Align::Center;
      canvas.Text(centerX, 14, DmdCanvas::FormatNumber(static_cast<int64_t>(game.GetHighScore()), value), value);
      break;
    }

    default:
      if (game.GetConfig().freePlay)
      {
        canvas.Text(centerX, 4, "FREE PLAY", line);
      }
      else
      {
        DmdTextOptions credits = line;
        canvas.Text(centerX, 4, "CREDITS " + DmdCanvas::FormatNumber(game.GetCredits(), Label()), credits);
      }
      // Blinks so an idle machine reads as alive rather than as frozen.
      if (BlinkOn(nowMs, 1000))
      {
        canvas.Text(centerX, canvas.Height() - 12, "PRESS START", line);
      }
      break;
  }
}

void DmdDefaultScreen::DrawTilt(DmdCanvas& canvas, uint64_t nowMs) const
{
  // Deliberately loud: the player has just lost their flippers and needs to
  // know why rather than assuming the machine broke.
  const uint8_t level = BlinkOn(nowMs, 500) ? 15 : 4;
  DmdTextOptions tilt = Label(level);
  tilt.align = Align::Center;
  tilt.scale = canvas.Height() >= 64 ? 4 : 3;
  canvas.Text(canvas.Width() / 2, canvas.Height() / 2 - 10, "TILT", tilt);
}

void DmdDefaultScreen::DrawScores(DmdCanvas& canvas, const GameCore& game) const
{
  const int right = canvas.Width() - 2;
  const uint8_t players = game.GetPlayerCount();
  const uint8_t current = game.GetCurrentPlayer();

  if (players <= 1)
  {
    const std::string text = DmdCanvas::FormatNumber(static_cast<int64_t>(game.GetScore(1)), Score(1));
    DmdTextOptions options = Score(FitScale(text, canvas.Width() - 4, 3));
    canvas.Text(right, 2, text, options);
    return;
  }

  if (players == 2)
  {
    for (uint8_t player = 1; player <= 2; ++player)
    {
      const bool up = player == current;
      const std::string text = DmdCanvas::FormatNumber(static_cast<int64_t>(game.GetScore(player)), Score(1));
      DmdTextOptions options = Score(FitScale(text, canvas.Width() - 8, 2), up ? 15 : 7);
      const int y = 1 + (player - 1) * 11;
      canvas.Text(right, y, text, options);
      if (up)
      {
        canvas.Text(0, y + 2, ">", Label());
      }
    }
    return;
  }

  // Three or four players: a 2x2 grid, current player's cell inverted so the
  // player up is obvious at a glance across the room.
  const int cellW = canvas.Width() / 2;
  const int cellH = 11;
  for (uint8_t player = 1; player <= players && player <= 4; ++player)
  {
    const int column = (player - 1) % 2;
    const int row = (player - 1) / 2;
    const int x = column * cellW;
    const int y = row * cellH;
    const bool up = player == current;

    if (up)
    {
      canvas.Fill(x, y, cellW - 1, cellH - 1, 15);
    }

    const std::string text = DmdCanvas::FormatNumber(static_cast<int64_t>(game.GetScore(player)), Score(1));
    DmdTextOptions options = Score(FitScale(text, cellW - 4, 1), up ? 0 : 12);
    canvas.Text(x + cellW - 3, y + 2, text, options);
  }
}

void DmdDefaultScreen::DrawStatusRow(DmdCanvas& canvas, const GameCore& game) const
{
  const int y = canvas.Height() - 8;
  canvas.Text(1, y, "BALL " + DmdCanvas::FormatNumber(game.GetCurrentBall(), Label()), Label());

  DmdTextOptions right = Label();
  right.align = Align::Right;
  canvas.Text(canvas.Width() - 2, y, "PLAYER " + DmdCanvas::FormatNumber(game.GetCurrentPlayer(), Label()), right);
}

void DmdDefaultScreen::DrawGameOver(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const
{
  // Alternates between the banner and the final scores so a four-player game
  // still gets to see who won.
  const bool banner = (nowMs / 2000) % 2 == 0;
  if (banner)
  {
    DmdTextOptions text = Label();
    text.align = Align::Center;
    text.scale = 2;
    canvas.Text(canvas.Width() / 2, canvas.Height() / 2 - 7, "GAME OVER", text);
    return;
  }

  DrawScores(canvas, game);
  const int matchDigits = game.GetMatchDigits();
  if (matchDigits >= 0)
  {
    DmdTextOptions match = Label();
    match.digits = 2;
    match.pad = '0';
    canvas.Text(1, canvas.Height() - 8, "MATCH " + DmdCanvas::FormatNumber(matchDigits, match), Label());
  }
}
