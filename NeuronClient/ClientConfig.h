#pragma once

#include <cstdint>
#include <string>

/*
 * What the client needs to start, as a plain struct assembled by the
 * composition root (ADR-008 §3). NeuronClient never reads a file, an
 * environment variable or a command line.
 */

namespace Neuron
{

struct ClientConfig
{
  std::uint32_t windowWidth = 1600;
  std::uint32_t windowHeight = 900;
  std::string windowTitle = "Outpost: Frontier";
  bool borderlessFullscreen = false;

  bool vsync = true;
  std::uint32_t frameCap = 0; // 0 = uncapped; vsync usually makes this moot.
  bool enableDebugLayer = false;

  std::string serverHost = "127.0.0.1";
  std::uint16_t serverPort = 7777;
};

} // namespace Neuron
