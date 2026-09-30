#include "castcore/cast_channel.h"
#include "castcore/device_auth.h"
#include "castcore/logger.h"
#include "castcore/net_platform.h"
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>

#include <cstring>
#include <chrono>
#include <csignal>
#include <algorithm>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
#endif

namespace castcore {

std::string RedactSecrets(const std::string& json) {
  // The OFFER carries the per-session AES key and IV mask in clear text; the
  // session log must never hold them. Replace the string value after each key.
  static constexpr const char* kSecretKeys[] = {"aesKey", "aesIvMask"};
  std::string out = json;
  for (const char* key : kSecretKeys) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = out.find(needle, pos)) != std::string::npos) {
      size_t colon = out.find(':', pos + needle.size());
      size_t open = colon == std::string::npos ? colon : out.find('"', colon + 1);
      size_t close = open == std::string::npos ? open : out.find('"', open + 1);
      if (close == std::string::npos) break;
      out.replace(open + 1, close - open - 1, "<redacted>");
      pos = open + 1;
    }
  }
  return out;
}

namespace {

bool IsHeartbeatPingPong(const std::string& namespace_, const std::string& payload) {
  if (namespace_ != kNamespaceHeartbeat) {
    return false;
  }
  return payload.find("PING") != std::string::npos || payload.find("PONG") != std::string::npos;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

CastChannel::CastChannel() = default;

CastChannel::~CastChannel() {
  Disconnect();
}

bool CastChannel::IsConnected() const {
  return is_connected_.load();
}

void CastChannel::SetMessageCallback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  message_callback_ = std::move(callback);
}

void CastChannel::SetStatusCallback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  status_callback_ = std::move(callback);
}

bool CastChannel::Connect(const std::string& ip_address, uint16_t port, int timeout_ms) {
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#else
  if (!EnsureSocketInit()) {
    LOG_ERROR << "Socket subsystem initialization failed";
    return false;
  }
#endif
  if (is_connected_.load()) {
    Disconnect();
  }

  ip_address_ = ip_address;
  port_ = port;
  should_stop_ = false;

  LOG_INFO << "Connecting TLS Cast Channel to " << ip_address << ":" << port << "...";

  // Create socket
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    LOG_ERROR << "Failed to create TCP socket";
    return false;
  }

  // Set TCP_NODELAY and socket timeouts
  int flag = 1;
  setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

#if defined(_WIN32)
  DWORD tv_ms = 6000;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
  setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
  struct timeval tv{};
  tv.tv_sec = 6;
  tv.tv_usec = 0;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip_address.c_str(), &server_addr.sin_addr) <= 0) {
    LOG_ERROR << "Invalid IP address: " << ip_address;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Connect with a bounded timeout so a sleeping/offline device fails fast
  // instead of blocking on the OS SYN retry schedule (~21s on Windows).
  int connect_ms = timeout_ms > 0 ? timeout_ms : 5000;
#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(socket_fd_, FIONBIO, &nonblocking);
#else
  int sock_flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, sock_flags | O_NONBLOCK);
#endif

  bool connected = connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
                           sizeof(server_addr)) == 0;
  if (!connected) {
    const int err = SocketLastError();
#if defined(_WIN32)
    const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
    const bool in_progress = (err == EINPROGRESS);
#endif
    if (in_progress) {
      fd_set write_fds;
      FD_ZERO(&write_fds);
      FD_SET(socket_fd_, &write_fds);
      struct timeval connect_tv{};
      connect_tv.tv_sec = connect_ms / 1000;
      connect_tv.tv_usec = (connect_ms % 1000) * 1000;
      if (select(static_cast<int>(socket_fd_) + 1, nullptr, &write_fds, nullptr, &connect_tv) > 0) {
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&so_error), &so_len) == 0) {
          connected = (so_error == 0);
        }
      }
    }
  }

  // Restore blocking mode for the TLS session.
#if defined(_WIN32)
  nonblocking = 0;
  ioctlsocket(socket_fd_, FIONBIO, &nonblocking);
#else
  fcntl(socket_fd_, F_SETFL, sock_flags);
