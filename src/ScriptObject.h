#pragma once

#include <initializer_list>
#include <optional>

struct ScriptClassDef;
union ScriptVariant;

// Instantiating and calling into a plugin-provided scriptable class.
//
// Several VPX plugins are reachable ONLY through the scriptable/COM API, not
// through any bus message: the B2S plugin registers "B2S.Server" and the
// PinMAME plugin registers "VPinMAME.Controller", each via
// ScriptablePluginAPI::SetCOMObjectOverride. A host that wants a backglass, or
// that wants to start a ROM, has to create the object and call members on it by
// name. There is no alternative entry point.
//
// This header deliberately depends on nothing but the plugin SDK's forward
// declarations and the standard library, so it can be unit tested against a
// hand-built ScriptClassDef with no plugin loaded.

// Finds a member by name and exact argument-type signature, returning its index.
//
// The signature match is what disambiguates the overloads these classes rely
// on: `GameName` getter (0 args) from setter (1 arg), `Dip(int)` from
// `Dip(int,int)`, `Run()` from `Run(long)` from `Run(long,int)`. Matching on
// name alone silently picks whichever came first.
std::optional<unsigned int> FindScriptMember(const ScriptClassDef* classDef, const char* name,
                                             std::initializer_list<const char*> argTypes);

// One live instance of a scriptable class. Move-only; releases on destruction.
//
// NOT thread safe, and must not outlive the plugin that registered its
// ScriptClassDef: at plugin unload the class definition and the code page
// behind every member's Call pointer go away together. Release() before the
// owning plugin is unloaded.
class ScriptObject final
{
 public:
  ScriptObject() = default;
  ~ScriptObject();

  ScriptObject(const ScriptObject&) = delete;
  ScriptObject& operator=(const ScriptObject&) = delete;
  ScriptObject(ScriptObject&& other) noexcept;
  ScriptObject& operator=(ScriptObject&& other) noexcept;

  // Creates an instance. Returns false and stays empty when `classDef` is null
  // or has no CreateObject, or when CreateObject itself fails. Releases any
  // instance already held.
  bool Create(const ScriptClassDef* classDef);

  // Calls the class's "Release" member if it has one, then drops the instance.
  // Idempotent, and safe on an empty object.
  void Release();

  bool IsValid() const { return m_instance != nullptr; }

  // Calls a member by name and signature. Returns false when the object is
  // empty or no member matches -- callers treat that as "this plugin build does
  // not expose it", which is why it is not an error.
  bool Call(const char* name, std::initializer_list<const char*> argTypes, ScriptVariant* pArgs = nullptr,
            ScriptVariant* pRet = nullptr) const;

  // True when a matching member exists, without calling it. For probing
  // optional API surface.
  bool Has(const char* name, std::initializer_list<const char*> argTypes) const;

 private:
  const ScriptClassDef* m_classDef = nullptr;
  void* m_instance = nullptr;
};
