#pragma once

class GameCore;
class DmdCanvas;
struct lua_State;

// Registers the `ppuc.game` and `ppuc.dmd` sub-tables.
//
// Both are registered only when the corresponding object exists, which is only
// under `engine: script`. `ppuc.game == nil` is the documented feature test, so
// a rules file that wants to run under either engine writes:
//
//     if ppuc.game then ppuc.game.addScore(1000) end
//
// Deliberately not stubbed with no-ops in ROM mode. A silent no-op turns a
// misconfigured machine into a mystery; an error names the line.
//
// The `ppuc` table must be on top of the stack when these are called.
void RegisterLuaGameApi(lua_State* L, GameCore* pGameCore);
void RegisterLuaDmdApi(lua_State* L, DmdCanvas* pCanvas);
