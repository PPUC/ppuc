// A plugin that observes the bus and writes down what it saw.
//
// PPUC publishes a ControllerDef, sound commands and injected state events, and
// every claim about those is otherwise inferred from reading AltSound's and
// PUP's source rather than observed. Real plugins fail silently when a host
// gets this wrong: AltSound simply never activates, and nothing says why.
//
// This binds exactly the way AltSound does -- CtrlItemConsumer<ControllerDef>
// filtered on PMPI_GAMEID_PREFIX, PMPI_EVT_ON_AUDIO_CMD for sound, and
// "B2S"/"OnStateChange:1" for injected events -- and appends one line per
// observation to the file named by PPUC_FAKECTL_LOG (stdout if unset), so a
// test can assert on it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pinmame/PinMAMEPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/MsgPlugin.h"

using namespace PinballPlugin::Controller;

namespace
{

const MsgPluginAPI* g_api = nullptr;
uint32_t g_endpointId = 0;
unsigned int g_onAudioCmdId = 0;
unsigned int g_onStateChangeId = 0;
std::unique_ptr<CtrlItemConsumer<ControllerDef>> g_controllers;
std::mutex g_logMutex;

void Observe(const char* format, ...)
{
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  std::lock_guard<std::mutex> lock(g_logMutex);
  const char* path = std::getenv("PPUC_FAKECTL_LOG");
  if (path != nullptr && path[0] != '\0')
  {
    if (FILE* f = std::fopen(path, "a"))
    {
      std::fprintf(f, "%s\n", line);
      std::fclose(f);
      return;
    }
  }
  std::printf("[fakectl] %s\n", line);
  std::fflush(stdout);
}

// Mirrors AltSoundPlugin::SetupAltSound: take the first pinmame:: controller
// and strip the prefix to get the game id.
void OnControllersChanged()
{
  const ControllerDef controller = g_controllers->With(
      [](const std::vector<ControllerDef>& items)
      { return items.empty() ? ControllerDef {} : items.front(); });

  if (controller.gameId == nullptr || controller.endpointId == 0)
  {
    Observe("controller-gone");
    return;
  }
  const std::string prefix(PMPI_GAMEID_PREFIX);
  const std::string gameId = std::string(controller.gameId).substr(prefix.length());
  Observe("controller endpoint=%u gameId=%s game=%s", controller.endpointId, controller.gameId, gameId.c_str());
}

void MSGPIAPI OnAudioCmd(const unsigned int, void*, void* msgData)
{
  const auto* msg = static_cast<const PinMAMEChildBoardEventMsg*>(msgData);
  if (msg != nullptr)
  {
    Observe("audio-cmd board=%u cmd=%u", msg->boardNo, msg->cmd);
  }
}

// Same payload B2SPluginEventStream reads off this message.
struct B2SPluginEvent
{
  uint8_t type;
  int32_t index;
  int32_t value;
};

void MSGPIAPI OnStateChange(const unsigned int, void*, void* msgData)
{
  const auto* ev = static_cast<const B2SPluginEvent*>(msgData);
  if (ev != nullptr)
  {
    Observe("state-change type=%c index=%d value=%d", ev->type ? ev->type : '?', ev->index, ev->value);
  }
}

}  // namespace

MSGPI_EXPORT void MSGPIAPI FakeCtlPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
  g_api = api;
  g_endpointId = sessionId;
  Observe("loaded endpoint=%u", g_endpointId);

  g_controllers = std::make_unique<CtrlItemConsumer<ControllerDef>>(
      g_api, g_endpointId, CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
      [](std::vector<ControllerDef>& items)
      {
        const std::string prefix(PMPI_GAMEID_PREFIX);
        std::erase_if(items, [&prefix](const ControllerDef& c)
                      { return c.gameId == nullptr || !std::string(c.gameId).starts_with(prefix); });
      },
      []() {}, []() { OnControllersChanged(); });
  g_controllers->Subscribe();

  g_onAudioCmdId = g_api->GetMsgID(PMPI_NAMESPACE, PMPI_EVT_ON_AUDIO_CMD);
  g_api->SubscribeMsg(g_endpointId, g_onAudioCmdId, OnAudioCmd, nullptr);

  g_onStateChangeId = g_api->GetMsgID("B2S", "OnStateChange:1");
  g_api->SubscribeMsg(g_endpointId, g_onStateChangeId, OnStateChange, nullptr);
}

MSGPI_EXPORT void MSGPIAPI FakeCtlPluginUnload()
{
  Observe("unloaded");
  g_controllers->Unsubscribe();
  g_controllers.reset();
  g_api->UnsubscribeMsg(g_onStateChangeId, OnStateChange, nullptr);
  g_api->ReleaseMsgID(g_onStateChangeId);
  g_api->UnsubscribeMsg(g_onAudioCmdId, OnAudioCmd, nullptr);
  g_api->ReleaseMsgID(g_onAudioCmdId);
  g_api = nullptr;
}
