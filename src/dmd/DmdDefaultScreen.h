#pragma once

#include <cstdint>
#include <string>

#include "DmdCanvas.h"
#include "game/GameCore.h"

// The screen a machine gets with no DMD scripting at all.
//
// A pure function of GameCore state: it reads scores, ball, player, credits and
// tilt through the same accessors ppuc.game.* uses, so the display and the
// machine cannot disagree. That is the payoff for having GameCore own ball,
// player and score rather than leaving them to rules.
class DmdDefaultScreen
{
 public:
  void SetTitle(const std::string& title) { m_title = title; }
  void SetAttractPageMs(uint32_t ms) { m_attractPageMs = ms; }

  // Draws the current page for `state`. `nowMs` drives attract paging and the
  // tilt/press-start blink, and comes from the same clock as GameCore so tests
  // are deterministic.
  void Draw(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const;

 private:
  void DrawAttract(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const;
  void DrawTilt(DmdCanvas& canvas, uint64_t nowMs) const;
  void DrawScores(DmdCanvas& canvas, const GameCore& game) const;
  void DrawGameOver(DmdCanvas& canvas, const GameCore& game, uint64_t nowMs) const;
  void DrawStatusRow(DmdCanvas& canvas, const GameCore& game) const;

  std::string m_title = "PPUC";
  uint32_t m_attractPageMs = 4000;
};
