#include "LuaGameApi.h"

#include <string>

#include "LuaApiSupport.h"
#include "dmd/DmdCanvas.h"
#include "game/GameCore.h"

namespace
{

// ------------------------------------------------------------- ppuc.game ----

GameCore* Game(lua_State* L) { return LuaOwner<GameCore>(L); }

// Player argument convention: absent or 0 means the current player. Applied
// everywhere so a rule never has to write ppuc.game.player() explicitly.
uint8_t PlayerArg(lua_State* L, int index)
{
  return static_cast<uint8_t>(OptionalInteger(L, index, 0));
}

int GameState_(lua_State* L)
{
  lua_pushstring(L, GameStateName(Game(L)->GetState()));
  return 1;
}

int GameInGame(lua_State* L)
{
  lua_pushboolean(L, Game(L)->IsInGame());
  return 1;
}

int GameAttract(lua_State* L)
{
  lua_pushboolean(L, Game(L)->IsAttract());
  return 1;
}

int GamePlayers(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetPlayerCount());
  return 1;
}

int GamePlayer(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetCurrentPlayer());
  return 1;
}

int GameBall(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetCurrentBall());
  return 1;
}

int GameBallsPerGame(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetConfig().ballsPerGame);
  return 1;
}

int GameScore(lua_State* L)
{
  lua_pushinteger(L, static_cast<lua_Integer>(Game(L)->GetScore(PlayerArg(L, 1))));
  return 1;
}

int GameHighScore(lua_State* L)
{
  lua_pushinteger(L, static_cast<lua_Integer>(Game(L)->GetHighScore()));
  return 1;
}

int GameHighPlayer(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetHighPlayer());
  return 1;
}

int GameTilted(lua_State* L)
{
  lua_pushboolean(L, Game(L)->IsTilted(PlayerArg(L, 1)));
  return 1;
}

int GameExtraBalls(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetExtraBalls(PlayerArg(L, 1)));
  return 1;
}

int GameCredits(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetCredits());
  return 1;
}

int GameBallsInTrough(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetBallsInTrough());
  return 1;
}

int GameBallsInPlay(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetBallsInPlay());
  return 1;
}

int GameMatchDigits(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetMatchDigits());
  return 1;
}

// ---- Awards ----
// addScore and awardExtraBall are silently ignored while tilted or out of play,
// inside GameCore. No rule has to guard them, which is the single most common
// homebrew rules bug removed by construction.

int GameAddScore(lua_State* L)
{
  Game(L)->AddScore(luaL_checkinteger(L, 1), PlayerArg(L, 2));
  return 0;
}

int GameSetScore(lua_State* L)
{
  Game(L)->SetScore(static_cast<uint64_t>(luaL_checkinteger(L, 1)), PlayerArg(L, 2));
  return 0;
}

int GameAwardExtraBall(lua_State* L)
{
  Game(L)->AwardExtraBall(PlayerArg(L, 1));
  return 0;
}

int GameAddCredits(lua_State* L)
{
  Game(L)->AddCredits(static_cast<int>(luaL_checkinteger(L, 1)));
  return 0;
}

int GameKnock(lua_State* L)
{
  Game(L)->Knock();
  return 0;
}

// ---- Control ----

int GameStartGame(lua_State* L)
{
  lua_pushboolean(L, Game(L)->StartGame());
  return 1;
}

int GameAddPlayer(lua_State* L)
{
  lua_pushboolean(L, Game(L)->AddPlayer());
  return 1;
}

int GameTilt(lua_State* L)
{
  Game(L)->Tilt();
  return 0;
}

int GameSlamTilt(lua_State* L)
{
  Game(L)->SlamTilt();
  return 0;
}

int GameEndBall(lua_State* L)
{
  Game(L)->EndBall();
  return 0;
}

int GameEndGame(lua_State* L)
{
  Game(L)->EndGame();
  return 0;
}

int GameServeBall(lua_State* L)
{
  Game(L)->ServeBall();
  return 0;
}

int GameBonusDone(lua_State* L)
{
  Game(L)->BonusDone();
  return 0;
}

// ---- Variables ----
// Somewhere for a rule to keep mode state without inventing its own storage.
// Per-player values live for the game; per-ball values are cleared at each ball
// start, but survive an extra ball, because shoot again continues the ball.

int GameGetVar(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetVar(luaL_checkstring(L, 1), PlayerArg(L, 2)));
  return 1;
}

int GameSetVar(lua_State* L)
{
  Game(L)->SetVar(luaL_checkstring(L, 1), luaL_checkinteger(L, 2), PlayerArg(L, 3));
  return 0;
}

int GameAddVar(lua_State* L)
{
  lua_pushinteger(L, Game(L)->AddVar(luaL_checkstring(L, 1), luaL_checkinteger(L, 2), PlayerArg(L, 3)));
  return 1;
}

