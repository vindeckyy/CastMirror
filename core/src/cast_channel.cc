#include "castcore/cast_channel.h"
#include "castcore/logger.h"
#include <nlohmann/json.hpp>

#include <cstring>
#include <chrono>
#include <csignal>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
#endif

namespace castcore {

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

  // Set TCP_NODELAY
  int flag = 1;
  setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip_address.c_str(), &server_addr.sin_addr) <= 0) {
    LOG_ERROR << "Invalid IP address: " << ip_address;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Connect socket
  if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
    LOG_ERROR << "TCP connection failed to " << ip_address << ":" << port;
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

  // Allow self-signed Cast device certificates (standard Cast v2 sender behavior)
  SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

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
    LOG_ERROR << "SSL handshake failed with Cast device at " << ip_address;
    Disconnect();
    return false;
  }

  is_connected_ = true;
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
    StatusCallback cb;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      cb = status_callback_;
    }
    if (cb) {
      cb(false, "Disconnected");
    }
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

  return SendRawPacket(packet.data(), packet.size());
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
        char err_buf[256];
        ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
        LOG_WARN << "Cast Channel socket disconnected while reading header: ret=" << ret << " err=" << err << " (" << err_buf << ")";
        is_connected_ = false;
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
        if (should_stop_.load()) return;
        LOG_WARN << "Cast Channel socket disconnected while reading body";
        is_connected_ = false;
        return;
      }
      body_read += ret;
    }

    proto::CastMessage msg;
    if (msg.ParseFromArray(payload_buf.data(), static_cast<int>(msg_len))) {
      std::string payload_str;
      if (msg.payload_type() == proto::CastMessage::STRING && msg.has_payload_utf8()) {
        payload_str = msg.payload_utf8();
      }

      // Automatically answer PING with PONG
      if (msg.namespace_() == kNamespaceHeartbeat) {
        try {
          auto j = nlohmann::json::parse(payload_str);
          if (j.contains("type") && j["type"] == "PING") {
            SendCastMessage(kNamespaceHeartbeat, "{\"type\":\"PONG\"}", msg.source_id(), msg.destination_id());
          }
        } catch (...) {}
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
  }
}

} // namespace castcore
