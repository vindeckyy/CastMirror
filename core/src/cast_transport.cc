#include "castcore/cast_transport.h"
#include "castcore/logger.h"

#include <cstring>
#include <chrono>
#include <vector>

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

  int opt = 1;
  setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

  struct sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = 0;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr));

  std::memset(&dest_addr_, 0, sizeof(dest_addr_));
  dest_addr_.sin_family = AF_INET;
  dest_addr_.sin_port = htons(receiver_udp_port);
  if (inet_pton(AF_INET, receiver_ip.c_str(), &dest_addr_.sin_addr) <= 0) {
    LOG_ERROR << "Invalid UDP destination IP: " << receiver_ip;
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Socket buffers for high bitrate streaming
  int sndbuf = 4 * 1024 * 1024;
  int rcvbuf = 2 * 1024 * 1024;
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
    last_sent_frame_id_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    sr_state_.clear();
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

  uint32_t ssrc = 0;
  uint32_t rtp_ts = 0;
  uint32_t octets_this_frame = 0;
  if (packets[0].data.size() >= 12) {
    const uint8_t* p0 = packets[0].data.data();
    rtp_ts = (static_cast<uint32_t>(p0[4]) << 24) | (static_cast<uint32_t>(p0[5]) << 16) |
             (static_cast<uint32_t>(p0[6]) << 8) | static_cast<uint32_t>(p0[7]);
    ssrc = (static_cast<uint32_t>(p0[8]) << 24) | (static_cast<uint32_t>(p0[9]) << 16) |
           (static_cast<uint32_t>(p0[10]) << 8) | static_cast<uint32_t>(p0[11]);
  }

  {
    std::lock_guard<std::mutex> clock(cache_mutex_);
    last_sent_frame_id_[ssrc] = current_fid;
    auto& frame_cache = packet_cache_[ssrc];
    for (const auto& pkt : packets) {
      frame_cache[current_fid][pkt.packet_id] = pkt;
    }

    // Keep the last 60 frames for this SSRC only. Audio and video frame IDs
    // are independent; a shared cache was dropping video packets as soon as
    // audio's higher frame IDs filled the map.
    while (frame_cache.size() > 60) {
      frame_cache.erase(frame_cache.begin());
    }
  }

  for (const auto& pkt : packets) {
    ssize_t sent = sendto(socket_fd_, reinterpret_cast<const char*>(pkt.data.data()), pkt.data.size(), 0,
                          reinterpret_cast<const struct sockaddr*>(&dest_addr_), sizeof(dest_addr_));
    if (sent < 0) {
      LOG_WARN << "UDP send error";
      return false;
    }
    octets_this_frame += static_cast<uint32_t>(pkt.data.size());
  }

  MaybeSendSenderReport(ssrc, rtp_ts, static_cast<uint32_t>(packets.size()), octets_this_frame);

  {
    std::lock_guard<std::mutex> slock(stats_mutex_);
    total_packets_sent_ += static_cast<uint32_t>(packets.size());
    total_frames_sent_++;
  }

  return true;
}

