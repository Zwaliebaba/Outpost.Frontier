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

/// One frame on the control stream: little-endian u16 length, then the payload.
/// A QUIC stream is a byte pipe, not a message queue, so the transport owns the
/// re-framing (ADR-003 §1).
constexpr std::size_t CONTROL_PREFIX_BYTES = 2;

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
    ConnectionState state = ConnectionState::Connecting;
    DisconnectReason endReason = DisconnectReason::ClosedByPeer;
    DisconnectReason closeReason = DisconnectReason::None; // Set by Close(); rides out once the stream has flushed.
    bool closeRequested = false;
    bool disconnectQueued = false;
    bool datagramSendEnabled = false;
    std::uint16_t maxDatagramBytes = 0;
    TransportStats stats;
    std::vector<std::uint8_t> assembly; // Control-frame reassembly; bounded by the frame cap below.
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
    // One bidirectional stream -- the control channel, opened by the client.
    settings.PeerBidiStreamCount = 1;
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
  [[nodiscard]] bool OnStreamBytes(Connection& _connection, const std::uint8_t* _data, std::uint32_t _size)
  {
    _connection.assembly.insert(_connection.assembly.end(), _data, _data + _size);
    _connection.stats.bytesReceived += _size;

    std::size_t at = 0;
    while (_connection.assembly.size() - at >= CONTROL_PREFIX_BYTES)
    {
      const std::size_t frameBytes =
        static_cast<std::size_t>(_connection.assembly[at]) | (static_cast<std::size_t>(_connection.assembly[at + 1]) << 8);
      if (frameBytes == 0 || frameBytes > MAX_DATAGRAM_BYTES)
      {
        return false;
      }
      if (_connection.assembly.size() - at < CONTROL_PREFIX_BYTES + frameBytes)
      {
        break;
      }
      QueueEvent(TransportEvent::Type::Message, _connection.id, TransportChannel::Control, DisconnectReason::None,
                 std::span<const std::uint8_t>{_connection.assembly.data() + at + CONTROL_PREFIX_BYTES, frameBytes});
      at += CONTROL_PREFIX_BYTES + frameBytes;
    }
    _connection.assembly.erase(_connection.assembly.begin(), _connection.assembly.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
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
        bool ok = true;
        for (std::uint32_t i = 0; i < _event->RECEIVE.BufferCount && ok; ++i)
        {
          ok = self->OnStreamBytes(*connection, _event->RECEIVE.Buffers[i].Buffer, _event->RECEIVE.Buffers[i].Length);
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
        if (connection->closeRequested)
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
      if (connection->stream != nullptr)
      {
        // One stream is the whole protocol, and the settings say so. Refusing
        // means returning failure without adopting the handle.
        return QUIC_STATUS_NOT_SUPPORTED;
      }
      connection->stream = _event->PEER_STREAM_STARTED.Stream;
      self->m_api->SetCallbackHandler(connection->stream, reinterpret_cast<void*>(StreamCallback), connection);
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

  const std::lock_guard<std::mutex> hold(impl.m_lock);

  // The RTT the HUD shows is msquic's own smoothed estimate, refreshed at the
  // poll cadence; loss on the reliable channel surfaces as controlResends,
  // exactly as the UDP implementation reported its stop-and-wait resends.
  for (auto& [id, connection] : impl.m_connections)
  {
    if (connection->state != ConnectionState::Connected || connection->connection == nullptr)
    {
      continue;
    }
    QUIC_STATISTICS_V2 statistics{};
    std::uint32_t size = sizeof(statistics);
    if (QUIC_SUCCEEDED(impl.m_api->GetParam(connection->connection, QUIC_PARAM_CONN_STATISTICS_V2, &size, &statistics)))
    {
      connection->stats.roundTripMs = static_cast<double>(statistics.Rtt) / 1000.0;
      connection->stats.minRoundTripMs = static_cast<double>(statistics.MinRtt) / 1000.0;
      connection->stats.controlResends = statistics.SendSuspectedLostPackets;
    }
  }

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
  if (impl.m_api == nullptr || _payload.size() > MAX_DATAGRAM_BYTES)
  {
    // The cap holds on both channels: nothing may learn to depend on the
    // reliable channel being roomier than a datagram (ADR-003 §1).
    return false;
  }

  const std::lock_guard<std::mutex> hold(impl.m_lock);
  Impl::Connection* connection = impl.Find(_connection);
  if (connection == nullptr || connection->state == ConnectionState::Closed || connection->state == ConnectionState::Draining)
  {
    return false;
  }

  if (_channel == TransportChannel::Control)
  {
    if (connection->stream == nullptr)
    {
      return false;
    }
    SendBuffer* send = MakeSendBuffer(_payload, true);
    // Non-blocking, and SEND_COMPLETE may run inline -- it takes no lock.
    const QUIC_STATUS status = impl.m_api->StreamSend(connection->stream, &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
    if (QUIC_FAILED(status))
    {
      delete send;
      return false;
    }
    connection->stats.bytesSent += send->buffer.Length;
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
  const QUIC_STATUS status = impl.m_api->DatagramSend(connection->connection, &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
  if (QUIC_FAILED(status))
  {
    delete send;
    ++connection->stats.datagramsDropped;
    return true;
  }
  ++connection->stats.datagramsSent;
  connection->stats.bytesSent += send->buffer.Length;
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
    if (streamHandle != nullptr)
    {
      connection->closeRequested = true;
      connection->closeReason = _reason;
    }
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
