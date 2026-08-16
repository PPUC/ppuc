#pragma once

#include <cstdint>
#include <string>

#include "LuaArg.h"
#include <vector>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

// Shared plumbing for registering `ppuc.*` functions.
//
// Lives here rather than in LuaRulesEngine.cpp so LuaGameApi.cpp registers its
// functions the same way, with the owning object as an upvalue rather than as a
// global.

using LuaFn = int (*)(lua_State*);

// Registers a function on the table at the top of the stack, carrying `owner` as
// upvalue 1.
template <typename T>
void SetLuaFunction(lua_State* L, T* owner, const char* name, LuaFn fn)
{
  lua_pushlightuserdata(L, owner);
  lua_pushcclosure(L, fn, 1);
  lua_setfield(L, -2, name);
}

// Recovers the owner from upvalue 1.
template <typename T>
T* LuaOwner(lua_State* L)
{
  return static_cast<T*>(lua_touserdata(L, lua_upvalueindex(1)));
}

inline void PushLuaArg(lua_State* L, const LuaArg& arg)
{
  switch (arg.type)
  {
    case LuaArg::Type::Integer: lua_pushinteger(L, arg.integer); break;
    case LuaArg::Type::String: lua_pushstring(L, arg.text.c_str()); break;
    case LuaArg::Type::Boolean: lua_pushboolean(L, arg.integer != 0); break;
  }
}

// Reads an optional integer argument, defaulting when absent or nil. Used
// pervasively by ppuc.game.*, where omitting the player means "the current one".
inline lua_Integer OptionalInteger(lua_State* L, int index, lua_Integer fallback)
{
  if (lua_isnoneornil(L, index))
  {
    return fallback;
  }
  return luaL_checkinteger(L, index);
}

inline bool OptionalBoolean(lua_State* L, int index, bool fallback)
{
  if (lua_isnoneornil(L, index))
  {
    return fallback;
  }
  return lua_toboolean(L, index) != 0;
}

// Reads a field from an options table at `index`. Absent table or absent field
// both yield the fallback, so every option really is optional.
inline lua_Integer OptionField(lua_State* L, int index, const char* key, lua_Integer fallback)
{
  if (!lua_istable(L, index))
  {
    return fallback;
  }
  lua_getfield(L, index, key);
  const lua_Integer value = lua_isnoneornil(L, -1) ? fallback : luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  return value;
}

inline bool OptionFieldBoolean(lua_State* L, int index, const char* key, bool fallback)
{
  if (!lua_istable(L, index))
  {
    return fallback;
  }
  lua_getfield(L, index, key);
  const bool value = lua_isnoneornil(L, -1) ? fallback : (lua_toboolean(L, -1) != 0);
  lua_pop(L, 1);
  return value;
}

inline std::string OptionFieldString(lua_State* L, int index, const char* key, const char* fallback)
{
  if (!lua_istable(L, index))
  {
    return fallback ? fallback : "";
  }
  lua_getfield(L, index, key);
  std::string value = lua_isnoneornil(L, -1) ? (fallback ? fallback : "") : luaL_checkstring(L, -1);
  lua_pop(L, 1);
  return value;
}