int GameGetBallVar(lua_State* L)
{
  lua_pushinteger(L, Game(L)->GetBallVar(luaL_checkstring(L, 1)));
  return 1;
}

int GameSetBallVar(lua_State* L)
{
  Game(L)->SetBallVar(luaL_checkstring(L, 1), luaL_checkinteger(L, 2));
  return 0;
}

int GameAddBallVar(lua_State* L)
{
  lua_pushinteger(L, Game(L)->AddBallVar(luaL_checkstring(L, 1), luaL_checkinteger(L, 2)));
  return 1;
}

// ---- Wiring ----
// Resolves the friendly names from emGame.names, so a rules set survives a
// rewire. Zero when the name is unknown.

int GameCoil(lua_State* L)
{
  lua_pushinteger(L, Game(L)->ResolveCoil(luaL_checkstring(L, 1)));
  return 1;
}

int GameSwitch(lua_State* L)
{
  lua_pushinteger(L, Game(L)->ResolveSwitch(luaL_checkstring(L, 1)));
  return 1;
}

int GameLamp(lua_State* L)
{
  lua_pushinteger(L, Game(L)->ResolveLamp(luaL_checkstring(L, 1)));
  return 1;
}

// -------------------------------------------------------------- ppuc.dmd ----

DmdCanvas* Canvas(lua_State* L) { return LuaOwner<DmdCanvas>(L); }

// Every drawing call takes an optional options table as its last argument.
DmdTextOptions ReadTextOptions(lua_State* L, int index)
{
  DmdTextOptions options;
  options.level = static_cast<uint8_t>(OptionField(L, index, "level", 15));
  options.scale = static_cast<uint8_t>(OptionField(L, index, "scale", 1));
  options.spacing = static_cast<int>(OptionField(L, index, "spacing", 0));
  options.commas = OptionFieldBoolean(L, index, "commas", false);
  options.digits = static_cast<int>(OptionField(L, index, "digits", 0));

  const std::string pad = OptionFieldString(L, index, "pad", " ");
  options.pad = pad.empty() ? ' ' : pad[0];

  const std::string align = OptionFieldString(L, index, "align", "left");
  if (align == "right")
  {
    options.align = DmdTextOptions::Align::Right;
  }
  else if (align == "center" || align == "centre")
  {
    options.align = DmdTextOptions::Align::Center;
  }
  return options;
}

int DmdWidth(lua_State* L)
{
  lua_pushinteger(L, Canvas(L)->Width());
  return 1;
}

int DmdHeight(lua_State* L)
{
  lua_pushinteger(L, Canvas(L)->Height());
  return 1;
}

int DmdColor(lua_State* L)
{
  Canvas(L)->SetColor(static_cast<uint8_t>(luaL_checkinteger(L, 1)), static_cast<uint8_t>(luaL_checkinteger(L, 2)),
                      static_cast<uint8_t>(luaL_checkinteger(L, 3)));
  return 0;
}

int DmdClear(lua_State* L)
{
  Canvas(L)->Clear(static_cast<uint8_t>(OptionalInteger(L, 1, 0)));
  return 0;
}

int DmdPixel(lua_State* L)
{
  Canvas(L)->Pixel(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)),
                   static_cast<uint8_t>(OptionalInteger(L, 3, 15)));
  return 0;
}

int DmdLine(lua_State* L)
{
  Canvas(L)->Line(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)),
                  static_cast<int>(luaL_checkinteger(L, 3)), static_cast<int>(luaL_checkinteger(L, 4)),
                  static_cast<uint8_t>(OptionalInteger(L, 5, 15)));
  return 0;
}

int DmdRect(lua_State* L)
{
  Canvas(L)->Rect(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)),
                  static_cast<int>(luaL_checkinteger(L, 3)), static_cast<int>(luaL_checkinteger(L, 4)),
                  static_cast<uint8_t>(OptionalInteger(L, 5, 15)), OptionalBoolean(L, 6, false));
  return 0;
}

int DmdFill(lua_State* L)
{
  Canvas(L)->Fill(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2)),
                  static_cast<int>(luaL_checkinteger(L, 3)), static_cast<int>(luaL_checkinteger(L, 4)),
                  static_cast<uint8_t>(OptionalInteger(L, 5, 15)));
  return 0;
}

int DmdText(lua_State* L)
{
  const int drawn = Canvas(L)->Text(static_cast<int>(luaL_checkinteger(L, 1)),
                                    static_cast<int>(luaL_checkinteger(L, 2)), luaL_checkstring(L, 3),
                                    ReadTextOptions(L, 4));
  lua_pushinteger(L, drawn);
  return 1;
}

int DmdNumber(lua_State* L)
{
  const int drawn = Canvas(L)->Number(static_cast<int>(luaL_checkinteger(L, 1)),
                                      static_cast<int>(luaL_checkinteger(L, 2)), luaL_checkinteger(L, 3),
                                      ReadTextOptions(L, 4));
  lua_pushinteger(L, drawn);
  return 1;
}

