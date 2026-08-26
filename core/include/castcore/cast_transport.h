#ifndef CASTCORE_CAST_TRANSPORT_H_
#define CASTCORE_CAST_TRANSPORT_H_

#include "castcore/types.h"
#include "castcore/rtp_packetizer.h"
#include "castcore/rtcp_parser.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <map>
#include <functional>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

namespace castcore {

class CastTransport {
 public:
  using PliCallback = std::function<void()>;
  using FeedbackCallback = std::function<void(const RtcpFeedback& feedback)>;

  CastTransport();
  ~CastTransport();

  bool Start(const std::string& receiver_ip, uint16_t receiver_udp_port);
  void Stop();
  bool IsRunning() const;

  // Transmit an array of RTP packets for a frame
  bool SendPackets(const std::vector<RtpPacket>& packets);

  void SetPliCallback(PliCallback callback);
  void SetFeedbackCallback(FeedbackCallback callback);

  StreamStats GetStats() const;

 private:
  void ReceiveLoop();
  void RetransmitPacket(uint32_t frame_id, uint16_t packet_id);

  std::string receiver_ip_;
  uint16_t receiver_port_ = 0;
  int socket_fd_ = -1;
  struct sockaddr_in dest_addr_{};

  std::atomic<bool> running_{false};
  std::thread receive_thread_;

  mutable std::mutex send_mutex_;
  mutable std::mutex stats_mutex_;
  mutable std::mutex cache_mutex_;

  // Retransmission cache: frame_id -> map of packet_id -> RtpPacket
  std::map<uint32_t, std::map<uint16_t, RtpPacket>> packet_cache_;
  uint32_t last_sent_frame_id_ = 0;

  PliCallback pli_callback_;
  FeedbackCallback feedback_callback_;

  // Performance metrics
  uint32_t total_packets_sent_ = 0;
  uint32_t total_frames_sent_ = 0;
  uint32_t total_nacks_received_ = 0;
  uint32_t total_pli_received_ = 0;
  double last_rtt_ms_ = 0.0;
  double last_loss_fraction_ = 0.0;
  std::chrono::steady_clock::time_point session_start_time_;
};

} // namespace castcore

#endif // CASTCORE_CAST_TRANSPORT_H_
