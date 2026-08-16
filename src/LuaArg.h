#pragma once

#include <cstdint>
#include <string>

// One argument passed to a Lua handler.
//
// Deliberately free of any Lua header: LuaRulesEngine.h includes this, and
// ppuc.cpp includes that only to dispatch handlers. Pushing a LuaArg onto a
// stack lives in LuaApiSupport.h, which the Lua translation units include.
//
// Strings exist because "noCredits" is a far better thing to receive in
// onStartRejected than 3, and because it reads properly in a Blockly block.
struct LuaArg
{
  enum class Type
  {
    Integer,
    String,
    Boolean,
  };

  Type type = Type::Integer;
  int64_t integer = 0;
  std::string text;

  LuaArg(int value) : type(Type::Integer), integer(value) {}
  LuaArg(unsigned int value) : type(Type::Integer), integer(value) {}
  LuaArg(long value) : type(Type::Integer), integer(value) {}
  LuaArg(long long value) : type(Type::Integer), integer(static_cast<int64_t>(value)) {}
  LuaArg(unsigned long long value) : type(Type::Integer), integer(static_cast<int64_t>(value)) {}
  LuaArg(bool value) : type(Type::Boolean), integer(value ? 1 : 0) {}
  LuaArg(const char* value) : type(Type::String), text(value ? value : "") {}
  LuaArg(std::string value) : type(Type::String), text(std::move(value)) {}
};