// Exists so a script can right-align without knowing font metrics.
int DmdTextWidth(lua_State* L)
{
  lua_pushinteger(L, DmdCanvas::TextWidth(luaL_checkstring(L, 1), ReadTextOptions(L, 2)));
  return 1;
}

int DmdFormatNumber(lua_State* L)
{
  lua_pushstring(L, DmdCanvas::FormatNumber(luaL_checkinteger(L, 1), ReadTextOptions(L, 2)).c_str());
  return 1;
}

}  // namespace

void RegisterLuaGameApi(lua_State* L, GameCore* pGameCore)
{
  if (pGameCore == nullptr)
  {
    return;
  }

  lua_newtable(L);

  SetLuaFunction(L, pGameCore, "state", GameState_);
  SetLuaFunction(L, pGameCore, "inGame", GameInGame);
  SetLuaFunction(L, pGameCore, "attract", GameAttract);
  SetLuaFunction(L, pGameCore, "players", GamePlayers);
  SetLuaFunction(L, pGameCore, "player", GamePlayer);
  SetLuaFunction(L, pGameCore, "ball", GameBall);
  SetLuaFunction(L, pGameCore, "ballsPerGame", GameBallsPerGame);
  SetLuaFunction(L, pGameCore, "score", GameScore);
  SetLuaFunction(L, pGameCore, "highScore", GameHighScore);
  SetLuaFunction(L, pGameCore, "highPlayer", GameHighPlayer);
  SetLuaFunction(L, pGameCore, "tilted", GameTilted);
  SetLuaFunction(L, pGameCore, "extraBalls", GameExtraBalls);
  SetLuaFunction(L, pGameCore, "credits", GameCredits);
  SetLuaFunction(L, pGameCore, "ballsInTrough", GameBallsInTrough);
  SetLuaFunction(L, pGameCore, "ballsInPlay", GameBallsInPlay);
  SetLuaFunction(L, pGameCore, "matchDigits", GameMatchDigits);

  SetLuaFunction(L, pGameCore, "addScore", GameAddScore);
  SetLuaFunction(L, pGameCore, "setScore", GameSetScore);
  SetLuaFunction(L, pGameCore, "awardExtraBall", GameAwardExtraBall);
  SetLuaFunction(L, pGameCore, "addCredits", GameAddCredits);
  SetLuaFunction(L, pGameCore, "knock", GameKnock);

  SetLuaFunction(L, pGameCore, "startGame", GameStartGame);
  SetLuaFunction(L, pGameCore, "addPlayer", GameAddPlayer);
  SetLuaFunction(L, pGameCore, "tilt", GameTilt);
  SetLuaFunction(L, pGameCore, "slamTilt", GameSlamTilt);
  SetLuaFunction(L, pGameCore, "endBall", GameEndBall);
  SetLuaFunction(L, pGameCore, "endGame", GameEndGame);
  SetLuaFunction(L, pGameCore, "serveBall", GameServeBall);
  SetLuaFunction(L, pGameCore, "bonusDone", GameBonusDone);

  SetLuaFunction(L, pGameCore, "get", GameGetVar);
  SetLuaFunction(L, pGameCore, "set", GameSetVar);
  SetLuaFunction(L, pGameCore, "add", GameAddVar);
  SetLuaFunction(L, pGameCore, "getBall", GameGetBallVar);
  SetLuaFunction(L, pGameCore, "setBall", GameSetBallVar);
  SetLuaFunction(L, pGameCore, "addBall", GameAddBallVar);

  SetLuaFunction(L, pGameCore, "coil", GameCoil);
  SetLuaFunction(L, pGameCore, "switch", GameSwitch);
  SetLuaFunction(L, pGameCore, "lamp", GameLamp);

  lua_setfield(L, -2, "game");
}

void RegisterLuaDmdApi(lua_State* L, DmdCanvas* pCanvas)
{
  if (pCanvas == nullptr)
  {
    return;
  }

  lua_newtable(L);

  SetLuaFunction(L, pCanvas, "width", DmdWidth);
  SetLuaFunction(L, pCanvas, "height", DmdHeight);
  SetLuaFunction(L, pCanvas, "color", DmdColor);
  SetLuaFunction(L, pCanvas, "clear", DmdClear);
  SetLuaFunction(L, pCanvas, "pixel", DmdPixel);
  SetLuaFunction(L, pCanvas, "line", DmdLine);
  SetLuaFunction(L, pCanvas, "rect", DmdRect);
  SetLuaFunction(L, pCanvas, "fill", DmdFill);
  SetLuaFunction(L, pCanvas, "text", DmdText);
  SetLuaFunction(L, pCanvas, "number", DmdNumber);
  SetLuaFunction(L, pCanvas, "textWidth", DmdTextWidth);
  SetLuaFunction(L, pCanvas, "formatNumber", DmdFormatNumber);

  lua_setfield(L, -2, "dmd");
}
