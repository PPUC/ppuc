// Drives the plugin bus without hardware and checks what a real plugin sees.
//
// ppuc-pinmame cannot reach OnGameStart without RS485 boards attached, so the
// three claims this migration rests on -- that PPUC publishes a ControllerDef
// plugins can bind to, that sound commands reach subscribers on
// PMPI_EVT_ON_AUDIO_CMD, and that injected state reaches "B2S"/"OnStateChange:1"
// -- were otherwise only inferred from reading AltSound's and PUP's source.
//
// This runs the host halves directly and asserts on what FakeCtl observed.
// Deliberately a separate binary from ppuc_tests, which must keep linking
// without SDL.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "AudioOutput.h"
#include "MediaPluginHost.h"
#include "PluginBus.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const std::string& what)
{
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok)
  {
    ++g_failures;
  }
}

std::string ReadAll(const std::string& path)
{
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s <plugin-dir>\n", argv[0]);
    return 2;
  }
  const std::string pluginDir = argv[1];

  const std::string logPath = (std::filesystem::temp_directory_path() / "ppuc_plugin_smoke.log").string();
  std::filesystem::remove(logPath);
  setenv("PPUC_FAKECTL_LOG", logPath.c_str(), 1);

  PluginBus bus;
  std::string error;
  if (!bus.Initialize(pluginDir, &error))
  {
    std::fprintf(stderr, "bus init failed: %s\n", error.c_str());
    return 2;
  }
  Check(bus.HostEndpointId() != 0, "host endpoint id is assigned, not assumed");

  if (!bus.LoadPluginById("FakeCtl"))
  {
    std::fprintf(stderr, "FakeCtl not found in %s\n", pluginDir.c_str());
    return 2;
  }

  AudioOutput audio;
  MediaPluginHost host(&audio, bus);

  // Every media feature off: this exercises the controller provider and the
  // event paths, not the backglass.
  MediaPluginHost::Options options;
  options.pluginDir = pluginDir.c_str();
  options.gameId = "emdemo";
  if (!host.Initialize(options, &error))
  {
    std::fprintf(stderr, "media host init failed: %s\n", error.c_str());
    return 2;
  }

  host.SetGameInfo("emdemo", 0);
  host.OnGameStart();
  host.OnSoundCommand(1, 42);
  host.QueueEvent('W', 207, 1);
  for (int i = 0; i < 4; ++i)
  {
    bus.Process();
    host.Process();
  }

  const std::string log = ReadAll(logPath);
  Check(log.find("loaded endpoint=") != std::string::npos, "FakeCtl loaded and reached the bus");
  Check(log.find("gameId=pinmame::emdemo") != std::string::npos,
        "a plugin binding like AltSound sees the controller, prefix included");
  Check(log.find("game=emdemo") != std::string::npos, "stripping the prefix yields the ROM name AltSound looks up");
  Check(log.find("audio-cmd board=1 cmd=42") != std::string::npos, "sound commands arrive on PMPI_EVT_ON_AUDIO_CMD");
  Check(log.find("state-change type=W index=207 value=1") != std::string::npos,
        "injected state arrives on B2S/OnStateChange:1");

  host.OnGameEnd();
  for (int i = 0; i < 2; ++i)
  {
    bus.Process();
    host.Process();
  }
  Check(ReadAll(logPath).find("controller-gone") != std::string::npos, "withdrawing the controller signals game end");

  host.Shutdown();
  bus.Shutdown();

  std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES PRESENT");
  return g_failures == 0 ? 0 : 1;
}