#endif

  if (!connected) {
    LOG_ERROR << "TCP connection to " << ip_address << ":" << port
              << " failed or timed out after " << connect_ms << "ms";
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Initialize OpenSSL
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();

  ssl_ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ssl_ctx_) {
    LOG_ERROR << "Failed to create SSL_CTX";
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Configure Cast device certificate verification policy
  DeviceAuth::ConfigureSslContext(ssl_ctx_, verify_device_cert_);

  // Pin to TLS 1.2: real Chromecasts never offer 1.3, and under 1.3 the
  // client's SSL_read can emit post-handshake records (NewSessionTicket /
  // KeyUpdate processing) concurrently with HeartbeatLoop's SSL_write on the
  // same SSL*. That full-duplex write/write race corrupts records both ways
  // ("bad record mac"). 1.2 keeps read and write cipher states independent.
  SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_TLSv1_3);

  // Chromecast routinely closes the TCP socket without sending a TLS
  // close_notify alert (e.g. on a WiFi glitch or when it tears down a
  // mirroring session). OpenSSL 3.0+ turns that into a hard
  // SSL_ERROR_SSL ("unexpected eof while reading") instead of a clean EOF,
  // which both pollutes diagnostics and can leave the SSL* in a state where
  // SSL_shutdown misbehaves. Treat an abrupt peer close as a clean EOF so we
  // can distinguish "peer closed" (expected) from a real protocol error.
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
  SSL_CTX_set_options(ssl_ctx_, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    LOG_ERROR << "Failed to create SSL structure";
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  SSL_set_fd(ssl_, socket_fd_);

  if (SSL_connect(ssl_) <= 0) {
    unsigned long ssl_err = ERR_get_error();
    char err_buf[256];
    ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
    LOG_ERROR << "SSL handshake failed with Cast device at " << ip_address << ": " << err_buf;
    Disconnect();
    return false;
  }

  is_connected_ = true;
  last_pong_ms_.store(NowMs());
  disconnect_notified_.store(false);
  LOG_INFO << "TLS Cast Channel established with " << ip_address << ":" << port;

  // Start receive and heartbeat threads
  receive_thread_ = std::thread(&CastChannel::ReceiveLoop, this);
  heartbeat_thread_ = std::thread(&CastChannel::HeartbeatLoop, this);

  // Send initial CONNECT to receiver-0
  ConnectVirtual(kPlatformReceiverId, kPlatformSenderId);

  StatusCallback cb;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cb = status_callback_;
  }
  if (cb) {
    cb(true, "");
  }

  return true;
}

void CastChannel::SetAppTransportId(const std::string& transport_id) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  app_transport_id_ = transport_id;
}

void CastChannel::Disconnect() {
  bool was_connected = is_connected_.exchange(false);
  should_stop_ = true;
  auth_cv_.notify_all();

  if (socket_fd_ >= 0) {
#if defined(_WIN32)
    shutdown(socket_fd_, SD_BOTH);
#else
    shutdown(socket_fd_, SHUT_RDWR);
#endif
  }

  if (receive_thread_.joinable() && std::this_thread::get_id() != receive_thread_.get_id()) {
    receive_thread_.join();
  }
  if (heartbeat_thread_.joinable() && std::this_thread::get_id() != heartbeat_thread_.get_id()) {
    heartbeat_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (ssl_) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_ = nullptr;
    }
    if (socket_fd_ >= 0) {
      close(socket_fd_);
      socket_fd_ = -1;
    }
    app_transport_id_.clear();
  }

  if (was_connected) {
    LOG_INFO << "Disconnected Cast Channel from " << ip_address_;
    NotifyDisconnected("Disconnected");
  }
}

bool CastChannel::SendRawPacket(const uint8_t* data, size_t length) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  if (!ssl_ || !is_connected_.load()) return false;

  size_t total_written = 0;
  while (total_written < length) {
    int ret = SSL_write(ssl_, data + total_written, static_cast<int>(length - total_written));
    if (ret <= 0) {
      int err = SSL_get_error(ssl_, ret);
      LOG_ERROR << "SSL_write error: " << err;
      return false;
    }
    total_written += ret;
  }
  return true;
}

