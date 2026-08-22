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

  /*
   * How many bytes of entity records one viewer's tick may cost
   * ([ADR-022](../Design/ADR/ADR-022-interest-and-delta.md) §5b).
   *
   * A deployment number and not a balancing constant, which is why it is here
   * and the tick rate is not: how much bandwidth a shard is willing to spend
   * per viewer is a property of where it runs, while how fast it ticks is a
   * property of the game. Zero means "use the engine's default", so a config
   * that has never heard of interest management gets a sane one.
   *
   * It is a **bandwidth** figure, not a datagram size: the 1,152-byte datagram
   * cap never moves, and what this decides is how many of them a tick may use.
   */
  std::uint32_t tickBudgetBytes = 0;
};

} // namespace Neuron
