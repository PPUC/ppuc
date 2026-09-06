#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugins/MsgPluginManager.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/ScriptablePlugin.h"

// The VPX pinball plugin bus, and the parts of it that are not media-specific.
//
// PPUC hosts VPX plugins by compiling VPX's own MsgPluginManager into this
// binary and registering itself as a statically linked plugin. That machinery
// used to live inside MediaPluginHost, which made it reachable only when PUP,
// AltSound or B2S was enabled. It is about to have a second client: the
// plugin-based game engine in docs/PLUGIN_MIGRATION.md needs the message API,
// the scriptable-class registry (to instantiate VPinMAME.Controller) and a
// settings override for PinMAME's ROM path, none of which are media concerns.
//
// Owned by main(), constructed BEFORE and destroyed AFTER every client: at
// plugin unload the ScriptClassDefs and the code behind every plugin-provided
// function pointer go away together.
//
// NOT thread safe. Everything here runs on the plugin API thread, which for
// PPUC is the main loop.
class PluginBus final
{
 public:
  PluginBus();
  ~PluginBus();

  PluginBus(const PluginBus&) = delete;
  PluginBus& operator=(const PluginBus&) = delete;

  // Registers PPUC as a plugin, installs the settings handler and the script
  // API, then scans `pluginDir` for plugins. Returns false and fills
  // `errorMessage` when the directory is missing.
  bool Initialize(const std::string& pluginDir, std::string* errorMessage);

  // Unloads every plugin, host last. Safe to call twice, and when Initialize
  // failed or was never called.
  void Shutdown();

  // Loads a discovered plugin by its plugin.cfg id. Logs and returns false if
  // no such plugin was found.
  bool LoadPluginById(const std::string& id);

  const MsgPluginAPI& Api() const;

  // PPUC's own endpoint id, as assigned by RegisterPlugin.
  //
  // Captured rather than assumed. MsgPluginManager hands out
  // `m_plugins.size() + 1`, so PPUC only gets 1 because it registers before the
  // plugin folder is scanned. Hardcoding that made a future reordering silently
  // wrong; every subscription and broadcast now uses this.
  uint32_t HostEndpointId() const { return m_hostEndpointId; }

  // Drains RunOnMainThread queues. Must be called from the main loop -- every
  // audio buffer a plugin produces is marshalled through here.
  void Process();

  // ---- Settings ---------------------------------------------------------
  // Plugins declare their settings and the host answers. A single handler
  // serves all of them, which is why the overrides live here rather than in
  // any one client: the media host injects PUP/AltSound folders, and the
  // engine will inject PinMAME's ROM path. Anything not overridden takes the
  // plugin's own default.
  void SetSettingOverride(const std::string& pluginId, const std::string& propId, const std::string& value);

  // ---- Scriptable classes -----------------------------------------------
  // Some plugins are reachable only through a COM-style override rather than a
  // bus message: B2S registers "B2S.Server", PinMAME registers
  // "VPinMAME.Controller". Both clients need to look those up, so the registry
  // belongs to the bus.
  const ScriptClassDef* ComOverride(const char* comClassName) const;
  ScriptClassDef* ClassDef(const char* typeName) const;

  // The last error a plugin reported through the script API, cleared by the
  // read. Callers use it to explain why a member call failed.
  bool TakeLastScriptError(std::string* out);

 private:
  void ConfigureSetting(const std::string& pluginId, MsgPI::MsgPluginManager::SettingAction action,
                        MsgSettingDef* settingDef);

  static void MSGPIAPI OnGetScriptApi(unsigned int msgId, void* context, void* msgData);
  static void MSGPIAPI OnGetLoggingApi(unsigned int msgId, void* context, void* msgData);

  static void MSGPIAPI Log(const char* source, const char* func, int line, unsigned int level,
                           const char* message);
  static void MSGPIAPI RegisterScriptClass(ScriptClassDef* classDef);
  static void MSGPIAPI RegisterScriptTypeAlias(const char*, const char*) {}
  static void MSGPIAPI RegisterScriptArrayType(ScriptArrayDef*) {}
  static void MSGPIAPI SubmitTypeLibrary(unsigned int) {}
  static void MSGPIAPI UnregisterScriptClass(ScriptClassDef* classDef);
  static void MSGPIAPI UnregisterScriptTypeAlias(const char*) {}
  static void MSGPIAPI UnregisterScriptArrayType(ScriptArrayDef*) {}
  static void MSGPIAPI OnScriptError(unsigned int type, const char* message);
  static void MSGPIAPI SetCOMObjectOverride(const char* classname, const ScriptClassDef* classDef);
  static ScriptClassDef* MSGPIAPI GetClassDef(const char* typeName);

  MsgPI::MsgPluginManager m_manager;
  std::shared_ptr<MsgPI::MsgPlugin> m_hostPlugin;
  std::vector<std::shared_ptr<MsgPI::MsgPlugin>> m_loadedPlugins;
  uint32_t m_hostEndpointId = 0;
  bool m_initialized = false;

  unsigned int m_getScriptApiId = 0;
  unsigned int m_getLoggingApiId = 0;

  std::map<std::pair<std::string, std::string>, std::string> m_settingOverrides;
  std::unordered_map<std::string, ScriptClassDef*> m_scriptClasses;
  std::unordered_map<std::string, const ScriptClassDef*> m_comOverrides;
  std::string m_lastScriptError;

  // The plugin APIs are C structs of function pointers with no context
  // argument, so they need a single instance to reach. PluginBus is created
  // once by main(), which makes that safe; the constructor asserts it.
  static PluginBus* s_instance;
  static ScriptablePluginAPI s_scriptApi;
  static LoggingPluginAPI s_loggingApi;
};
