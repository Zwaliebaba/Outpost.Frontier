#include "pch.h"

#include "QuicTransport.h"

#include "Log.h"

#include <msquic.h>

#include <ncrypt.h>
#include <wincrypt.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "msquic.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")

namespace Neuron
{
namespace
{

/// Application-layer protocol negotiation (ADR-003 §3). QUIC requires one, and
/// it doubles as a version gate: a build speaking a different protocol fails
/// the handshake at the door rather than mid-session.
constexpr const char* ALPN = "opf/1";

/// One frame on a reliable stream: little-endian u16 length, then the payload.
/// A QUIC stream is a byte pipe, not a message queue, so the transport owns the
/// re-framing (ADR-003 §1). Both reliable channels use it -- the framing is the
/// same, only the cap differs (ADR-022 §3c).
constexpr std::size_t CONTROL_PREFIX_BYTES = 2;

/*
 * Which client-initiated bidirectional stream is which.
 *
 * QUIC numbers client-initiated bidirectional streams 0, 4, 8..., in the order
 * the client starts them, so the id **is** the channel and no handshake is
 * needed to agree on it. Read off the handle rather than inferred from which
 * `PEER_STREAM_STARTED` arrived first: msquic makes no promise about that
 * ordering, and a protocol that silently swapped its control and bulk channels
 * under load would be a bug nobody could reproduce.
 */
constexpr std::uint64_t CONTROL_STREAM_ID = 0;
constexpr std::uint64_t BULK_STREAM_ID = 4;

/// The largest frame each reliable channel will read or write.
[[nodiscard]] constexpr std::size_t ReliableCapFor(TransportChannel _channel) noexcept
{
  return _channel == TransportChannel::Bulk ? MAX_BULK_BYTES : MAX_DATAGRAM_BYTES;
}

/*
 * A send in flight. msquic does not copy what it is given -- the bytes must
 * stay put until the completion callback says it is finished with them, which
 * is what the ClientContext pointer on every send is for.
 */
struct SendBuffer
{
  QUIC_BUFFER buffer{};
  std::vector<std::uint8_t> bytes;
};

[[nodiscard]] SendBuffer* MakeSendBuffer(std::span<const std::uint8_t> _payload, bool _lengthPrefixed)
{
  auto* send = new SendBuffer;
  send->bytes.reserve(_payload.size() + (_lengthPrefixed ? CONTROL_PREFIX_BYTES : 0));
  if (_lengthPrefixed)
  {
    send->bytes.push_back(static_cast<std::uint8_t>(_payload.size() & 0xff));
    send->bytes.push_back(static_cast<std::uint8_t>(_payload.size() >> 8));
  }
  send->bytes.insert(send->bytes.end(), _payload.begin(), _payload.end());
  send->buffer.Buffer = send->bytes.data();
  send->buffer.Length = static_cast<std::uint32_t>(send->bytes.size());
  return send;
}

/// A peer's application close code comes back as our own reason if it parses,
/// because Close() put a DisconnectReason there on the way out.
[[nodiscard]] DisconnectReason ReasonFromErrorCode(std::uint64_t _errorCode)
{
  return _errorCode <= static_cast<std::uint64_t>(DisconnectReason::ShuttingDown) ? static_cast<DisconnectReason>(_errorCode)
                                                                                  : DisconnectReason::ProtocolError;
}

[[nodiscard]] DisconnectReason ReasonFromTransportStatus(QUIC_STATUS _status)
{
  if (_status == QUIC_STATUS_CONNECTION_IDLE)
  {
    return DisconnectReason::TimedOut;
  }
  if (_status == QUIC_STATUS_CONNECTION_REFUSED || _status == QUIC_STATUS_UNREACHABLE)
  {
    return DisconnectReason::Refused;
  }
  return DisconnectReason::ProtocolError;
}

/*
 * The server's certificate, created in memory (ADR-003 §3). There is no CA and
 * no name to certify -- clients are told not to validate -- so this buys
 * encryption without authentication; pinning comes with real deployment.
 *
 * The private key is a persisted CNG key even so, because Schannel reaches the
 * key through the certificate's provider info and an ephemeral key has no
 * container to name. The container name carries the process id and a serial so
 * two listeners in one process cannot overwrite each other's key, and the key
 * is deleted with the certificate so a machine that has finished hosting is
 * not left carrying one.
 */
[[nodiscard]] PCCERT_CONTEXT CreateHostCertificate()
{
  static std::atomic<std::uint32_t> sm_serial{0}; // Thread-safe by type; only ever incremented.

  wchar_t keyName[64];
  swprintf_s(keyName, L"Outpost.Frontier Host Key %u-%u", static_cast<unsigned>(GetCurrentProcessId()), static_cast<unsigned>(++sm_serial));

  NCRYPT_PROV_HANDLE provider = 0;
  SECURITY_STATUS status = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
  if (status != ERROR_SUCCESS)
  {
    NEURON_LOG_ERROR("NCryptOpenStorageProvider failed (0x%08x)", static_cast<unsigned>(status));
    return nullptr;
  }

  NCRYPT_KEY_HANDLE key = 0;
  status = NCryptCreatePersistedKey(provider, &key, NCRYPT_RSA_ALGORITHM, keyName, 0, NCRYPT_OVERWRITE_KEY_FLAG);
  if (status == ERROR_SUCCESS)
  {
    DWORD bits = 2048;
    status = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0);
    if (status == ERROR_SUCCESS)
    {
      status = NCryptFinalizeKey(key, 0);
    }
    NCryptFreeObject(key);
  }
  NCryptFreeObject(provider);
  if (status != ERROR_SUCCESS)
  {
    NEURON_LOG_ERROR("creating the host key failed (0x%08x)", static_cast<unsigned>(status));
    return nullptr;
  }