bool CastChannel::SendCastMessage(const std::string& namespace_,
                                 const std::string& payload_utf8,
                                 const std::string& destination_id,
                                 const std::string& source_id) {
  if (!is_connected_.load()) return false;

  proto::CastMessage msg;
  msg.set_protocol_version(proto::CastMessage::CASTV2_1_0);
  msg.set_source_id(source_id);
  msg.set_destination_id(destination_id);
  msg.set_namespace_(namespace_);
  msg.set_payload_type(proto::CastMessage::STRING);
  msg.set_payload_utf8(payload_utf8);

  std::string serialized;
  if (!msg.SerializeToString(&serialized)) {
    LOG_ERROR << "Failed to serialize CastMessage";
    return false;
  }

  uint32_t payload_len = static_cast<uint32_t>(serialized.size());
  uint8_t header[4];
  header[0] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
  header[1] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
  header[2] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
  header[3] = static_cast<uint8_t>(payload_len & 0xFF);

  std::vector<uint8_t> packet;
  packet.reserve(4 + payload_len);
  packet.insert(packet.end(), header, header + 4);
  packet.insert(packet.end(), serialized.begin(), serialized.end());

  if (!IsHeartbeatPingPong(namespace_, payload_utf8)) {
    LOG_DEBUG << "[CastChannel SEND] ns=" << namespace_ << " src=" << source_id
              << " dst=" << destination_id << " payload=" << RedactSecrets(payload_utf8);
  }
  return SendRawPacket(packet.data(), packet.size());
}

bool CastChannel::SendCastMessageBinary(const std::string& namespace_,
                                        const std::string& payload_binary,
                                        const std::string& destination_id,
                                        const std::string& source_id) {
  if (!is_connected_.load()) return false;

  proto::CastMessage msg;
  msg.set_protocol_version(proto::CastMessage::CASTV2_1_0);
  msg.set_source_id(source_id);
  msg.set_destination_id(destination_id);
  msg.set_namespace_(namespace_);
  msg.set_payload_type(proto::CastMessage::BINARY);
  msg.set_payload_binary(payload_binary);

  std::string serialized;
  if (!msg.SerializeToString(&serialized)) {
    LOG_ERROR << "Failed to serialize binary CastMessage";
    return false;
  }

  uint32_t payload_len = static_cast<uint32_t>(serialized.size());
  uint8_t header[4];
  header[0] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
  header[1] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
  header[2] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
  header[3] = static_cast<uint8_t>(payload_len & 0xFF);

  std::vector<uint8_t> packet;
  packet.reserve(4 + payload_len);
  packet.insert(packet.end(), header, header + 4);
  packet.insert(packet.end(), serialized.begin(), serialized.end());

  LOG_DEBUG << "[CastChannel SEND] ns=" << namespace_ << " src=" << source_id
            << " dst=" << destination_id << " (binary " << payload_binary.size() << " bytes)";
  return SendRawPacket(packet.data(), packet.size());
}

bool CastChannel::AuthenticateDevice(int timeout_ms) {
  if (!is_connected_.load() || !ssl_) return false;

  // Cast protocol note: the receiver's TLS certificate is self-signed, so an
  // X.509 validation at the TLS layer would always fail. Authenticity is
  // instead proven by signing a fresh nonce with a certificate chain that
  // anchors in the Cast/Eureka root CAs.
  if (!verify_device_cert_) {
    LOG_WARN << "Device authentication disabled; skipping AUTH_CHALLENGE";
    return true;
  }

  std::vector<uint8_t> nonce = DeviceAuth::GenerateNonce(32);
  if (nonce.size() != 32) {
    LOG_ERROR << "Failed to generate device auth nonce";
    return false;
  }

  proto::DeviceAuthMessage challenge_msg;
  challenge_msg.mutable_challenge()->set_sender_nonce(nonce.data(), nonce.size());

  std::string serialized;
  if (!challenge_msg.SerializeToString(&serialized)) {
    LOG_ERROR << "Failed to serialize device auth challenge";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_done_ = false;
    auth_verified_ = false;
    auth_error_.clear();
    auth_nonce_ = nonce;
  }

  if (!SendCastMessageBinary(kNamespaceDeviceAuth, serialized,
                             kPlatformReceiverId, kPlatformSenderId)) {
    LOG_ERROR << "Failed to send AUTH_CHALLENGE";
    return false;
  }

  std::unique_lock<std::mutex> lock(auth_mutex_);
  bool completed = auth_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
    return auth_done_ || should_stop_.load() || !is_connected_.load();
  });
  if (!completed || !auth_done_) {
    LOG_ERROR << "Device authentication timed out after " << timeout_ms << "ms";
    return false;
  }
  if (!auth_verified_) {
    LOG_ERROR << "Device authentication failed: " << auth_error_;
    return false;
  }
  return true;
}

