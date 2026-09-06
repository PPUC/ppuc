#include "PluginBus.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "SDL3/SDL.h"

namespace
{

// Loads plugin shared objects through SDL, which already abstracts the
// per-platform dynamic loader PPUC needs.
class SDLModuleLoader final : public MsgPI::MsgModuleLoader
{
 public:
  void* Link(const std::string& directory, const std::string& file) override
  {
#if defined(_WIN32) || defined(__MINGW32__)
    SetDllDirectoryA(directory.c_str());
#else
    (void)directory;
#endif
    void* module = SDL_LoadObject(file.c_str());
#if defined(_WIN32) || defined(__MINGW32__)
    SetDllDirectoryA(nullptr);
#endif
    return module;
  }

  void Unlink(void* dynamicModule) override { SDL_UnloadObject(static_cast<SDL_SharedObject*>(dynamicModule)); }

  void* GetFunction(void* dynamicModule, const std::string& functionName) override
  {
    return reinterpret_cast<void*>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(dynamicModule), functionName.c_str()));
  }
};

void MSGPIAPI HostPluginLoad(const uint32_t, const MsgPluginAPI*) {}
void MSGPIAPI HostPluginUnload() {}

}  // namespace

PluginBus* PluginBus::s_instance = nullptr;

ScriptablePluginAPI PluginBus::s_scriptApi = {
    .RegisterScriptClass = &PluginBus::RegisterScriptClass,
    .RegisterScriptTypeAlias = &PluginBus::RegisterScriptTypeAlias,
    .RegisterScriptArrayType = &PluginBus::RegisterScriptArrayType,
    .SubmitTypeLibrary = &PluginBus::SubmitTypeLibrary,
    .UnregisterScriptClass = &PluginBus::UnregisterScriptClass,
    .UnregisterScriptTypeAlias = &PluginBus::UnregisterScriptTypeAlias,
    .UnregisterScriptArrayType = &PluginBus::UnregisterScriptArrayType,
    .OnError = &PluginBus::OnScriptError,
    .SetCOMObjectOverride = &PluginBus::SetCOMObjectOverride,
    .GetClassDef = &PluginBus::GetClassDef,
};

LoggingPluginAPI PluginBus::s_loggingApi = {
    .Log = &PluginBus::Log,
};

PluginBus::PluginBus()
{
  // The plugin APIs are C structs with no context pointer, so their statics
  // have to reach one instance. main() creates exactly one.
  assert(s_instance == nullptr && "only one PluginBus may exist");
  s_instance = this;
}

PluginBus::~PluginBus()
{
  Shutdown();
  if (s_instance == this)
  {
    s_instance = nullptr;
  }
}

const MsgPluginAPI& PluginBus::Api() const { return m_manager.GetMsgAPI(); }

bool PluginBus::Initialize(const std::string& pluginDir, std::string* errorMessage)
{
  if (m_initialized)
  {
    return true;
  }

  m_hostPlugin = m_manager.RegisterPlugin("PPUC", "PPUC", "PPUC plugin host", "", "", "", HostPluginLoad, HostPluginUnload);
  m_manager.LoadPlugin(*m_hostPlugin);
  m_hostEndpointId = m_hostPlugin->m_endpointId;
  assert(m_hostEndpointId != 0 && "RegisterPlugin must assign an endpoint id");

  m_manager.SetSettingsHandler([this](const std::string& pluginId, MsgPI::MsgPluginManager::SettingAction action,
                                      MsgSettingDef* settingDef) { ConfigureSetting(pluginId, action, settingDef); });

  const MsgPluginAPI& api = Api();
  m_getScriptApiId = api.GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
  m_getLoggingApiId = api.GetMsgID(LOGPI_NAMESPACE, LOGPI_MSG_GET_API);
  api.SubscribeMsg(m_hostEndpointId, m_getScriptApiId, OnGetScriptApi, this);
  api.SubscribeMsg(m_hostEndpointId, m_getLoggingApiId, OnGetLoggingApi, this);

  if (!std::filesystem::exists(pluginDir))
  {
    if (errorMessage != nullptr)
    {
      *errorMessage = "plugin directory does not exist: " + pluginDir;
    }
    return false;
  }
  std::printf("Plugin directory: %s\n", pluginDir.c_str());
  m_manager.ScanPluginFolder(std::make_shared<SDLModuleLoader>(), pluginDir, [](MsgPI::MsgPlugin&) {});

  m_initialized = true;
  return true;
}

void PluginBus::Shutdown()
{
  if (!m_initialized)
  {
    return;
  }

  const MsgPluginAPI& api = Api();
  for (auto it = m_loadedPlugins.rbegin(); it != m_loadedPlugins.rend(); ++it)
  {
    if (*it && (*it)->IsLoaded())
    {
      m_manager.UnloadPlugin(**it);
    }
  }
  m_loadedPlugins.clear();

  api.UnsubscribeMsg(m_getScriptApiId, OnGetScriptApi, this);
  api.UnsubscribeMsg(m_getLoggingApiId, OnGetLoggingApi, this);
  api.ReleaseMsgID(m_getScriptApiId);
  api.ReleaseMsgID(m_getLoggingApiId);

  // Host last: unloading it tears down the endpoint every other subscription
  // was registered against.
  if (m_hostPlugin && m_hostPlugin->IsLoaded())
  {
    m_manager.UnloadPlugin(*m_hostPlugin);
  }
  m_hostPlugin.reset();
  m_manager.SetSettingsHandler(nullptr);

  m_scriptClasses.clear();
  m_comOverrides.clear();
  m_initialized = false;
}

