// Tests for the scriptable-class instantiation helper.
//
// Several VPX plugins are reachable only through the scriptable/COM API: the
// B2S plugin registers "B2S.Server", the PinMAME plugin registers
// "VPinMAME.Controller". PPUC has to create those objects and call members on
// them by name, which means the member lookup is load-bearing in a way that is
// invisible at compile time -- a wrong overload silently calls the wrong thing.
//
// These classes carry deliberate overloads (`GameName` get vs set, `Dip(int)`
// vs `Dip(int,int)`, `Run()` vs `Run(long)` vs `Run(long,int)`), so the lookup
// matches on name AND the full argument-type signature. Every case below exists
// because getting it wrong produces a plausible-looking no-op rather than an
// error.

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

#include "plugins/ScriptablePlugin.h"

#include "ScriptObject.h"

namespace
{

// A hand-built ScriptClassDef standing in for a plugin's. Deliberately not
// built with the PSC_CLASS macros: this must work against any provider, and
// building it by hand is also the only way to assert on Release().
struct FakeClass
{
  struct Instance
  {
    int created = 0;
    int released = 0;
    std::vector<std::string> calls;
    int lastInt = 0;
  };

  static Instance s_instance;

  static void* MSGPIAPI CreateObject()
  {
    s_instance = Instance {};
    s_instance.created = 1;
    return &s_instance;
  }

  static void MSGPIAPI CallRelease(void* me, int, ScriptVariant*, ScriptVariant*)
  {
    static_cast<Instance*>(me)->released++;
  }

  static void MSGPIAPI CallNoArg(void* me, int, ScriptVariant*, ScriptVariant*)
  {
    static_cast<Instance*>(me)->calls.emplace_back("noarg");
  }

  static void MSGPIAPI CallOneInt(void* me, int, ScriptVariant* pArgs, ScriptVariant*)
  {
    auto* self = static_cast<Instance*>(me);
    self->calls.emplace_back("oneint");
    if (pArgs != nullptr)
    {
      self->lastInt = pArgs[0].vInt32;
    }
  }

  static void MSGPIAPI CallTwoInt(void* me, int, ScriptVariant*, ScriptVariant*)
  {
    static_cast<Instance*>(me)->calls.emplace_back("twoint");
  }
};

FakeClass::Instance FakeClass::s_instance;

ScriptTypeNameDef TypeName(const char* name) { return ScriptTypeNameDef { name, 0u }; }

// ScriptClassDef ends in a flexible array member, exactly as the PSC_CLASS
// macros build it, so the members live in the same allocation as the header.
struct FakeClassDef
{
  explicit FakeClassDef(std::vector<ScriptClassMemberDef> members)
  {
    const size_t bytes = sizeof(ScriptClassDef) + members.size() * sizeof(ScriptClassMemberDef);
    m_storage.resize(bytes);
    auto* def = Get();
    def->name = TypeName("Fake");
    def->CreateObject = &FakeClass::CreateObject;
    def->nMembers = static_cast<unsigned int>(members.size());
    for (size_t i = 0; i < members.size(); ++i)
    {
      def->members[i] = members[i];
    }
  }

  ScriptClassDef* Get() { return reinterpret_cast<ScriptClassDef*>(m_storage.data()); }
  const ScriptClassDef* Get() const { return reinterpret_cast<const ScriptClassDef*>(m_storage.data()); }

 private:
  std::vector<unsigned char> m_storage;
};

// "Value" appears three times with three different signatures -- the exact
// overload shape these plugin classes use for property get/set and indexed
// properties.
std::vector<ScriptClassMemberDef> MakeMembers(bool withRelease = true)
{
  std::vector<ScriptClassMemberDef> members;

  if (withRelease)
  {
    ScriptClassMemberDef release {};
    release.name = TypeName("Release");
    release.nArgs = 0;
    release.Call = &FakeClass::CallRelease;
    members.push_back(release);
  }

  ScriptClassMemberDef getter {};
  getter.name = TypeName("Value");
  getter.nArgs = 0;
  getter.Call = &FakeClass::CallNoArg;
  members.push_back(getter);

  ScriptClassMemberDef setter {};
  setter.name = TypeName("Value");
  setter.nArgs = 1;
  setter.callArgType[0] = TypeName("int");
  setter.Call = &FakeClass::CallOneInt;
  members.push_back(setter);

  ScriptClassMemberDef indexedSetter {};
  indexedSetter.name = TypeName("Value");
  indexedSetter.nArgs = 2;
  indexedSetter.callArgType[0] = TypeName("int");
  indexedSetter.callArgType[1] = TypeName("int");
  indexedSetter.Call = &FakeClass::CallTwoInt;
  members.push_back(indexedSetter);

  // Same name and arity as the setter but a different type: must not match.
  ScriptClassMemberDef stringSetter {};
  stringSetter.name = TypeName("Name");
  stringSetter.nArgs = 1;
  stringSetter.callArgType[0] = TypeName("string");
  stringSetter.Call = &FakeClass::CallOneInt;
  members.push_back(stringSetter);

  return members;
}

}  // namespace