void CastChannel::HandleDeviceAuthResponse(const std::string& payload_binary) {
  DeviceAuthResult result;
  std::vector<uint8_t> signature_input;

  proto::DeviceAuthMessage msg;
  if (!msg.ParseFromString(payload_binary)) {
    result.error_message = "Failed to parse DeviceAuthMessage";
  } else if (msg.has_error()) {
    result.error_message = "Receiver returned DeviceAuthError type " +
                           std::to_string(static_cast<int>(msg.error().error_type()));
  } else if (!msg.has_response()) {
    result.error_message = "DeviceAuthMessage has no response";
  } else {
    const proto::AuthResponse& response = msg.response();

    std::vector<uint8_t> nonce_response;
    {
      std::lock_guard<std::mutex> lock(auth_mutex_);
      nonce_response = auth_nonce_;
    }
    if (response.has_sender_nonce()) {
      const std::string& echoed = response.sender_nonce();
      if (!nonce_response.empty() && echoed.size() != nonce_response.size()) {
        result.error_message = "Device auth response nonce length mismatch";
      } else if (!nonce_response.empty() &&
                 CRYPTO_memcmp(echoed.data(), nonce_response.data(), nonce_response.size()) != 0) {
        result.error_message = "Device auth response nonce mismatch";
      } else {
        nonce_response.assign(echoed.begin(), echoed.end());
      }
    }

    std::vector<uint8_t> leaf(response.client_auth_certificate().begin(),
                              response.client_auth_certificate().end());
    std::vector<std::vector<uint8_t>> intermediates;
    intermediates.reserve(static_cast<size_t>(response.intermediate_certificate_size()));
    for (int i = 0; i < response.intermediate_certificate_size(); ++i) {
      const std::string& inter = response.intermediate_certificate(i);
      intermediates.emplace_back(inter.begin(), inter.end());
    }
    std::vector<uint8_t> signature(response.signature().begin(), response.signature().end());
    std::vector<uint8_t> peer_cert = GetPeerCertificateDer();

    // Chromium signs/verifies the echoed nonce followed by the TLS peer
    // certificate DER.
    signature_input.reserve(nonce_response.size() + peer_cert.size());
    signature_input.insert(signature_input.end(), nonce_response.begin(), nonce_response.end());
    signature_input.insert(signature_input.end(), peer_cert.begin(), peer_cert.end());

    if (result.error_message.empty()) {
      result = DeviceAuth::VerifyAuthResponse(leaf, intermediates, signature, signature_input);
    }
  }

  {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_done_ = true;
    auth_verified_ = result.verified;
    auth_error_ = result.error_message;
  }
  auth_cv_.notify_all();

  if (result.verified) {
    LOG_INFO << "Device authenticated against Cast Root CA (CN=" << result.common_name
             << ", issuer=" << result.peer_cert_issuer << ")";
  } else {
    LOG_WARN << "Device authentication verification failed: " << result.error_message;
  }
}

std::vector<uint8_t> CastChannel::GetPeerCertificateDer() const {
  if (!ssl_) return {};
  X509* cert = SSL_get1_peer_certificate(ssl_);
  if (!cert) return {};
  unsigned char* out = nullptr;
  int len = i2d_X509(cert, &out);
  std::vector<uint8_t> der;
  if (len > 0 && out) {
    der.assign(out, out + len);
  }
  if (out) OPENSSL_free(out);
  X509_free(cert);
  return der;
}

bool CastChannel::ConnectVirtual(const std::string& destination_id, const std::string& source_id) {
  nlohmann::json payload;
  payload["type"] = "CONNECT";
  payload["userAgent"] = "CastMirror/1.0";
  return SendCastMessage(kNamespaceConnection, payload.dump(), destination_id, source_id);
}

