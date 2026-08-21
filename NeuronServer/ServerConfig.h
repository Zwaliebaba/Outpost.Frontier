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

class DurableStore;

struct ServerConfig
{
  std::uint16_t port = 7777; // 0 binds an ephemeral port; ask the host which it got.
  std::uint32_t maxSessions = 8;
  std::string serverName = "Outpost";

  /*
   * Where the shard writes itself down, or null for a shard that persists
   * nothing (ADR-025 §7).
   *
   * Borrowed, not owned: the composition root constructs the store, opens it
   * and loads whatever was there **before** the host starts, because a shard
   * that began ticking and then discovered it had a past would have to undo the
   * ticks. The host's part is the cadence -- a snapshot every
   * `SNAPSHOT_INTERVAL_SECONDS` and one on the way out -- which is the part
   * that has to happen on the thread that owns the state.
   */
  DurableStore* durableStore = nullptr;
};

} // namespace Neuron
