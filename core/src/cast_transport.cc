#include "castcore/cast_transport.h"
#include "castcore/logger.h"

#include <cstring>
#include <chrono>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
#endif

namespace castcore {

CastTransport::CastTransport() = default;

CastTransport::~CastTransport() {
  Stop();
}

bool CastTransport::Start(const std::string& receiver_ip, uint16_t receiver_udp_port) {
  Stop();

  receiver_ip_ = receiver_ip;
  receiver_port_ = receiver_udp_port;
  session_start_time_ = std::chrono::steady_clock::now();

  LOG_INFO << "Starting Cast Media Transport to UDP " << receiver_ip << ":" << receiver_udp_port << "...";

  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOG_ERROR << "Failed to create media UDP socket";
    return false;
  }

  // Connect UDP socket for performance and direct send/recv
  struct sockaddr_in dest_addr{};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(receiver_udp_port);
  if (inet_pton(AF_INET, receiver_ip.c_str(), &dest_addr.sin_addr) <= 0) {
    LOG_ERROR << "Invalid UDP destination IP: " << receiver_ip;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr)) < 0) {
    LOG_ERROR << "Failed to connect UDP socket to " << receiver_ip << ":" << receiver_udp_port;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Socket buffers for high bitrate streaming
  int sndbuf = 2 * 1024 * 1024;
  int rcvbuf = 1024 * 1024;
  setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

  running_ = true;
  receive_thread_ = std::thread(&CastTransport::ReceiveLoop, this);
  return true;
}

void CastTransport::Stop() {
  if (!running_.exchange(false)) return;

  LOG_INFO << "Stopping Cast Media Transport...";
  if (socket_fd_ >= 0) {
#if defined(_WIN32)
    shutdown(socket_fd_, SD_BOTH);
#else
    shutdown(socket_fd_, SHUT_RDWR);
#endif
    close(socket_fd_);
    socket_fd_ = -1;
  }

  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    packet_cache_.clear();
  }
}

bool CastTransport::IsRunning() const {
  return running_.load();
}

void CastTransport::SetPliCallback(PliCallback callback) {
  pli_callback_ = std::move(callback);
}

void CastTransport::SetFeedbackCallback(FeedbackCallback callback) {
  feedback_callback_ = std::move(callback);
}

bool CastTransport::SendPackets(const std::vector<RtpPacket>& packets) {
  if (!running_.load() || socket_fd_ < 0 || packets.empty()) return false;

  std::lock_guard<std::mutex> lock(send_mutex_);
  uint32_t current_fid = packets[0].frame_id;

  {
    std::lock_guard<std::mutex> clock(cache_mutex_);
    last_sent_frame_id_ = current_fid;
    for (const auto& pkt : packets) {
      packet_cache_[current_fid][pkt.packet_id] = pkt;
    }

    // Prune old frames from cache (keep last 60 frames)
    while (packet_cache_.size() > 60) {
      packet_cache_.erase(packet_cache_.begin());
    }
  }

  for (const auto& pkt : packets) {
    ssize_t sent = send(socket_fd_, reinterpret_cast<const char*>(pkt.data.data()), pkt.data.size(), 0);
    if (sent < 0) {
      LOG_WARN << "UDP send error";
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> slock(stats_mutex_);
    total_packets_sent_ += static_cast<uint32_t>(packets.size());
    total_frames_sent_++;
  }

  return true;
}

void CastTransport::RetransmitPacket(uint32_t frame_id, uint16_t packet_id) {
  std::lock_guard<std::mutex> clock(cache_mutex_);
  auto fit = packet_cache_.find(frame_id);
  if (fit != packet_cache_.end()) {
    auto pit = fit->second.find(packet_id);
    if (pit != fit->second.end()) {
      const auto& pkt = pit->second;
      std::lock_guard<std::mutex> slock(send_mutex_);
      send(socket_fd_, reinterpret_cast<const char*>(pkt.data.data()), pkt.data.size(), 0);
    }
  }
}

void CastTransport::ReceiveLoop() {
  uint8_t buffer[4096];

  while (running_.load()) {
    ssize_t bytes_read = recv(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (bytes_read <= 0) {
      if (!running_.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    uint32_t last_fid;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      last_fid = last_sent_frame_id_;
    }

    RtcpFeedback feedback;
    if (RtcpParser::ParseCompoundPacket(buffer, static_cast<size_t>(bytes_read), last_fid, feedback)) {
      {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        last_loss_fraction_ = feedback.fraction_lost;
        total_nacks_received_ += static_cast<uint32_t>(feedback.nacks.size());
        if (feedback.picture_loss_indicator) {
          total_pli_received_++;
        }
      }

      // Checkpoint acknowledgement: free frames up to checkpoint
      if (feedback.checkpoint_frame_id > 0) {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        auto it = packet_cache_.begin();
        while (it != packet_cache_.end() && it->first <= feedback.checkpoint_frame_id) {
          it = packet_cache_.erase(it);
        }
      }

      // Handle retransmission requests (NACKs)
      for (const auto& nack : feedback.nacks) {
        RetransmitPacket(nack.frame_id, nack.packet_id);
      }

      // Picture Loss Indicator: request new keyframe
      if (feedback.picture_loss_indicator && pli_callback_) {
        LOG_INFO << "Received RTCP Picture Loss Indicator (PLI) -> requesting keyframe";
        pli_callback_();
      }

      if (feedback_callback_) {
        feedback_callback_(feedback);
      }
    }
  }
}

StreamStats CastTransport::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  StreamStats s;
  s.packets_sent = total_packets_sent_;
  s.frames_sent = total_frames_sent_;
  s.nacks_received = total_nacks_received_;
  s.pli_received = total_pli_received_;
  s.packet_loss_fraction = last_loss_fraction_;
  s.round_trip_time_ms = last_rtt_ms_;

  auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - session_start_time_).count();
  if (elapsed_s > 0) {
    s.current_fps = static_cast<double>(total_frames_sent_) / elapsed_s;
  }
  return s;
}

} // namespace castcore