void CastTransport::MaybeSendSenderReport(uint32_t ssrc, uint32_t rtp_timestamp,
                                         uint32_t packets_just_sent, uint32_t octets_just_sent) {
  if (ssrc == 0 || socket_fd_ < 0) return;

  auto& st = sr_state_[ssrc];
  st.last_rtp_timestamp = rtp_timestamp;
  st.packets += packets_just_sent;
  st.octets += octets_just_sent;

  auto now = std::chrono::steady_clock::now();
  if (st.last_sr.time_since_epoch().count() != 0 &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - st.last_sr).count() < 40) {
    return;
  }
  st.last_sr = now;

  auto unix_now = std::chrono::system_clock::now().time_since_epoch();
  uint64_t unix_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(unix_now).count());
  uint32_t ntp_sec = static_cast<uint32_t>((unix_us / 1000000ULL) + 2208988800ULL);
  uint32_t ntp_frac = static_cast<uint32_t>(((unix_us % 1000000ULL) << 32) / 1000000ULL);

  uint8_t sr[28];
  sr[0] = 0x80;  // V=2, P=0, RC=0
  sr[1] = 200;   // Sender Report
  sr[2] = 0x00;
  sr[3] = 0x06;  // length = 6 words after header
  sr[4] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
  sr[5] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
  sr[6] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
  sr[7] = static_cast<uint8_t>(ssrc & 0xFF);
  sr[8]  = static_cast<uint8_t>((ntp_sec >> 24) & 0xFF);
  sr[9]  = static_cast<uint8_t>((ntp_sec >> 16) & 0xFF);
  sr[10] = static_cast<uint8_t>((ntp_sec >> 8) & 0xFF);
  sr[11] = static_cast<uint8_t>(ntp_sec & 0xFF);
  sr[12] = static_cast<uint8_t>((ntp_frac >> 24) & 0xFF);
  sr[13] = static_cast<uint8_t>((ntp_frac >> 16) & 0xFF);
  sr[14] = static_cast<uint8_t>((ntp_frac >> 8) & 0xFF);
  sr[15] = static_cast<uint8_t>(ntp_frac & 0xFF);
  sr[16] = static_cast<uint8_t>((rtp_timestamp >> 24) & 0xFF);
  sr[17] = static_cast<uint8_t>((rtp_timestamp >> 16) & 0xFF);
  sr[18] = static_cast<uint8_t>((rtp_timestamp >> 8) & 0xFF);
  sr[19] = static_cast<uint8_t>(rtp_timestamp & 0xFF);
  sr[20] = static_cast<uint8_t>((st.packets >> 24) & 0xFF);
  sr[21] = static_cast<uint8_t>((st.packets >> 16) & 0xFF);
  sr[22] = static_cast<uint8_t>((st.packets >> 8) & 0xFF);
  sr[23] = static_cast<uint8_t>(st.packets & 0xFF);
  sr[24] = static_cast<uint8_t>((st.octets >> 24) & 0xFF);
  sr[25] = static_cast<uint8_t>((st.octets >> 16) & 0xFF);
  sr[26] = static_cast<uint8_t>((st.octets >> 8) & 0xFF);
  sr[27] = static_cast<uint8_t>(st.octets & 0xFF);

  sendto(socket_fd_, reinterpret_cast<const char*>(sr), sizeof(sr), 0,
         reinterpret_cast<const struct sockaddr*>(&dest_addr_), sizeof(dest_addr_));
}

void CastTransport::RetransmitPacket(uint32_t ssrc, uint32_t frame_id, uint16_t packet_id) {
  std::vector<RtpPacket> to_send;
  {
    std::lock_guard<std::mutex> clock(cache_mutex_);
    auto sit = packet_cache_.find(ssrc);
    if (sit != packet_cache_.end()) {
      auto fit = sit->second.find(frame_id);
      if (fit != sit->second.end()) {
        if (packet_id == 0xFFFF) {
          for (const auto& kv : fit->second) {
            to_send.push_back(kv.second);
          }
        } else {
          auto pit = fit->second.find(packet_id);
          if (pit != fit->second.end()) {
            to_send.push_back(pit->second);
          }
        }
      }
    }
  }

  if (!to_send.empty()) {
    std::lock_guard<std::mutex> slock(send_mutex_);
    if (socket_fd_ >= 0) {
      for (const auto& pkt : to_send) {
        sendto(socket_fd_, reinterpret_cast<const char*>(pkt.data.data()), pkt.data.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&dest_addr_), sizeof(dest_addr_));
      }
    }
  }
}

void CastTransport::ReceiveLoop() {
  uint8_t buffer[4096];

  while (running_.load()) {
    struct sockaddr_in src_addr{};
    socklen_t slen = sizeof(src_addr);
    ssize_t bytes_read = recvfrom(socket_fd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                  reinterpret_cast<struct sockaddr*>(&src_addr), &slen);
    if (bytes_read <= 0) {
      if (!running_.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    std::map<uint32_t, uint32_t> last_by_ssrc;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      last_by_ssrc = last_sent_frame_id_;
    }

    RtcpFeedback feedback;
    bool parsed = RtcpParser::ParseCompoundPacket(buffer, static_cast<size_t>(bytes_read),
                                                 last_by_ssrc, feedback);
    if (parsed) {
      {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        last_loss_fraction_ = feedback.fraction_lost;
        total_nacks_received_ += static_cast<uint32_t>(feedback.nacks.size());
        if (feedback.picture_loss_indicator) {
          total_pli_received_++;
        }
      }

      // Checkpoint acknowledgement: free frames up to checkpoint for this SSRC.
      if (feedback.sender_ssrc != 0 && feedback.checkpoint_frame_id > 0) {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        auto sit = packet_cache_.find(feedback.sender_ssrc);
        if (sit != packet_cache_.end()) {
          auto it = sit->second.begin();
          while (it != sit->second.end() && it->first <= feedback.checkpoint_frame_id) {
            it = sit->second.erase(it);
          }
        }
      }

      // Handle retransmission requests (NACKs)
      for (const auto& nack : feedback.nacks) {
        RetransmitPacket(feedback.sender_ssrc, nack.frame_id, nack.packet_id);
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