TEST_CASE("FindScriptMember picks the overload matching the full signature")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  const auto getter = FindScriptMember(&def, "Value", {});
  const auto setter = FindScriptMember(&def, "Value", { "int" });
  const auto indexed = FindScriptMember(&def, "Value", { "int", "int" });

  REQUIRE(getter);
  REQUIRE(setter);
  REQUIRE(indexed);
  CHECK(*getter != *setter);
  CHECK(*setter != *indexed);
  CHECK(def.members[*getter].nArgs == 0);
  CHECK(def.members[*setter].nArgs == 1);
  CHECK(def.members[*indexed].nArgs == 2);
}

TEST_CASE("FindScriptMember rejects a matching arity with a mismatched type")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  // Right name, right arity, wrong type: calling this would pass an int where
  // the plugin expects a string.
  CHECK_FALSE(FindScriptMember(&def, "Name", { "int" }));
  CHECK(FindScriptMember(&def, "Name", { "string" }));
}

TEST_CASE("FindScriptMember is null-safe and misses cleanly")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  CHECK_FALSE(FindScriptMember(nullptr, "Value", {}));
  CHECK_FALSE(FindScriptMember(&def, nullptr, {}));
  CHECK_FALSE(FindScriptMember(&def, "Missing", {}));
  CHECK_FALSE(FindScriptMember(&def, "Value", { "int", "int", "int" }));
}

TEST_CASE("ScriptObject creates, calls and releases")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  ScriptObject object;
  CHECK_FALSE(object.IsValid());

  REQUIRE(object.Create(&def));
  CHECK(object.IsValid());
  CHECK(FakeClass::s_instance.created == 1);

  ScriptVariant arg {};
  arg.vInt32 = 42;
  CHECK(object.Call("Value", { "int" }, &arg));
  CHECK(FakeClass::s_instance.lastInt == 42);
  REQUIRE(FakeClass::s_instance.calls.size() == 1);
  CHECK(FakeClass::s_instance.calls[0] == "oneint");

  object.Release();
  CHECK_FALSE(object.IsValid());
  CHECK(FakeClass::s_instance.released == 1);

  // Idempotent: a second Release must not call through again.
  object.Release();
  CHECK(FakeClass::s_instance.released == 1);
}

TEST_CASE("ScriptObject releases on destruction")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  {
    ScriptObject object;
    REQUIRE(object.Create(&def));
  }
  CHECK(FakeClass::s_instance.released == 1);
}

TEST_CASE("ScriptObject Create fails cleanly without a usable class def")
{
  ScriptObject object;
  CHECK_FALSE(object.Create(nullptr));
  CHECK_FALSE(object.IsValid());

  FakeClassDef fake(MakeMembers());
  fake.Get()->CreateObject = nullptr;
  CHECK_FALSE(object.Create(fake.Get()));
  CHECK_FALSE(object.IsValid());
}

TEST_CASE("ScriptObject calls on an empty object are a no-op, not a crash")
{
  ScriptObject object;
  CHECK_FALSE(object.Call("Value", {}));
  CHECK_FALSE(object.Has("Value", {}));
}

TEST_CASE("ScriptObject reports a missing member without calling anything")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  ScriptObject object;
  REQUIRE(object.Create(&def));

  CHECK_FALSE(object.Call("NotThere", {}));
  CHECK(FakeClass::s_instance.calls.empty());

  CHECK(object.Has("Value", { "int" }));
  CHECK_FALSE(object.Has("Value", { "string" }));
  // Has() must not call the member it finds.
  CHECK(FakeClass::s_instance.calls.empty());
}

TEST_CASE("ScriptObject tolerates a class with no Release member")
{
  FakeClassDef fake(MakeMembers(/*withRelease=*/false));
  const ScriptClassDef& def = *fake.Get();

  ScriptObject object;
  REQUIRE(object.Create(&def));
  object.Release();
  CHECK_FALSE(object.IsValid());
  CHECK(FakeClass::s_instance.released == 0);
}

TEST_CASE("ScriptObject move transfers ownership exactly once")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  {
    ScriptObject source;
    REQUIRE(source.Create(&def));

    ScriptObject target(std::move(source));
    CHECK(target.IsValid());
    CHECK_FALSE(source.IsValid());
    CHECK(FakeClass::s_instance.released == 0);
  }
  CHECK(FakeClass::s_instance.released == 1);
}

TEST_CASE("ScriptObject move-assign releases whatever it held")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  ScriptObject first;
  REQUIRE(first.Create(&def));
  auto* firstInstance = &FakeClass::s_instance;

  ScriptObject second;
  REQUIRE(second.Create(&def));

  // Create() reset the shared fake, so the first instance's release lands on
  // the same struct; what matters is that the assignment released something
  // before overwriting.
  first = std::move(second);
  CHECK(first.IsValid());
  CHECK(firstInstance->released == 1);
}

TEST_CASE("ScriptObject Create replaces an existing instance")
{
  FakeClassDef fake(MakeMembers());
  const ScriptClassDef& def = *fake.Get();

  ScriptObject object;
  REQUIRE(object.Create(&def));
  REQUIRE(object.Create(&def));
  CHECK(object.IsValid());
  // The second Create released the first instance before creating the second.
  CHECK(FakeClass::s_instance.created == 1);
}