bool CastChannel::DisconnectVirtual(const std::string& destination_id, const std::string& source_id) {
  nlohmann::json payload;
  payload["type"] = "CLOSE";
  return SendCastMessage(kNamespaceConnection, payload.dump(), destination_id, source_id);
}

int CastChannel::LaunchApp(const std::string& app_id) {
  int req_id = next_request_id_++;
  nlohmann::json payload;
  payload["type"] = "LAUNCH";
  payload["appId"] = app_id;
  payload["requestId"] = req_id;
  payload["language"] = "en-US";
  payload["supportedAppTypes"] = nlohmann::json::array({"WEB"});

  LOG_INFO << "Sending LAUNCH request for app " << app_id << " (requestId: " << req_id << ")...";
  SendCastMessage(kNamespaceReceiver, payload.dump(), kPlatformReceiverId, kPlatformSenderId);
  return req_id;
}

int CastChannel::StopApp(const std::string& session_id) {
  int req_id = next_request_id_++;
  nlohmann::json payload;
  payload["type"] = "STOP";
  payload["sessionId"] = session_id;
  payload["requestId"] = req_id;

  LOG_INFO << "Sending STOP request for session " << session_id << " (requestId: " << req_id << ")...";
  SendCastMessage(kNamespaceReceiver, payload.dump(), kPlatformReceiverId, kPlatformSenderId);
  return req_id;
}

int CastChannel::RequestReceiverStatus() {
  int req_id = next_request_id_++;
  nlohmann::json payload;
  payload["type"] = "GET_STATUS";
  payload["requestId"] = req_id;

  SendCastMessage(kNamespaceReceiver, payload.dump(), kPlatformReceiverId, kPlatformSenderId);
  return req_id;
}

void CastChannel::ReceiveLoop() {
  uint8_t len_buf[4];

  while (!should_stop_.load() && is_connected_.load()) {
    // Read 4-byte big-endian header
    size_t header_read = 0;
    while (header_read < 4) {
      int ret = SSL_read(ssl_, len_buf + header_read, static_cast<int>(4 - header_read));
      if (ret <= 0) {
        if (should_stop_.load() || !is_connected_.load()) return;
        int err = SSL_get_error(ssl_, ret);
        // Transient retry, NOT a disconnect. The cast-channel socket uses a
        // 6s SO_RCVTIMEO, so when the control channel is idle (common while
        // media flows over UDP and the Chromecast has nothing to send),
        // SSL_read returns -1 with WANT_READ. Tearing the channel down here
        // would cause spurious full reconnects on a perfectly healthy link.
        // Loop and keep waiting for the next real message.
        // On Windows a recv timeout surfaces as SSL_ERROR_SYSCALL with
        // WSAETIMEDOUT (OpenSSL does not classify it as WANT_READ), so also
        // treat a transient socket error as retryable here.
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
            (err == SSL_ERROR_SYSCALL && SocketErrorIsTransient(SocketLastError()))) {
          continue;
        }
        char err_buf[256];
        ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
        LOG_WARN << "Cast Channel socket disconnected while reading header: ret=" << ret << " err=" << err << " (" << err_buf << ")";
        is_connected_ = false;
        NotifyDisconnected("TLS socket closed");
        return;
      }
      header_read += ret;
    }

    uint32_t msg_len = (static_cast<uint32_t>(len_buf[0]) << 24) |
                       (static_cast<uint32_t>(len_buf[1]) << 16) |
                       (static_cast<uint32_t>(len_buf[2]) << 8) |
                       static_cast<uint32_t>(len_buf[3]);

    if (msg_len == 0 || msg_len > 64 * 1024 * 1024) {
      LOG_ERROR << "Invalid CastMessage size: " << msg_len;
      is_connected_ = false;
      return;
    }

    std::vector<uint8_t> payload_buf(msg_len);
    size_t body_read = 0;
    while (body_read < msg_len) {
      int ret = SSL_read(ssl_, payload_buf.data() + body_read, static_cast<int>(msg_len - body_read));
      if (ret <= 0) {
        if (should_stop_.load() || !is_connected_.load()) return;
        int err = SSL_get_error(ssl_, ret);
        // Same transient-retry rationale as the header read: a 6s control-
        // channel idle timeout (WANT_READ) is not a disconnect. On Windows the
        // timeout arrives as SSL_ERROR_SYSCALL + WSAETIMEDOUT.
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
            (err == SSL_ERROR_SYSCALL && SocketErrorIsTransient(SocketLastError()))) {
          continue;
        }
        LOG_WARN << "Cast Channel socket disconnected while reading body";
        is_connected_ = false;
        NotifyDisconnected("TLS socket closed");
        return;
      }
      body_read += ret;
    }

    proto::CastMessage msg;
    if (msg.ParseFromArray(payload_buf.data(), static_cast<int>(msg_len))) {
      std::string payload_str;
      const bool is_binary = msg.payload_type() == proto::CastMessage::BINARY;
      if (is_binary && msg.has_payload_binary()) {
        // Binary payloads (device auth) are handled internally; the bytes are
        // still passed through to the message callback for completeness.
        payload_str = msg.payload_binary();
      } else if (!is_binary && msg.has_payload_utf8()) {
        payload_str = msg.payload_utf8();
      }

      if (!is_binary && !IsHeartbeatPingPong(msg.namespace_(), payload_str)) {
        LOG_DEBUG << "[CastChannel RECV] ns=" << msg.namespace_() << " src=" << msg.source_id()
                  << " dst=" << msg.destination_id() << " payload=" << RedactSecrets(payload_str);
      } else if (is_binary) {
        LOG_DEBUG << "[CastChannel RECV] ns=" << msg.namespace_() << " src=" << msg.source_id()
                  << " dst=" << msg.destination_id() << " (binary " << payload_str.size() << " bytes)";
      }

      // Automatically answer PING with PONG
      if (msg.namespace_() == kNamespaceHeartbeat && !is_binary) {
        try {
          auto j = nlohmann::json::parse(payload_str);
          if (j.contains("type") && j["type"] == "PING") {
            SendCastMessage(kNamespaceHeartbeat, "{\"type\":\"PONG\"}", msg.source_id(), msg.destination_id());
          } else if (j.contains("type") && j["type"] == "PONG") {
            NoteIncomingPong();
          } else {
            LOG_WARN << "Unexpected Cast heartbeat payload: " << payload_str;
          }
        } catch (...) {}
      }

      // Device-auth replies are consumed by the channel itself (the session
      // must not see the raw certificate bytes).
      if (msg.namespace_() == kNamespaceDeviceAuth) {
        HandleDeviceAuthResponse(payload_str);
        continue;
      }

      MessageCallback cb;
      {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = message_callback_;
      }
      if (cb) {
        cb(msg.namespace_(), payload_str, msg.source_id(), msg.destination_id());
      }
    }
  }
}

