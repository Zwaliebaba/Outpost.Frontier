#include "pch.h"

#include "UdpTransport.h"

#include "Clock.h"
#include "Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace Neuron
{
namespace
{

/// Winsock is refcounted per process; the transport may be created more than
/// once (a host runs a listener and a client in the same process).
class WinsockScope
{
public:
  static bool Acquire()
  {
    if (sm_references == 0)
    {
      WSADATA data{};
      const int result = WSAStartup(MAKEWORD(2, 2), &data);
      if (result != 0)
      {
        NEURON_LOG_ERROR("WSAStartup failed (%d)", result);
        return false;
      }
    }
    ++sm_references;
    return true;
  }

  static void Release()
  {
    if (sm_references > 0 && --sm_references == 0)
    {
      WSACleanup();
    }
  }

private:
  static int sm_references;
};

int WinsockScope::sm_references = 0;

constexpr std::uint8_t FlagAckOnly = 0x80;

/// Sequence numbers wrap, so "newer" is a distance question rather than a
/// greater-than one.
[[nodiscard]] bool IsNewer(std::uint16_t _sequence, std::uint16_t _previous) noexcept
{
  return static_cast<std::uint16_t>(_sequence - _previous) < 0x8000u && _sequence != _previous;
}

} // namespace

UdpTransport::~UdpTransport()
{
  Shutdown();
}

bool UdpTransport::OpenSocket(std::uint16_t _port)
{
  if (!WinsockScope::Acquire())
  {
    return false;
  }

  const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == INVALID_SOCKET)
  {
    NEURON_LOG_ERROR("socket() failed (%d)", WSAGetLastError());
    WinsockScope::Release();
    return false;
  }

  u_long nonBlocking = 1;
  if (ioctlsocket(handle, FIONBIO, &nonBlocking) == SOCKET_ERROR)
  {
    NEURON_LOG_ERROR("ioctlsocket(FIONBIO) failed (%d)", WSAGetLastError());
    closesocket(handle);
    WinsockScope::Release();
    return false;
  }

  // Loopback drops when the receive buffer fills, and the sim thread only
  // drains once a tick, so give it room for a burst.
  int receiveBuffer = 1 << 20;
  setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));

  // Windows raises WSAECONNRESET on a datagram socket when a previous send was
  // refused by a now-absent peer. Left on, that turns one dead client into a
  // recvfrom failure that looks like the socket itself broke.
  BOOL reportUnreachable = FALSE;
  DWORD returned = 0;
  WSAIoctl(handle, SIO_UDP_CONNRESET, &reportUnreachable, sizeof(reportUnreachable), nullptr, 0, &returned, nullptr, nullptr);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // MVP is loopback only, by constraint.
  address.sin_port = htons(_port);
  if (bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
  {
    NEURON_LOG_ERROR("bind(port %u) failed (%d)", static_cast<unsigned>(_port), WSAGetLastError());
    closesocket(handle);
    WinsockScope::Release();
    return false;
  }

  sockaddr_in bound{};
  int boundSize = sizeof(bound);
  if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0)
  {
    m_boundPort = ntohs(bound.sin_port); // Port 0 asked the OS to choose; this is what it chose.
  }

  m_socket = static_cast<std::uintptr_t>(handle);
  return true;
}

bool UdpTransport::Listen(std::uint16_t _port)
{
  if (!OpenSocket(_port))
  {
    return false;
  }
  m_listening = true;
  NEURON_LOG_INFO("udp transport listening on 127.0.0.1:%u", static_cast<unsigned>(m_boundPort));
  return true;
}

ConnectionId UdpTransport::Connect(const std::string& _host, std::uint16_t _port)
{
  if (!OpenSocket(0)) // Ephemeral local port; the peer is addressed per send.
  {
    return InvalidConnection;
  }

  in_addr resolved{};
  if (inet_pton(AF_INET, _host.c_str(), &resolved) != 1)
  {
    NEURON_LOG_ERROR("'%s' is not an IPv4 address", _host.c_str());
    return InvalidConnection;
  }

  Endpoint endpoint;
  endpoint.address = resolved.s_addr;
  endpoint.port = htons(_port);

  Connection& connection = AddConnection(endpoint, ConnectionState::Connecting);
  connection.lastReceivedCounter = Clock::Counter();
  NEURON_LOG_INFO("udp transport connecting to %s:%u from local port %u", _host.c_str(), static_cast<unsigned>(_port),
                  static_cast<unsigned>(m_boundPort));
  return connection.id;
}