  // The subject exists to be recognisable to a person, not to be validated.
  const char* subjectText = "CN=Outpost.Frontier Host";
  CERT_NAME_BLOB subject{};
  if (!CertStrToNameA(X509_ASN_ENCODING, subjectText, CERT_X500_NAME_STR, nullptr, nullptr, &subject.cbData, nullptr))
  {
    return nullptr;
  }
  std::vector<BYTE> subjectBytes(subject.cbData);
  subject.pbData = subjectBytes.data();
  if (!CertStrToNameA(X509_ASN_ENCODING, subjectText, CERT_X500_NAME_STR, nullptr, subject.pbData, &subject.cbData, nullptr))
  {
    return nullptr;
  }

  CRYPT_KEY_PROV_INFO providerInfo{};
  providerInfo.pwszContainerName = keyName;
  providerInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
  providerInfo.dwProvType = 0; // Zero says CNG, which is what NCryptCreatePersistedKey made.

  // SHA-256 rather than the SHA-1 default: TLS 1.3 will not sign with SHA-1.
  CRYPT_ALGORITHM_IDENTIFIER algorithm{};
  algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

  // Server-auth EKU, explicitly: Schannel picking a certificate with no usage
  // at all is a coin toss across Windows versions.
  LPSTR usageOid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
  CERT_ENHKEY_USAGE usage{1, &usageOid};
  CRYPT_DATA_BLOB encodedUsage{};
  if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &usage, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &encodedUsage.pbData,
                           &encodedUsage.cbData))
  {
    return nullptr;
  }
  CERT_EXTENSION extension{const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, encodedUsage};
  CERT_EXTENSIONS extensions{1, &extension};

  // Default validity (a year) -- the certificate lives for one hosting session
  // and is destroyed with the transport.
  const PCCERT_CONTEXT certificate =
    CertCreateSelfSignCertificate(0, &subject, 0, &providerInfo, &algorithm, nullptr, nullptr, &extensions);
  LocalFree(encodedUsage.pbData);
  if (certificate == nullptr)
  {
    NEURON_LOG_ERROR("CertCreateSelfSignCertificate failed (%u)", static_cast<unsigned>(GetLastError()));
  }
  return certificate;
}

void DestroyHostCertificate(PCCERT_CONTEXT _certificate)
{
  NCRYPT_KEY_HANDLE key = 0;
  DWORD keySpec = 0;
  BOOL callerMustFree = FALSE;
  if (CryptAcquireCertificatePrivateKey(_certificate, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &key,
                                        &keySpec, &callerMustFree))
  {
    // NCryptDeleteKey frees the handle whether it succeeds or not.
    if (callerMustFree)
    {
      NCryptDeleteKey(key, 0);
    }
    else
    {
      NCryptFreeObject(key);
    }
  }
  CertFreeCertificateContext(_certificate);
}

} // namespace

/*
 * Everything msquic lives here, behind the header's promise not to leak a
 * networking type. The locking rules, learned the hard way in the sibling
 * repository's integration and kept:
 *
 *   - never hold m_lock across a blocking msquic call (ConnectionClose,
 *     ListenerClose, RegistrationClose all wait for callbacks to drain);
 *   - the send-completion callbacks take no lock at all, because msquic may
 *     run them inline inside StreamSend on the sending thread.
 */
struct QuicTransport::Impl
{
  struct Connection
  {
    Impl* owner = nullptr;
    ConnectionId id = INVALID_CONNECTION;
    HQUIC connection = nullptr;
    HQUIC stream = nullptr;

    /// The second reliable ordered stream (ADR-022 §3c). Null until the peer
    /// starts it, which a build that predates the amendment never will --
    /// hence a `Bulk` send to a connection without one is refused rather than
    /// quietly rerouted onto `Control`, where it would do the head-of-line
    /// damage the channel exists to avoid.
    HQUIC bulkStream = nullptr;
    ConnectionState state = ConnectionState::Connecting;
    DisconnectReason endReason = DisconnectReason::ClosedByPeer;
    DisconnectReason closeReason = DisconnectReason::None; // Set by Close(); rides out once the stream has flushed.
    bool closeRequested = false;
    bool disconnectQueued = false;
    bool datagramSendEnabled = false;
    std::uint16_t maxDatagramBytes = 0;
    TransportStats stats;
    std::vector<std::uint8_t> assembly;     // Control-frame reassembly; bounded by the frame cap.
    std::vector<std::uint8_t> bulkAssembly; // The same, for `Bulk`. Separate, because the streams interleave.
  };

  struct QueuedEvent
  {
    TransportEvent::Type type = TransportEvent::Type::None;
    ConnectionId connection = INVALID_CONNECTION;
    TransportChannel channel = TransportChannel::Control;
    DisconnectReason reason = DisconnectReason::None;
    std::vector<std::uint8_t> payload;
  };

