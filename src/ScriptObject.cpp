#include "ScriptObject.h"

#include <cstring>
#include <utility>

#include "plugins/ScriptablePlugin.h"

std::optional<unsigned int> FindScriptMember(const ScriptClassDef* classDef, const char* name,
                                             std::initializer_list<const char*> argTypes)
{
  if (classDef == nullptr || name == nullptr)
  {
    return std::nullopt;
  }

  for (unsigned int i = 0; i < classDef->nMembers; ++i)
  {
    const ScriptClassMemberDef& member = classDef->members[i];
    if (member.name.name == nullptr || std::strcmp(member.name.name, name) != 0 ||
        member.nArgs != argTypes.size() || member.Call == nullptr)
    {
      continue;
    }

    bool argsMatch = true;
    unsigned int argIndex = 0;
    for (const char* expectedType : argTypes)
    {
      const char* actualType = member.callArgType[argIndex].name;
      if (expectedType == nullptr || actualType == nullptr || std::strcmp(actualType, expectedType) != 0)
      {
        argsMatch = false;
        break;
      }
      ++argIndex;
    }
    if (argsMatch)
    {
      return i;
    }
  }
  return std::nullopt;
}

ScriptObject::~ScriptObject() { Release(); }

ScriptObject::ScriptObject(ScriptObject&& other) noexcept
    : m_classDef(std::exchange(other.m_classDef, nullptr)), m_instance(std::exchange(other.m_instance, nullptr))
{
}

ScriptObject& ScriptObject::operator=(ScriptObject&& other) noexcept
{
  if (this != &other)
  {
    Release();
    m_classDef = std::exchange(other.m_classDef, nullptr);
    m_instance = std::exchange(other.m_instance, nullptr);
  }
  return *this;
}

bool ScriptObject::Create(const ScriptClassDef* classDef)
{
  Release();

  if (classDef == nullptr || classDef->CreateObject == nullptr)
  {
    return false;
  }

  void* instance = classDef->CreateObject();
  if (instance == nullptr)
  {
    return false;
  }

  m_classDef = classDef;
  m_instance = instance;
  return true;
}

void ScriptObject::Release()
{
  if (m_instance == nullptr || m_classDef == nullptr)
  {
    m_classDef = nullptr;
    m_instance = nullptr;
    return;
  }

  // Every PSC_CLASS gets AddRef at member 0 and Release at member 1 with a
  // refcount starting at 1, so this drops the last reference and destroys the
  // object. Looked up by name rather than assumed to be index 1, because a
  // hand-rolled ScriptClassDef need not follow the macro's layout.
  if (const auto member = FindScriptMember(m_classDef, "Release", {}))
  {
    m_classDef->members[*member].Call(m_instance, static_cast<int>(*member), nullptr, nullptr);
  }

  m_classDef = nullptr;
  m_instance = nullptr;
}

bool ScriptObject::Call(const char* name, std::initializer_list<const char*> argTypes, ScriptVariant* pArgs,
                        ScriptVariant* pRet) const
{
  if (m_instance == nullptr || m_classDef == nullptr)
  {
    return false;
  }

  const auto member = FindScriptMember(m_classDef, name, argTypes);
  if (!member)
  {
    return false;
  }

  m_classDef->members[*member].Call(m_instance, static_cast<int>(*member), pArgs, pRet);
  return true;
}

bool ScriptObject::Has(const char* name, std::initializer_list<const char*> argTypes) const
{
  return m_classDef != nullptr && FindScriptMember(m_classDef, name, argTypes).has_value();
}
