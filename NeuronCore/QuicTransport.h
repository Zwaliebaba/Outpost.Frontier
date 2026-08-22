#pragma once

#include "Transport.h"

#include <memory>

/*
 * The product transport (ADR-003 §3): msquic behind the same seam the loopback
 * UDP implementation validated, and since S13 the only implementation -- owner
 * directive, recorded in the build order: QUIC only, no transport knob.
 *
 * The mapping is the one the interface was shaped for. The control channel is
 * stream 0, opened by the client, with a two-byte little-endian length prefix
 * turning the byte pipe back into messages; the state channel is QUIC DATAGRAM
 * frames, which keep message boundaries themselves. **The bulk channel is the
 * client's second bidirectional stream** (id 4), same framing, a wider cap, and
 * a separate reassembly buffer -- ADR-022 §3c's amendment to ADR-003 §1, so a
 * keyframe does not park in front of the player's orders. The two are told
 * apart by their QUIC stream id rather than by which `PEER_STREAM_STARTED`
 * arrived first, because arrival order is not a thing this protocol should
 * depend on. ALPN is `opf/1`; the
 * server presents an in-memory self-signed certificate
 * (`CertCreateSelfSignCertificate` + `QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT`
 * -- the Schannel-flavour package cannot load PEM), and the client is told not
 * to validate it, which on loopback buys encryption without authentication.
 * Pinning comes with real deployment, out of MVP.
 *
 * Threading (ADR-007): msquic runs its own workers and calls back on them.
 * Callbacks append to an internal queue under a lock and touch nothing else;
 * events surface only when the owning thread calls Poll(). The send-completion
 * callbacks take no lock at all, because msquic may run them inline inside
 * StreamSend on the sending thread.
 *
 * Everything msquic lives behind the Impl so this header stays free of
 * networking headers -- the seam (Transport.h) promises that, and every test
 * that includes this header holds it to the promise.
 */

namespace Neuron
{

class QuicTransport final : public Transport
{
public:
  QuicTransport();
  ~QuicTransport() override;

  QuicTransport(const QuicTransport&) = delete;
  QuicTransport& operator=(const QuicTransport&) = delete;

  [[nodiscard]] bool Listen(std::uint16_t _port) override;
  [[nodiscard]] ConnectionId Connect(const std::string& _host, std::uint16_t _port) override;
  void Poll() override;
  [[nodiscard]] bool NextEvent(TransportEvent& _outEvent) override;
  [[nodiscard]] bool Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload) override;
  void Close(ConnectionId _connection, DisconnectReason _reason) override;
  void Shutdown() override;
  [[nodiscard]] ConnectionState State(ConnectionId _connection) const override;
  [[nodiscard]] TransportStats Stats(ConnectionId _connection) const override;
  [[nodiscard]] std::uint16_t BoundPort() const override;

  /// A connection with no traffic at all for this long is gone. The same figure
  /// the UDP implementation used, so nothing above the seam re-learns patience.
  static constexpr std::uint32_t IDLE_TIMEOUT_MS = 10000;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace Neuron