  const QUIC_API_TABLE* m_api = nullptr;
  HQUIC m_registration = nullptr;
  HQUIC m_configuration = nullptr;
  HQUIC m_listener = nullptr;
  PCCERT_CONTEXT m_certificate = nullptr;
  std::uint16_t m_boundPort = 0;

  mutable std::mutex m_lock;
  std::unordered_map<ConnectionId, std::unique_ptr<Connection>> m_connections;
  ConnectionId m_nextConnectionId = 1;

  /// Callbacks append here under the lock; Poll() moves everything across to
  /// m_events, so delivery happens only on the owning thread (ADR-007).
  std::deque<QueuedEvent> m_arrived;
  std::deque<QueuedEvent> m_events;           // Owning thread only.
  std::vector<std::uint8_t> m_currentPayload; // Backs the span handed to the caller.

  ~Impl()
  {
    Teardown();
  }

  [[nodiscard]] bool EnsureApi()
  {
    if (m_api != nullptr)
    {
      return true;
    }
    QUIC_STATUS status = MsQuicOpen2(&m_api);
    if (QUIC_FAILED(status))
    {
      NEURON_LOG_ERROR("MsQuicOpen2 failed (0x%08x)", static_cast<unsigned>(status));
      m_api = nullptr;
      return false;
    }
    // LOW_LATENCY rather than the default: this carries orders, where a late
    // packet is worse than a small one.
    const QUIC_REGISTRATION_CONFIG registrationConfig{"OutpostFrontier", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    status = m_api->RegistrationOpen(&registrationConfig, &m_registration);
    if (QUIC_FAILED(status))
    {
      NEURON_LOG_ERROR("RegistrationOpen failed (0x%08x)", static_cast<unsigned>(status));
      MsQuicClose(m_api);
      m_api = nullptr;
      m_registration = nullptr;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool OpenConfiguration(bool _asServer)
  {
    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = IDLE_TIMEOUT_MS;
    settings.IsSet.IdleTimeoutMs = TRUE;
    // Two bidirectional streams, both opened by the client: `Control` (stream 0)
    // and `Bulk` (stream 4). The second is ADR-022 §3c's amendment to ADR-003 §1
    // -- a keyframe is a baseline rather than fresh state, so it must not queue
    // behind the player's orders.
    settings.PeerBidiStreamCount = 2;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;

    QUIC_BUFFER alpn;
    alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(ALPN));
    alpn.Length = static_cast<uint32_t>(std::strlen(ALPN));

    QUIC_STATUS status = m_api->ConfigurationOpen(m_registration, &alpn, 1, &settings, sizeof(settings), nullptr, &m_configuration);
    if (QUIC_FAILED(status))
    {
      NEURON_LOG_ERROR("ConfigurationOpen failed (0x%08x)", static_cast<unsigned>(status));
      m_configuration = nullptr;
      return false;
    }

    QUIC_CREDENTIAL_CONFIG credential{};
    if (_asServer)
    {
      m_certificate = CreateHostCertificate();
      if (m_certificate == nullptr)
      {
        m_api->ConfigurationClose(m_configuration);
        m_configuration = nullptr;
        return false;
      }
      credential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
      credential.CertificateContext = reinterpret_cast<QUIC_CERTIFICATE*>(const_cast<CERT_CONTEXT*>(m_certificate));
    }
    else
    {
      // Self-signed and no name to check it against: validating would fail
      // every time. Encryption without authentication, per ADR-003 §3.
      credential.Type = QUIC_CREDENTIAL_TYPE_NONE;
      credential.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    status = m_api->ConfigurationLoadCredential(m_configuration, &credential);
    if (QUIC_FAILED(status))
    {
      NEURON_LOG_ERROR("ConfigurationLoadCredential failed (0x%08x)", static_cast<unsigned>(status));
      m_api->ConfigurationClose(m_configuration);
      m_configuration = nullptr;
      if (m_certificate != nullptr)
      {
        DestroyHostCertificate(m_certificate);
        m_certificate = nullptr;
      }
      return false;
    }
    return true;
  }

  /// Called with m_lock held.
  void QueueEvent(TransportEvent::Type _type, ConnectionId _connection, TransportChannel _channel, DisconnectReason _reason,
                  std::span<const std::uint8_t> _payload)
  {
    QueuedEvent event;
    event.type = _type;
    event.connection = _connection;
    event.channel = _channel;
    event.reason = _reason;
    event.payload.assign(_payload.begin(), _payload.end());
    m_arrived.push_back(std::move(event));
  }

  [[nodiscard]] Connection* Find(ConnectionId _id)
  {
    const auto entry = m_connections.find(_id);
    return entry == m_connections.end() ? nullptr : entry->second.get();
  }

  /*
   * Control bytes off the wire, called with m_lock held. Returns false when the
   * peer framed something this protocol cannot read -- a zero or over-cap
   * length -- and the caller then drops the connection rather than guessing.
   */
  [[nodiscard]] bool OnStreamBytes(Connection& _connection, TransportChannel _channel, const std::uint8_t* _data,
                                   std::uint32_t _size)
  {
    // Each reliable stream reassembles into its own buffer. They interleave on
    // the wire and a shared buffer would splice one channel's frame into the
    // other's, which the length prefix would then read as garbage.
    std::vector<std::uint8_t>& assembly = _channel == TransportChannel::Bulk ? _connection.bulkAssembly : _connection.assembly;
    const std::size_t cap = ReliableCapFor(_channel);

    assembly.insert(assembly.end(), _data, _data + _size);
    _connection.stats.bytesReceived += _size;

    std::size_t at = 0;
    while (assembly.size() - at >= CONTROL_PREFIX_BYTES)
    {
      const std::size_t frameBytes = static_cast<std::size_t>(assembly[at]) | (static_cast<std::size_t>(assembly[at + 1]) << 8);
      if (frameBytes == 0 || frameBytes > cap)
      {
        return false;
      }
      if (assembly.size() - at < CONTROL_PREFIX_BYTES + frameBytes)
      {
        break;
      }
      QueueEvent(TransportEvent::Type::Message, _connection.id, _channel, DisconnectReason::None,
                 std::span<const std::uint8_t>{assembly.data() + at + CONTROL_PREFIX_BYTES, frameBytes});
      at += CONTROL_PREFIX_BYTES + frameBytes;
    }
    assembly.erase(assembly.begin(), assembly.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
  }

  /// Which channel a stream handle is. Called with m_lock held.
  [[nodiscard]] static TransportChannel ChannelOf(const Connection& _connection, HQUIC _stream) noexcept
  {
    return _stream == _connection.bulkStream ? TransportChannel::Bulk : TransportChannel::Control;
  }

  static QUIC_STATUS QUIC_API StreamCallback(HQUIC _stream, void* _context, QUIC_STREAM_EVENT* _event)
  {
    auto* connection = static_cast<Connection*>(_context);
    Impl* self = connection->owner;

    switch (_event->Type)
    {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      // No lock: msquic can raise this inline from StreamSend, and taking
      // m_lock here would be taking it twice on the same thread.
      delete static_cast<SendBuffer*>(_event->SEND_COMPLETE.ClientContext);
      break;

    case QUIC_STREAM_EVENT_RECEIVE:
    {
      HQUIC toDrop = nullptr;
      {
        const std::lock_guard<std::mutex> hold(self->m_lock);
        const TransportChannel channel = Impl::ChannelOf(*connection, _stream);
        bool ok = true;
        for (std::uint32_t i = 0; i < _event->RECEIVE.BufferCount && ok; ++i)
        {
          ok = self->OnStreamBytes(*connection, channel, _event->RECEIVE.Buffers[i].Buffer, _event->RECEIVE.Buffers[i].Length);
        }
        if (!ok)
        {
          toDrop = connection->connection;
        }
      }
      if (toDrop != nullptr)
      {
        // Non-blocking, so callable from the connection's own callback thread;
        // the close itself waits for SHUTDOWN_COMPLETE and the owning thread.
        self->m_api->ConnectionShutdown(toDrop, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                        static_cast<QUIC_UINT62>(DisconnectReason::ProtocolError));
      }
      break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    {
      // An abort is abnormal -- the peer tore the stream out from under the
      // protocol -- so the connection goes with it. A graceful FIN
      // (PEER_SEND_SHUTDOWN) is different: it is how Close() flushes its last
      // messages, and the peer's connection close follows it on the wire, so
      // nothing is initiated here for one -- the idle timeout backstops a peer
      // that sent a FIN and then vanished.
      HQUIC toDrop = nullptr;
      {
        const std::lock_guard<std::mutex> hold(self->m_lock);
        toDrop = connection->connection;
      }
      if (toDrop != nullptr)
      {
        self->m_api->ConnectionShutdown(toDrop, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                        static_cast<QUIC_UINT62>(DisconnectReason::ClosedByPeer));
      }
      break;
    }

    case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
    {
      // Everything Close() had queued -- a refusal, a goodbye -- has now been
      // delivered and acknowledged, so the connection close it deferred can go
      // out without discarding any of it.
      HQUIC toClose = nullptr;
      DisconnectReason reason = DisconnectReason::None;
      {
        const std::lock_guard<std::mutex> hold(self->m_lock);
        // The control stream is what the deferred close waits on, and only it:
        // `Bulk` shuts down alongside, and letting either trigger the close
        // would race two shutdowns against one connection.
        if (connection->closeRequested && _stream == connection->stream)
        {
          connection->closeRequested = false;
          toClose = connection->connection;
          reason = connection->closeReason;
        }
      }
      if (toClose != nullptr)
      {
        self->m_api->ConnectionShutdown(toClose, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, static_cast<QUIC_UINT62>(reason));
      }
      break;
    }

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
    {
      {
        const std::lock_guard<std::mutex> hold(self->m_lock);
        if (connection->stream == _stream)
        {
          connection->stream = nullptr;
        }
        else if (connection->bulkStream == _stream)
        {
          connection->bulkStream = nullptr;
        }
      }
      self->m_api->StreamClose(_stream);
      break;
    }

    default:
      break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC, void* _context, QUIC_CONNECTION_EVENT* _event)
  {
    auto* connection = static_cast<Connection*>(_context);
    Impl* self = connection->owner;

    switch (_event->Type)
    {
    case QUIC_CONNECTION_EVENT_CONNECTED:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      if (connection->state == ConnectionState::Connecting)
      {
        connection->state = ConnectionState::Connected;
        self->QueueEvent(TransportEvent::Type::Connected, connection->id, TransportChannel::Control, DisconnectReason::None, {});
      }
      break;
    }

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      HQUIC opened = _event->PEER_STREAM_STARTED.Stream;

      /*
       * Which channel this is, from the stream's own id (ADR-022 §3c).
       *
       * Not "the first one is control": msquic promises nothing about the order
       * `PEER_STREAM_STARTED` fires in, and a protocol whose channels could
       * swap under load would be a bug nobody could reproduce. A stream whose
       * id is neither is refused rather than adopted -- two streams is the
       * whole protocol, and the settings say so.
       */
      std::uint64_t streamId = 0;
      std::uint32_t idBytes = sizeof(streamId);
      if (QUIC_FAILED(self->m_api->GetParam(opened, QUIC_PARAM_STREAM_ID, &idBytes, &streamId)))
      {
        return QUIC_STATUS_NOT_SUPPORTED;
      }

      HQUIC* slot = nullptr;
      if (streamId == CONTROL_STREAM_ID)
      {
        slot = &connection->stream;
      }
      else if (streamId == BULK_STREAM_ID)
      {
        slot = &connection->bulkStream;
      }
      if (slot == nullptr || *slot != nullptr)
      {
        return QUIC_STATUS_NOT_SUPPORTED;
      }

      *slot = opened;
      self->m_api->SetCallbackHandler(opened, reinterpret_cast<void*>(StreamCallback), connection);
      break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      connection->datagramSendEnabled = _event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
      connection->maxDatagramBytes = _event->DATAGRAM_STATE_CHANGED.MaxSendLength;
      break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      const QUIC_BUFFER* buffer = _event->DATAGRAM_RECEIVED.Buffer;
      if (buffer->Length > MAX_DATAGRAM_BYTES)
      {
        ++connection->stats.datagramsDropped; // Bigger than the contract allows: counted rather than hidden.
        break;
      }
      ++connection->stats.datagramsReceived;
      connection->stats.bytesReceived += buffer->Length;
      self->QueueEvent(TransportEvent::Type::Message, connection->id, TransportChannel::State, DisconnectReason::None,
                       std::span<const std::uint8_t>{buffer->Buffer, buffer->Length});
      break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      // No lock, as with SEND_COMPLETE. The first sent-or-terminal state is the
      // one that frees: msquic will not touch the buffer again, and ACKNOWLEDGED
      // may never arrive for a datagram nobody acknowledges. Clearing the
      // context stops a later state for the same send freeing it twice.
      switch (_event->DATAGRAM_SEND_STATE_CHANGED.State)
      {
      case QUIC_DATAGRAM_SEND_SENT:
      case QUIC_DATAGRAM_SEND_LOST_DISCARDED:
      case QUIC_DATAGRAM_SEND_ACKNOWLEDGED:
      case QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS:
      case QUIC_DATAGRAM_SEND_CANCELED:
        delete static_cast<SendBuffer*>(_event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
        _event->DATAGRAM_SEND_STATE_CHANGED.ClientContext = nullptr;
        break;
      default:
        break;
      }
      break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      if (connection->state != ConnectionState::Closed)
      {
        connection->state = ConnectionState::Draining;
        connection->endReason = ReasonFromErrorCode(_event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
      }
      break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      if (connection->state != ConnectionState::Closed)
      {
        connection->state = ConnectionState::Draining;
        connection->endReason = ReasonFromTransportStatus(_event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
      }
      break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    {
      // The handle stays open -- Teardown() owns every ConnectionClose, so
      // there is exactly one place a handle dies and no race to close it twice.
      const std::lock_guard<std::mutex> hold(self->m_lock);
      if (!connection->disconnectQueued)
      {
        connection->disconnectQueued = true;
        self->QueueEvent(TransportEvent::Type::Disconnected, connection->id, TransportChannel::Control, connection->endReason, {});
      }
      connection->state = ConnectionState::Closed;
      break;
    }

    default:
      break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  static QUIC_STATUS QUIC_API ListenerCallback(HQUIC, void* _context, QUIC_LISTENER_EVENT* _event)
  {
    Impl* self = static_cast<Impl*>(_context);
    if (_event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
    {
      return QUIC_STATUS_SUCCESS;
    }

    Connection* connection = nullptr;
    {
      const std::lock_guard<std::mutex> hold(self->m_lock);
      auto record = std::make_unique<Connection>();
      record->owner = self;
      record->id = self->m_nextConnectionId++;
      record->connection = _event->NEW_CONNECTION.Connection;
      connection = record.get();
      self->m_connections.emplace(record->id, std::move(record));
    }

    self->m_api->SetCallbackHandler(connection->connection, reinterpret_cast<void*>(ConnectionCallback), connection);
    const QUIC_STATUS status = self->m_api->ConnectionSetConfiguration(connection->connection, self->m_configuration);
    if (QUIC_FAILED(status))
    {
      // Returning failure hands the handle back to msquic to dispose of, so it
      // is not this file's to close and the record must not keep it.
      const std::lock_guard<std::mutex> hold(self->m_lock);
      self->m_connections.erase(connection->id);
      return status;
    }
    return QUIC_STATUS_SUCCESS;
  }

  /*
   * Closes everything this instance owns, in dependency order, holding the
   * lock across none of it -- every close blocks until the object's callbacks
   * have drained, and those callbacks take the lock.
   */
  void Teardown()
  {
    if (m_api == nullptr)
    {
      return;
    }

    if (m_listener != nullptr)
    {
      m_api->ListenerClose(m_listener);
      m_listener = nullptr;
    }

    std::vector<HQUIC> handles;
    {
      const std::lock_guard<std::mutex> hold(m_lock);
      for (auto& [id, connection] : m_connections)
      {
        if (connection->connection != nullptr)
        {
          handles.push_back(connection->connection);
        }
      }
    }
    for (HQUIC handle : handles)
    {
      m_api->ConnectionShutdown(handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, static_cast<QUIC_UINT62>(DisconnectReason::ShuttingDown));
      m_api->ConnectionClose(handle); // Blocks until the callbacks are done; streams close from their own callbacks.
    }

    {
      const std::lock_guard<std::mutex> hold(m_lock);
      m_connections.clear();
      m_arrived.clear();
    }
    m_events.clear();

    if (m_configuration != nullptr)
    {
      m_api->ConfigurationClose(m_configuration);
      m_configuration = nullptr;
    }
    if (m_certificate != nullptr)
    {
      DestroyHostCertificate(m_certificate);
      m_certificate = nullptr;
    }
    if (m_registration != nullptr)
    {
      m_api->RegistrationClose(m_registration);
      m_registration = nullptr;
    }
    MsQuicClose(m_api);
    m_api = nullptr;
    m_boundPort = 0;
  }
};

QuicTransport::QuicTransport()
  : m_impl(std::make_unique<Impl>())
{
}

QuicTransport::~QuicTransport()
{
  Shutdown();
}

bool QuicTransport::Listen(std::uint16_t _port)
{
  Impl& impl = *m_impl;
  if (!impl.EnsureApi() || !impl.OpenConfiguration(true))
  {
    return false;
  }

  QUIC_STATUS status = impl.m_api->ListenerOpen(impl.m_registration, Impl::ListenerCallback, &impl, &impl.m_listener);
  if (QUIC_FAILED(status))
  {
    NEURON_LOG_ERROR("ListenerOpen failed (0x%08x)", static_cast<unsigned>(status));
    impl.m_listener = nullptr;
    return false;
  }

  // Loopback only, by the same constraint the UDP implementation bound under:
  // the MVP is one machine, and nothing may depend on that quietly widening.
  QUIC_ADDR address{};
  if (!QuicAddrFromString("127.0.0.1", _port, &address))
  {
    return false;
  }

  QUIC_BUFFER alpn;
  alpn.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(ALPN));
  alpn.Length = static_cast<uint32_t>(std::strlen(ALPN));

  status = impl.m_api->ListenerStart(impl.m_listener, &alpn, 1, &address);
  if (QUIC_FAILED(status))
  {
    NEURON_LOG_ERROR("ListenerStart failed on port %u (0x%08x)", static_cast<unsigned>(_port), static_cast<unsigned>(status));
    return false;
  }

  // Port 0 asked the OS to choose; this is what it chose.
  QUIC_ADDR bound{};
  std::uint32_t boundSize = sizeof(bound);
  if (QUIC_SUCCEEDED(impl.m_api->GetParam(impl.m_listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &boundSize, &bound)))
  {
    impl.m_boundPort = QuicAddrGetPort(&bound);
  }

  NEURON_LOG_INFO("quic transport listening on 127.0.0.1:%u (alpn %s)", static_cast<unsigned>(impl.m_boundPort), ALPN);
  return true;
}

ConnectionId QuicTransport::Connect(const std::string& _host, std::uint16_t _port)
{
  Impl& impl = *m_impl;
  if (!impl.EnsureApi() || !impl.OpenConfiguration(false))
  {
    return INVALID_CONNECTION;
  }

  Impl::Connection* connection = nullptr;
  {
    const std::lock_guard<std::mutex> hold(impl.m_lock);
    auto record = std::make_unique<Impl::Connection>();
    record->owner = &impl;
    record->id = impl.m_nextConnectionId++;
    connection = record.get();
    impl.m_connections.emplace(record->id, std::move(record));
  }

  QUIC_STATUS status = impl.m_api->ConnectionOpen(impl.m_registration, Impl::ConnectionCallback, connection, &connection->connection);
  if (QUIC_SUCCEEDED(status))
  {
    // The control stream, opened before the handshake so a Send() the moment
    // Connect() returns is queued rather than refused -- which is the UDP
    // behaviour every caller was written against.
    status =
      impl.m_api->StreamOpen(connection->connection, QUIC_STREAM_OPEN_FLAG_NONE, Impl::StreamCallback, connection, &connection->stream);
    if (QUIC_SUCCEEDED(status))
    {
      status = impl.m_api->StreamStart(connection->stream, QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL);
    }
    // And the bulk stream, immediately after, so it takes id 4 (ADR-022 §3c).
    // Started here rather than lazily on the first keyframe: opening a stream
    // mid-session would put a round trip in front of the one message on this
    // channel that a client is waiting on to render anything at all.
    if (QUIC_SUCCEEDED(status))
    {
      status = impl.m_api->StreamOpen(connection->connection, QUIC_STREAM_OPEN_FLAG_NONE, Impl::StreamCallback, connection,
                                      &connection->bulkStream);
    }
    if (QUIC_SUCCEEDED(status))
    {
      /*
       * **`IMMEDIATE`, and it is load-bearing rather than an optimisation.**
       *
       * QUIC does not put a stream on the wire until something is written to
       * it, so a stream that is opened and started and then stays quiet is a
       * stream the peer has never heard of. On this channel the **server**
       * speaks first -- the keyframe is server to client -- so without this
       * flag the server would have no `bulkStream` to send on and every
       * keyframe would be silently refused, which is a client that joins and
       * then watches nothing at all.
       *
       * The control stream does not need it because the client's `Hello` is the
       * first thing on the connection, and that write announces the stream.
       */
      status = impl.m_api->StreamStart(connection->bulkStream,
                                       QUIC_STREAM_START_FLAG_IMMEDIATE | QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL);
    }
  }
  if (QUIC_SUCCEEDED(status))
  {
    status = impl.m_api->ConnectionStart(connection->connection, impl.m_configuration, QUIC_ADDRESS_FAMILY_UNSPEC, _host.c_str(), _port);
  }

  if (QUIC_FAILED(status))
  {
    NEURON_LOG_ERROR("quic connect to %s:%u failed (0x%08x)", _host.c_str(), static_cast<unsigned>(_port), static_cast<unsigned>(status));
    HQUIC handle = nullptr;
    {
      const std::lock_guard<std::mutex> hold(impl.m_lock);
      connection->disconnectQueued = true; // The connection never existed for the caller; no event for its death.
      connection->state = ConnectionState::Closed;
      handle = connection->connection;
    }
    if (handle != nullptr)
    {
      impl.m_api->ConnectionClose(handle); // Streams close from their own callbacks during this.
    }
    const std::lock_guard<std::mutex> hold(impl.m_lock);
    impl.m_connections.erase(connection->id);
    return INVALID_CONNECTION;
  }

  NEURON_LOG_INFO("quic transport connecting to %s:%u", _host.c_str(), static_cast<unsigned>(_port));
  return connection->id;
}

void QuicTransport::Poll()
{
  Impl& impl = *m_impl;
  if (impl.m_api == nullptr)
  {
    return;
  }

  // The RTT the HUD shows is msquic's own smoothed estimate, refreshed at the
  // poll cadence; loss on the reliable channel surfaces as controlResends,
  // exactly as the UDP implementation reported its stop-and-wait resends.
  //
  // GetParam on a connection handle is one of the blocking calls the locking
  // rules above name: msquic queues the request to the connection's worker and
  // waits for it. Holding m_lock across it deadlocks against any callback
  // waiting for that same lock on that same worker -- so the handles are
  // copied out, asked outside the lock, and the answers written back.
  std::vector<std::pair<ConnectionId, HQUIC>> polled;
  {
    const std::lock_guard<std::mutex> hold(impl.m_lock);
    polled.reserve(impl.m_connections.size());
    for (auto& [id, connection] : impl.m_connections)
    {
      if (connection->state == ConnectionState::Connected && connection->connection != nullptr)
      {
        polled.emplace_back(id, connection->connection);
      }
    }
  }

  // Safe outside the lock: ConnectionClose only ever runs on this thread
  // (Teardown and the Connect failure path), so a handle collected above
  // cannot be closed underneath this loop.
  for (const auto& [id, handle] : polled)
  {
    QUIC_STATISTICS_V2 statistics{};
    std::uint32_t size = sizeof(statistics);
    if (!QUIC_SUCCEEDED(impl.m_api->GetParam(handle, QUIC_PARAM_CONN_STATISTICS_V2, &size, &statistics)))
    {
      continue;
    }

    const std::lock_guard<std::mutex> hold(impl.m_lock);
    // Re-found rather than remembered: a callback may have erased the record
    // while the lock was down.
    Impl::Connection* connection = impl.Find(id);
    if (connection == nullptr)
    {
      continue;
    }
    connection->stats.roundTripMs = static_cast<double>(statistics.Rtt) / 1000.0;
    connection->stats.minRoundTripMs = static_cast<double>(statistics.MinRtt) / 1000.0;
    connection->stats.controlResends = statistics.SendSuspectedLostPackets;
  }

  const std::lock_guard<std::mutex> hold(impl.m_lock);
  while (!impl.m_arrived.empty())
  {
    impl.m_events.push_back(std::move(impl.m_arrived.front()));
    impl.m_arrived.pop_front();
  }
}

bool QuicTransport::NextEvent(TransportEvent& _outEvent)
{
  Impl& impl = *m_impl;
  if (impl.m_events.empty())
  {
    return false;
  }

  Impl::QueuedEvent event = std::move(impl.m_events.front());
  impl.m_events.pop_front();

  // The payload span must stay valid until the next call, so it points at a
  // buffer the transport owns rather than at the event that just died.
  impl.m_currentPayload = std::move(event.payload);

  _outEvent.type = event.type;
  _outEvent.connection = event.connection;
  _outEvent.channel = event.channel;
  _outEvent.reason = event.reason;
  _outEvent.payload = std::span<const std::uint8_t>{impl.m_currentPayload};
  return true;
}

bool QuicTransport::Send(ConnectionId _connection, TransportChannel _channel, std::span<const std::uint8_t> _payload)
{
  Impl& impl = *m_impl;
  /*
   * The cap is the channel's, and two of the three share the datagram's.
   *
   * `Control` and `State` both hold at `MAX_DATAGRAM_BYTES` for ADR-003 §1's
   * reason: nothing may learn to depend on the reliable channel being roomier
   * than a datagram, or the day it has to fit one it will not. `Bulk` is the
   * exception the amendment bought -- a keyframe is not a datagram-shaped
   * object -- and it is a *separate* channel precisely so the exception cannot
   * leak into the messages that must stay small.
   */
  const std::size_t cap = _channel == TransportChannel::Bulk ? MAX_BULK_BYTES : MAX_DATAGRAM_BYTES;
  if (impl.m_api == nullptr || _payload.size() > cap)
  {
    return false;
  }

  const std::lock_guard<std::mutex> hold(impl.m_lock);
  Impl::Connection* connection = impl.Find(_connection);
  if (connection == nullptr || connection->state == ConnectionState::Closed || connection->state == ConnectionState::Draining)
  {
    return false;
  }

  if (_channel == TransportChannel::Control || _channel == TransportChannel::Bulk)
  {
    HQUIC stream = _channel == TransportChannel::Bulk ? connection->bulkStream : connection->stream;
    if (stream == nullptr)
    {
      // A peer that never started this stream cannot be sent on it. Refused
      // rather than rerouted onto `Control`: that would do exactly the
      // head-of-line damage the second channel exists to avoid, and it would
      // do it silently.
      return false;
    }
    SendBuffer* send = MakeSendBuffer(_payload, true);
    // Read before the send, never after: SEND_COMPLETE may run inline and it
    // deletes the buffer, so `send` can already be freed when StreamSend
    // returns.
    const std::uint64_t sentBytes = send->buffer.Length;
    // Non-blocking, and SEND_COMPLETE may run inline -- it takes no lock.
    const QUIC_STATUS status = impl.m_api->StreamSend(stream, &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
    if (QUIC_FAILED(status))
    {
      delete send;
      return false;
    }
    connection->stats.bytesSent += sentBytes;
    return true;
  }

  // Unreliable by design: a datagram that cannot go right now is replaced by
  // the next one, and the drop is counted rather than hidden.
  if (!connection->datagramSendEnabled || _payload.size() > connection->maxDatagramBytes)
  {
    ++connection->stats.datagramsDropped;
    return true;
  }
  SendBuffer* send = MakeSendBuffer(_payload, false);
  // As above: DATAGRAM_SEND_STATE_CHANGED can free this inline.
  const std::uint64_t sentBytes = send->buffer.Length;
  const QUIC_STATUS status = impl.m_api->DatagramSend(connection->connection, &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
  if (QUIC_FAILED(status))
  {
    delete send;
    ++connection->stats.datagramsDropped;
    return true;
  }
  ++connection->stats.datagramsSent;
  connection->stats.bytesSent += sentBytes;
  return true;
}

void QuicTransport::Close(ConnectionId _connection, DisconnectReason _reason)
{
  Impl& impl = *m_impl;
  if (impl.m_api == nullptr)
  {
    return;
  }

  HQUIC connectionHandle = nullptr;
  HQUIC streamHandle = nullptr;
  HQUIC bulkHandle = nullptr;
  {
    const std::lock_guard<std::mutex> hold(impl.m_lock);
    Impl::Connection* connection = impl.Find(_connection);
    if (connection == nullptr || connection->state == ConnectionState::Closed)
    {
      return;
    }
    connection->state = ConnectionState::Closed;
    if (!connection->disconnectQueued)
    {
      connection->disconnectQueued = true;
      impl.QueueEvent(TransportEvent::Type::Disconnected, _connection, TransportChannel::Control, _reason, {});
    }
    connectionHandle = connection->connection;
    streamHandle = connection->stream;
    bulkHandle = connection->bulkStream;
    if (streamHandle != nullptr)
    {
      connection->closeRequested = true;
      connection->closeReason = _reason;
    }
  }

  // `Bulk` flushes alongside, and the deferred close still waits on `Control`
  // alone: a keyframe in flight is worth finishing, and two streams racing to
  // issue one connection close would be two shutdowns for one connection.
  if (bulkHandle != nullptr)
  {
    impl.m_api->StreamShutdown(bulkHandle, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
  }

  /*
   * ConnectionShutdown abandons whatever is still queued on the streams --
   * which would include the refusal or goodbye the caller just sent, and a
   * refusal that never leaves is a client that never learns why. So when a
   * stream exists it is shut down gracefully first (pending data, then FIN),
   * and SEND_SHUTDOWN_COMPLETE issues the deferred connection close with the
   * reason once everything has actually been delivered.
   */
  if (streamHandle != nullptr)
  {
    impl.m_api->StreamShutdown(streamHandle, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
  }
  else if (connectionHandle != nullptr)
  {
    // Non-blocking; the reason rides the wire as the application close code,
    // so the peer can tell a deliberate close from a crash.
    impl.m_api->ConnectionShutdown(connectionHandle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, static_cast<QUIC_UINT62>(_reason));
  }
}

ConnectionState QuicTransport::State(ConnectionId _connection) const
{
  const Impl& impl = *m_impl;
  const std::lock_guard<std::mutex> hold(impl.m_lock);
  const auto entry = impl.m_connections.find(_connection);
  return entry == impl.m_connections.end() ? ConnectionState::Closed : entry->second->state;
}

TransportStats QuicTransport::Stats(ConnectionId _connection) const
{
  const Impl& impl = *m_impl;
  const std::lock_guard<std::mutex> hold(impl.m_lock);
  const auto entry = impl.m_connections.find(_connection);
  return entry == impl.m_connections.end() ? TransportStats{} : entry->second->stats;
}

std::uint16_t QuicTransport::BoundPort() const
{
  return m_impl->m_boundPort;
}

void QuicTransport::Shutdown()
{
  m_impl->Teardown();
}

} // namespace Neuron