void CastChannel::HeartbeatLoop() {
  while (!should_stop_.load() && is_connected_.load()) {
    // 40 x 50ms = 2.0 seconds
    for (int i = 0; i < 40; ++i) {
      if (should_stop_.load() || !is_connected_.load()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (should_stop_.load() || !is_connected_.load()) break;

    // 1. Platform keepalive
    SendCastMessage(kNamespaceHeartbeat, "{\"type\":\"PING\"}", kPlatformReceiverId, kPlatformSenderId);

    // 2. Application keepalive
    std::string app_tid;
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      app_tid = app_transport_id_;
    }
    if (!app_tid.empty()) {
      SendCastMessage(kNamespaceHeartbeat, "{\"type\":\"PING\"}", app_tid, kPlatformSenderId);
    }

    if (HeartbeatTimedOut()) {
      LOG_WARN << "No Cast heartbeat PONG for 6s; treating channel as dead";
      is_connected_ = false;
      NotifyDisconnected("Heartbeat timeout");
      return;
    }
  }
}

void CastChannel::NoteIncomingPong() {
  last_pong_ms_.store(NowMs());
}

bool CastChannel::HeartbeatTimedOut() const {
  if (!is_connected_.load()) {
    return true;
  }
  int64_t last = last_pong_ms_.load();
  if (last <= 0) {
    return false;
  }
  return (NowMs() - last) > 6000;
}

void CastChannel::NotifyDisconnected(const std::string& reason) {
  if (disconnect_notified_.exchange(true)) {
    return;
  }
  StatusCallback cb;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cb = status_callback_;
  }
  if (cb) {
    cb(false, reason);
  }
}

} // namespace castcore
