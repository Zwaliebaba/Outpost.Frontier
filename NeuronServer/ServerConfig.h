#pragma once

#include <cstdint>
#include <string>

/*
 * What the server needs to start, as a plain struct from the composition root
 * (ADR-008 §3). Note what is absent: the tick rate. That is a balancing
 * constant owned by ADR-002, not a deployment knob, and a server that ticks at
 * a rate the client does not expect is a bug rather than a configuration.
 */

namespace Neuron
{

struct ServerConfig
{
  std::uint16_t port = 7777; // 0 binds an ephemeral port; ask the host which it got.
  std::string transport = "udp";
  std::uint32_t maxSessions = 8;
  std::string serverName = "Outpost";
};

} // namespace Neuron