bool PluginBus::LoadPluginById(const std::string& id)
{
  for (const auto& plugin : m_manager.GetPlugins())
  {
    if (plugin->m_id == id)
    {
      m_manager.LoadPlugin(*plugin);
      m_loadedPlugins.push_back(plugin);
      return true;
    }
  }
  std::printf("Plugin not found: %s\n", id.c_str());
  return false;
}

void PluginBus::Process() { m_manager.ProcessAsyncCallbacks(); }

void PluginBus::SetSettingOverride(const std::string& pluginId, const std::string& propId, const std::string& value)
{
  m_settingOverrides[{pluginId, propId}] = value;
}

void PluginBus::ConfigureSetting(const std::string& pluginId, MsgPI::MsgPluginManager::SettingAction action,
                                 MsgSettingDef* settingDef)
{
  if (action != MsgPI::MsgPluginManager::SettingAction::Load || settingDef == nullptr)
  {
    return;
  }

  if (settingDef->type == MSGPI_SETTING_TYPE_STRING)
  {
    const char* value = settingDef->stringDef.defVal;
    if (settingDef->propId != nullptr)
    {
      const auto it = m_settingOverrides.find({pluginId, settingDef->propId});
      if (it != m_settingOverrides.end())
      {
        value = it->second.c_str();
      }
    }
    if (settingDef->stringDef.Set != nullptr)
    {
      settingDef->stringDef.Set(value ? value : "");
    }
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_BOOL && settingDef->boolDef.Set != nullptr)
  {
    settingDef->boolDef.Set(settingDef->boolDef.defVal);
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_INT && settingDef->intDef.Set != nullptr)
  {
    settingDef->intDef.Set(settingDef->intDef.defVal);
  }
  else if (settingDef->type == MSGPI_SETTING_TYPE_FLOAT && settingDef->floatDef.Set != nullptr)
  {
    settingDef->floatDef.Set(settingDef->floatDef.defVal);
  }
}

const ScriptClassDef* PluginBus::ComOverride(const char* comClassName) const
{
  if (comClassName == nullptr)
  {
    return nullptr;
  }
  const auto it = m_comOverrides.find(comClassName);
  return it == m_comOverrides.end() ? nullptr : it->second;
}

ScriptClassDef* PluginBus::ClassDef(const char* typeName) const
{
  if (typeName == nullptr)
  {
    return nullptr;
  }
  const auto it = m_scriptClasses.find(typeName);
  return it == m_scriptClasses.end() ? nullptr : it->second;
}

bool PluginBus::TakeLastScriptError(std::string* out)
{
  if (m_lastScriptError.empty())
  {
    return false;
  }
  if (out != nullptr)
  {
    *out = m_lastScriptError;
  }
  m_lastScriptError.clear();
  return true;
}

// ---- Static API trampolines ---------------------------------------------

void MSGPIAPI PluginBus::OnGetScriptApi(unsigned int, void*, void* msgData)
{
  if (msgData != nullptr)
  {
    *static_cast<ScriptablePluginAPI**>(msgData) = &s_scriptApi;
  }
}

void MSGPIAPI PluginBus::OnGetLoggingApi(unsigned int, void*, void* msgData)
{
  if (msgData != nullptr)
  {
    *static_cast<LoggingPluginAPI**>(msgData) = &s_loggingApi;
  }
}

void MSGPIAPI PluginBus::Log(const char* source, const char*, int, unsigned int level, const char* message)
{
  const char* levelName = "INFO";
  if (level >= LPI_LVL_ERROR)
  {
    levelName = "ERROR";
  }
  else if (level >= LPI_LVL_WARN)
  {
    levelName = "WARN";
  }
  else if (level <= LPI_LVL_DEBUG)
  {
    levelName = "DEBUG";
  }
  std::printf("[%s:%s] %s\n", source ? source : "plugin", levelName, message ? message : "");
}

void MSGPIAPI PluginBus::RegisterScriptClass(ScriptClassDef* classDef)
{
  if (s_instance == nullptr || classDef == nullptr || classDef->name.name == nullptr)
  {
    return;
  }
  s_instance->m_scriptClasses[classDef->name.name] = classDef;
}

void MSGPIAPI PluginBus::UnregisterScriptClass(ScriptClassDef* classDef)
{
  if (s_instance == nullptr || classDef == nullptr || classDef->name.name == nullptr)
  {
    return;
  }
  auto it = s_instance->m_scriptClasses.find(classDef->name.name);
  if (it != s_instance->m_scriptClasses.end() && it->second == classDef)
  {
    s_instance->m_scriptClasses.erase(it);
  }
  // A COM override naming this class is now dangling; drop it with the class.
  for (auto o = s_instance->m_comOverrides.begin(); o != s_instance->m_comOverrides.end();)
  {
    o = (o->second == classDef) ? s_instance->m_comOverrides.erase(o) : std::next(o);
  }
}

void MSGPIAPI PluginBus::OnScriptError(unsigned int type, const char* message)
{
  if (s_instance != nullptr)
  {
    s_instance->m_lastScriptError = message ? message : "";
  }
  std::printf("[plugin-script:%u] %s\n", type, message ? message : "");
}

void MSGPIAPI PluginBus::SetCOMObjectOverride(const char* classname, const ScriptClassDef* classDef)
{
  if (s_instance == nullptr || classname == nullptr || classname[0] == '\0')
  {
    return;
  }
  if (classDef == nullptr)
  {
    s_instance->m_comOverrides.erase(classname);
    return;
  }
  s_instance->m_comOverrides[classname] = classDef;
}

ScriptClassDef* MSGPIAPI PluginBus::GetClassDef(const char* typeName)
{
  return s_instance == nullptr ? nullptr : s_instance->ClassDef(typeName);
}
