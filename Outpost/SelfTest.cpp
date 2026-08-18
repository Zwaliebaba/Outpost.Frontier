#include "pch.h"

#include "SelfTest.h"

#include "ClientConnection.h"

#include "ServerConfig.h"
#include "ServerHost.h"
#include "Simulation.h"

#include "Clock.h"
#include "Log.h"

#include <cstdint>

namespace Outpost
{
namespace
{

/// Generous: the point is to catch "never happened", not to measure. A healthy
/// loopback finishes in well under a second.
constexpr double TimeoutMs = 10000.0;

/// One pong proves the path; three prove the cadence keeps running.
constexpr std::uint64_t RequiredPongs = 3;

/// A round trip over loopback is measured in microseconds. A whole second means
/// the number is being computed wrong, not that the link is slow.
constexpr double ImplausibleRoundTripMs = 1000.0;

class Checklist
{
public:
  void Record(const char* _name, bool _passed)
  {
    if (_passed)
    {
      NEURON_LOG_INFO("self test: %s -- ok", _name);
    }
    else
    {
      NEURON_LOG_ERROR("self test: %s -- FAILED", _name);
      ++m_failures;
    }
  }

  [[nodiscard]] std::uint32_t Failures() const noexcept { return m_failures; }

private:
  std::uint32_t m_failures = 0;
};

/// Services the client until the predicate holds or the deadline passes. The
/// server has its own thread, so only this end needs pumping.
template <typename Predicate>
bool PumpUntil(Neuron::ClientConnection& _client, Predicate _predicate)
{
  const std::int64_t start = Neuron::Clock::Counter();
  while (Neuron::Clock::MillisecondsBetween(start, Neuron::Clock::Counter()) < TimeoutMs)
  {
    _client.Poll();
    if (_predicate())
    {
      return true;
    }
    Sleep(1);
  }
  return false;
}

} // namespace

int RunSelfTest(const AppConfig& _config, Neuron::Simulation& _simulation)
{
  NEURON_LOG_INFO("self test: starting (milestone M0: handshake and heartbeat over loopback)");

  Checklist checks;

  // Port 0 whatever the config says: a self test must not fail because the
  // configured port is already taken by the thing it is testing.
  Neuron::ServerConfig serverConfig;
  serverConfig.port = 0;
  serverConfig.transport = _config.server.transport;
  serverConfig.maxSessions = _config.server.maxSessions;

  Neuron::ServerHost server;
  const bool started = server.Start(serverConfig, _simulation);
  checks.Record("server starts", started);
  if (!started)
  {
    return 3; // Nothing after this can mean anything.
  }
  checks.Record("server reports its bound port", server.BoundPort() != 0);

  {
    Neuron::ClientConnection client;
    const bool connected = client.Connect("127.0.0.1", server.BoundPort(), _simulation.SchemaHash(), _simulation.ContentHash(),
                                          "self test");
    checks.Record("client opens a socket", connected);

    if (connected)
    {
      const bool joined = PumpUntil(client, [&] { return client.State() != Neuron::ClientLinkState::Connecting; });
      checks.Record("handshake completes", joined && client.State() == Neuron::ClientLinkState::Joined);
      checks.Record("server issues a client id", client.ClientId() != 0);
      checks.Record("server reports 20 Hz", client.ServerTickRate() == 20);
      checks.Record("server counts the session", server.SessionCount() == 1);

      const bool ponged = PumpUntil(client, [&] { return client.PongCount() >= RequiredPongs; });
      checks.Record("heartbeat crosses the loopback", ponged);
      checks.Record("round trip is plausible", client.RoundTripMs() >= 0.0 && client.RoundTripMs() < ImplausibleRoundTripMs);
      checks.Record("server advances its tick", client.ServerTick() > 0);

      const Neuron::TransportStats stats = client.Stats();
      checks.Record("no datagram was dropped", stats.datagramsDropped == 0);

      NEURON_LOG_INFO("self test: %llu pings, %llu pongs, rtt %.3f ms, %llu resends, server at tick %u",
                      static_cast<unsigned long long>(client.PingCount()), static_cast<unsigned long long>(client.PongCount()),
                      client.RoundTripMs(), static_cast<unsigned long long>(stats.controlResends), client.ServerTick());
    }

    // Leaving the scope disconnects, which is itself part of what is checked:
    // the server must survive it rather than shutting down (ADR-008 §1).
  }

  // The goodbye has to reach the sim thread and be acted on, so this waits for
  // the session table to empty rather than assuming it already has.
  bool emptied = false;
  const std::int64_t waitStart = Neuron::Clock::Counter();
  while (Neuron::Clock::MillisecondsBetween(waitStart, Neuron::Clock::Counter()) < TimeoutMs)
  {
    if (server.SessionCount() == 0)
    {
      emptied = true;
      break;
    }
    Sleep(1);
  }
  checks.Record("server outlives the client", emptied && server.Running());

  server.Stop();
  server.Join();
  checks.Record("server stops cleanly", !server.Running() && server.TickCount() > 0);
  checks.Record("no tick overran", server.OverrunCount() == 0);

  if (checks.Failures() == 0)
  {
    NEURON_LOG_INFO("self test: PASSED");
    return 0;
  }
  NEURON_LOG_ERROR("self test: FAILED (%u checks)", checks.Failures());
  return 3;
}

} // namespace Outpost