UdpTransport::Connection& UdpTransport::AddConnection(const Endpoint& _endpoint, ConnectionState _state)
{
  const ConnectionId id = m_nextConnectionId++;
  Connection connection;
  connection.id = id;
  connection.endpoint = _endpoint;
  connection.state = _state;
  connection.lastReceivedCounter = Clock::Counter();
  return m_connections.emplace(id, std::move(connection)).first->second;
}

UdpTransport::Connection* UdpTransport::Find(ConnectionId _id)
{
  const auto entry = m_connections.find(_id);
  return entry == m_connections.end() ? nullptr : &entry->second;
}

const UdpTransport::Connection* UdpTransport::Find(ConnectionId _id) const
{
  const auto entry = m_connections.find(_id);
  return entry == m_connections.end() ? nullptr : &entry->second;
}

UdpTransport::Connection* UdpTransport::FindByEndpoint(const Endpoint& _endpoint)
{
  for (auto& [id, connection] : m_connections)
  {
    if (connection.endpoint == _endpoint)
    {
      return &connection;
    }
  }
  return nullptr;
}

void UdpTransport::SendRaw(Connection& _connection, TransportChannel _channel, std::uint16_t _sequence, std::uint16_t _ack,
                           std::span<const std::uint8_t> _payload)
{
  if (m_socket == ~static_cast<std::uintptr_t>(0) || _payload.size() > MaxPayloadBytes)
  {
    ++_connection.stats.datagramsDropped;
    return;
  }

  std::uint8_t datagram[MaxDatagramBytes];
  datagram[0] = static_cast<std::uint8_t>(_channel);
  if (_payload.empty() && _channel == TransportChannel::Control && _sequence == 0)
  {
    datagram[0] |= FlagAckOnly; // A bare acknowledgement carries nothing else.
  }
  datagram[1] = static_cast<std::uint8_t>(_sequence & 0xff);
  datagram[2] = static_cast<std::uint8_t>(_sequence >> 8);
  datagram[3] = static_cast<std::uint8_t>(_ack & 0xff);
  datagram[4] = static_cast<std::uint8_t>(_ack >> 8);
  datagram[5] = static_cast<std::uint8_t>(_payload.size() & 0xff);
  datagram[6] = static_cast<std::uint8_t>(_payload.size() >> 8);
  if (!_payload.empty())
  {
    std::memcpy(datagram + HeaderBytes, _payload.data(), _payload.size());
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = _connection.endpoint.address;
  address.sin_port = _connection.endpoint.port;

  const int total = static_cast<int>(HeaderBytes + _payload.size());
  const int sent = sendto(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(datagram), total, 0,
                          reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  if (sent == SOCKET_ERROR)
  {
    ++_connection.stats.datagramsDropped;
    return;
  }
  ++_connection.stats.datagramsSent;
  _connection.stats.bytesSent += static_cast<std::uint64_t>(sent);
  _connection.lastAckSent = _ack;
}

bool UdpTransport::Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload)
{
  Connection* connection = Find(_connection);
  if (connection == nullptr || connection->state == ConnectionState::Closed || _payload.size() > MaxPayloadBytes)
  {
    return false;
  }

  if (_channel == TransportChannel::State)
  {
    // Unreliable by design: a lost snapshot is replaced by the next one, and
    // retransmitting stale state would be worse than dropping it.
    SendRaw(*connection, TransportChannel::State, 0, connection->lastReceivedSequence, _payload);
    return true;
  }

  PendingControl pending;
  pending.payload.assign(_payload.begin(), _payload.end());
  pending.sequence = connection->nextSequence++;
  if (connection->nextSequence == 0)
  {
    connection->nextSequence = 1; // Zero means "no sequence" in the header.
  }
  connection->controlQueue.push_back(std::move(pending));

  // Stop and wait: only the front message is ever in flight.
  if (connection->controlQueue.size() == 1)
  {
    PendingControl& front = connection->controlQueue.front();
    SendRaw(*connection, TransportChannel::Control, front.sequence, connection->lastReceivedSequence, front.payload);
    front.sent = true;
    front.lastSentCounter = Clock::Counter();
  }
  return true;
}

void UdpTransport::QueueEvent(TransportEvent::Type _type, ConnectionId _connection, TransportChannel _channel,
                              DisconnectReason _reason, std::span<const std::uint8_t> _payload)
{
  QueuedEvent event;
  event.type = _type;
  event.connection = _connection;
  event.channel = _channel;
  event.reason = _reason;
  event.payload.assign(_payload.begin(), _payload.end());
  m_events.push_back(std::move(event));
}

void UdpTransport::ReceiveAll()
{
  if (m_socket == ~static_cast<std::uintptr_t>(0))
  {
    return;
  }

  for (;;)
  {
    std::uint8_t datagram[MaxDatagramBytes];
    sockaddr_in from{};
    int fromSize = sizeof(from);
    const int received = recvfrom(static_cast<SOCKET>(m_socket), reinterpret_cast<char*>(datagram), static_cast<int>(sizeof(datagram)),
                                  0, reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (received == SOCKET_ERROR)
    {
      const int error = WSAGetLastError();
      if (error != WSAEWOULDBLOCK && error != WSAECONNRESET)
      {
        NEURON_LOG_WARNING("recvfrom failed (%d)", error);
      }
      return; // Drained, or a failure that says nothing about the next datagram.
    }
    if (received < static_cast<int>(HeaderBytes))
    {
      continue; // Too short to be ours.
    }

    Endpoint endpoint;
    endpoint.address = from.sin_addr.s_addr;
    endpoint.port = from.sin_port;

    Connection* connection = FindByEndpoint(endpoint);
    if (connection == nullptr)
    {
      if (!m_listening)
      {
        continue; // A client accepts traffic only from the peer it dialled.
      }
      connection = &AddConnection(endpoint, ConnectionState::Connected);
      QueueEvent(TransportEvent::Type::Connected, connection->id, TransportChannel::Control, DisconnectReason::None, {});
    }

    const auto channel = static_cast<TransportChannel>(datagram[0] & 0x0f);
    const bool ackOnly = (datagram[0] & FlagAckOnly) != 0;
    const auto sequence = static_cast<std::uint16_t>(datagram[1] | (datagram[2] << 8));
    const auto ack = static_cast<std::uint16_t>(datagram[3] | (datagram[4] << 8));
    const auto length = static_cast<std::uint16_t>(datagram[5] | (datagram[6] << 8));

    if (static_cast<std::size_t>(received) != HeaderBytes + length)
    {
      ++connection->stats.datagramsDropped; // Header disagrees with the datagram.
      continue;
    }

    ++connection->stats.datagramsReceived;
    connection->stats.bytesReceived += static_cast<std::uint64_t>(received);
    connection->lastReceivedCounter = Clock::Counter();

    if (connection->state == ConnectionState::Connecting)
    {
      connection->state = ConnectionState::Connected;
      QueueEvent(TransportEvent::Type::Connected, connection->id, TransportChannel::Control, DisconnectReason::None, {});
    }

    // Their acknowledgement retires our in-flight control message and releases
    // the next one.
    if (!connection->controlQueue.empty() && connection->controlQueue.front().sequence == ack)
    {
      const std::int64_t sentAt = connection->controlQueue.front().lastSentCounter;
      connection->stats.roundTripMs = Clock::MillisecondsBetween(sentAt, Clock::Counter());
      connection->controlQueue.pop_front();
      if (!connection->controlQueue.empty())
      {
        PendingControl& next = connection->controlQueue.front();
        SendRaw(*connection, TransportChannel::Control, next.sequence, connection->lastReceivedSequence, next.payload);
        next.sent = true;
        next.lastSentCounter = Clock::Counter();
      }
    }

    if (ackOnly || length == 0)
    {
      continue;
    }

    const std::span<const std::uint8_t> payload{datagram + HeaderBytes, length};

    if (channel == TransportChannel::Control)
    {
      if (!IsNewer(sequence, connection->lastReceivedSequence))
      {
        // A duplicate means our acknowledgement was lost, not that they are
        // confused: acknowledge again and drop the copy.
        SendRaw(*connection, TransportChannel::Control, 0, connection->lastReceivedSequence, {});
        continue;
      }
      connection->lastReceivedSequence = sequence;
      SendRaw(*connection, TransportChannel::Control, 0, sequence, {});
    }

    QueueEvent(TransportEvent::Type::Message, connection->id, channel, DisconnectReason::None, payload);
  }
}

void UdpTransport::ServiceResends()
{
  const std::int64_t now = Clock::Counter();
  for (auto& [id, connection] : m_connections)
  {
    if (connection.controlQueue.empty())
    {
      continue;
    }
    PendingControl& front = connection.controlQueue.front();
    if (!front.sent || Clock::MillisecondsBetween(front.lastSentCounter, now) < ResendIntervalMs)
    {
      continue;
    }
    SendRaw(connection, TransportChannel::Control, front.sequence, connection.lastReceivedSequence, front.payload);
    front.lastSentCounter = now;
    ++connection.stats.controlResends;
  }
}

void UdpTransport::ServiceTimeouts()
{
  const std::int64_t now = Clock::Counter();
  for (auto& [id, connection] : m_connections)
  {
    if (connection.state == ConnectionState::Closed)
    {
      continue;
    }
    if (Clock::MillisecondsBetween(connection.lastReceivedCounter, now) > TimeoutMs)
    {
      connection.state = ConnectionState::Closed;
      QueueEvent(TransportEvent::Type::Disconnected, connection.id, TransportChannel::Control, DisconnectReason::TimedOut, {});
    }
  }
}

void UdpTransport::Poll()
{
  ReceiveAll();
  ServiceResends();
  ServiceTimeouts();
}

bool UdpTransport::NextEvent(TransportEvent& _outEvent)
{
  if (m_events.empty())
  {
    return false;
  }

  QueuedEvent event = std::move(m_events.front());
  m_events.pop_front();

  // The payload span must stay valid until the next call, so it points at a
  // buffer the transport owns rather than at the event that just died.
  m_currentPayload = std::move(event.payload);

  _outEvent.type = event.type;
  _outEvent.connection = event.connection;
  _outEvent.channel = event.channel;
  _outEvent.reason = event.reason;
  _outEvent.payload = std::span<const std::uint8_t>{m_currentPayload};
  return true;
}

void UdpTransport::Close(ConnectionId _connection, DisconnectReason _reason)
{
  Connection* connection = Find(_connection);
  if (connection == nullptr || connection->state == ConnectionState::Closed)
  {
    return;
  }
  connection->state = ConnectionState::Closed;
  connection->controlQueue.clear();
  QueueEvent(TransportEvent::Type::Disconnected, _connection, TransportChannel::Control, _reason, {});
}

ConnectionState UdpTransport::State(ConnectionId _connection) const
{
  const Connection* connection = Find(_connection);
  return connection == nullptr ? ConnectionState::Closed : connection->state;
}

TransportStats UdpTransport::Stats(ConnectionId _connection) const
{
  const Connection* connection = Find(_connection);
  return connection == nullptr ? TransportStats{} : connection->stats;
}

void UdpTransport::Shutdown()
{
  if (m_socket != ~static_cast<std::uintptr_t>(0))
  {
    closesocket(static_cast<SOCKET>(m_socket));
    m_socket = ~static_cast<std::uintptr_t>(0);
    WinsockScope::Release();
  }
  m_connections.clear();
  m_events.clear();
  m_listening = false;
  m_boundPort = 0;
}

} // namespace Neuron
