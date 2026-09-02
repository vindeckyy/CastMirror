#ifndef CASTCORE_HTTP_FALLBACK_SERVER_H_
#define CASTCORE_HTTP_FALLBACK_SERVER_H_

#include "castcore/types.h"
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

namespace castcore {

class HttpFallbackServer {
 public:
  HttpFallbackServer();
  ~HttpFallbackServer();

  // Start HTTP server on local port (0 = ephemeral)
  bool Start(uint16_t requested_port = 0);
  void Stop();
  bool IsRunning() const { return running_.load(); }

  uint16_t GetPort() const { return bound_port_; }
  std::string GetStreamUrl(const std::string& local_ip) const;

  // Push an encoded video frame (H.264 Annex B) into live ring buffer
  void PushVideoFrame(const EncodedFrame& frame);

  // Generates CAF receiver LOAD JSON command payload
  static std::string FormatCafLoadMessage(const std::string& media_url,
                                          const std::string& content_type = "video/mp4",
                                          int request_id = 1);

 private:
  void ServerLoop();
  void HandleClient(int client_fd);

  std::atomic<bool> running_{false};
  uint16_t bound_port_ = 0;
  int server_socket_ = -1;
  std::thread accept_thread_;

  std::mutex buffer_mutex_;
  std::vector<uint8_t> stream_header_;
  std::vector<uint8_t> recent_stream_buffer_;
};

}  // namespace castcore

#endif  // CASTCORE_HTTP_FALLBACK_SERVER_H_
