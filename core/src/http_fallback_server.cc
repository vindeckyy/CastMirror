#include "castcore/http_fallback_server.h"
#include "castcore/logger.h"

#include <cstring>
#include <sstream>
#include <chrono>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace castcore {

HttpFallbackServer::HttpFallbackServer() = default;

HttpFallbackServer::~HttpFallbackServer() {
  Stop();
}

bool HttpFallbackServer::Start(uint16_t requested_port) {
  Stop();

#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_ < 0) {
    LOG_ERROR << "Failed to create HTTP fallback server socket";
    return false;
  }

  int opt = 1;
#if !defined(_WIN32)
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#else
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#endif

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(requested_port);

  if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG_ERROR << "Failed to bind HTTP fallback server socket on port " << requested_port;
#if !defined(_WIN32)
    close(server_socket_);
#else
    closesocket(server_socket_);
#endif
    server_socket_ = -1;
    return false;
  }

  if (listen(server_socket_, 5) < 0) {
    LOG_ERROR << "Failed to listen on HTTP fallback server socket";
#if !defined(_WIN32)
    close(server_socket_);
#else
    closesocket(server_socket_);
#endif
    server_socket_ = -1;
    return false;
  }

  socklen_t len = sizeof(addr);
  if (getsockname(server_socket_, (struct sockaddr*)&addr, &len) == 0) {
    bound_port_ = ntohs(addr.sin_port);
  } else {
    bound_port_ = requested_port;
  }

  running_ = true;
  accept_thread_ = std::thread(&HttpFallbackServer::ServerLoop, this);
  LOG_INFO << "HTTP fallback streaming server listening on port " << bound_port_;
  return true;
}

void HttpFallbackServer::Stop() {
  if (!running_.exchange(false)) return;

  if (server_socket_ >= 0) {
#if !defined(_WIN32)
    shutdown(server_socket_, SHUT_RDWR);
    close(server_socket_);
#else
    shutdown(server_socket_, SD_BOTH);
    closesocket(server_socket_);
#endif
    server_socket_ = -1;
  }

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  std::lock_guard<std::mutex> lock(buffer_mutex_);
  recent_stream_buffer_.clear();
  stream_header_.clear();
  bound_port_ = 0;
}

std::string HttpFallbackServer::GetStreamUrl(const std::string& local_ip) const {
  std::ostringstream ss;
  ss << "http://" << (local_ip.empty() ? "127.0.0.1" : local_ip) << ":" << bound_port_ << "/live.mp4";
  return ss.str();
}

void HttpFallbackServer::PushVideoFrame(const EncodedFrame& frame) {
  if (frame.data.empty()) return;
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  if (frame.dependency == FrameDependency::kKeyFrame) {
    stream_header_ = frame.data;
  }

  // Keep circular buffer capped to ~4MB
  const size_t kMaxBuffer = 4 * 1024 * 1024;
  if (recent_stream_buffer_.size() + frame.data.size() > kMaxBuffer) {
    size_t to_drop = (recent_stream_buffer_.size() + frame.data.size()) - (kMaxBuffer / 2);
    recent_stream_buffer_.erase(recent_stream_buffer_.begin(), recent_stream_buffer_.begin() + to_drop);
  }

  recent_stream_buffer_.insert(recent_stream_buffer_.end(), frame.data.begin(), frame.data.end());
}

std::string HttpFallbackServer::FormatCafLoadMessage(const std::string& media_url,
                                                    const std::string& content_type,
                                                    int request_id) {
  std::ostringstream ss;
  ss << "{"
     << "\"type\":\"LOAD\","
     << "\"requestId\":" << request_id << ","
     << "\"media\":{"
     << "\"contentId\":\"" << media_url << "\","
     << "\"streamType\":\"LIVE\","
     << "\"contentType\":\"" << content_type << "\","
     << "\"metadata\":{\"metadataType\":0,\"title\":\"CastMirror Desktop Stream\"}"
     << "},"
     << "\"autoplay\":true,"
     << "\"currentTime\":0"
     << "}";
  return ss.str();
}

void HttpFallbackServer::HandleClient(int client_fd) {
  char req_buf[2048];
  int n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
  if (n <= 0) {
#if !defined(_WIN32)
    close(client_fd);
#else
    closesocket(client_fd);
#endif
    return;
  }
  req_buf[n] = '\0';

  // Return standard HTTP 200 with CORS
  std::string response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: video/mp4\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET, OPTIONS, HEAD\r\n"
      "Access-Control-Allow-Headers: *\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: close\r\n\r\n";

  send(client_fd, response.data(), response.size(), 0);

  // Send current stream data
  std::vector<uint8_t> payload;
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!stream_header_.empty()) {
      payload.insert(payload.end(), stream_header_.begin(), stream_header_.end());
    }
    payload.insert(payload.end(), recent_stream_buffer_.begin(), recent_stream_buffer_.end());
  }

  if (!payload.empty()) {
    send(client_fd, reinterpret_cast<const char*>(payload.data()), payload.size(), 0);
  }

#if !defined(_WIN32)
  close(client_fd);
#else
  closesocket(client_fd);
#endif
}

void HttpFallbackServer::ServerLoop() {
  while (running_) {
    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_socket_, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (!running_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    HandleClient(client_fd);
  }
}

}  // namespace castcore
