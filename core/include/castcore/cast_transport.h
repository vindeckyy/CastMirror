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
#include <chrono>
#include <cstdint>

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
  size_t GetCachedFrameCount(uint32_t ssrc) const;

  // Returns the highest frame id that is safe to drop from the retransmit cache.
  // 0 means "do not erase" (bogus wrap / checkpoint too far ahead).
  static uint32_t SafeCacheEraseLimit(uint32_t checkpoint, uint32_t last_sent);

  // Phase 2 network hardening constants
  static constexpr int kPacingMaxBurst = 10;
  static constexpr int kPacingIntervalMs = 2;
  static constexpr double kRttEwmaAlpha = 0.2;   // EWMA: rtt = 0.8*old + 0.2*sample
  static constexpr double kRttEwmaKeep = 0.8;
  static constexpr double kJitterEwmaAlpha = 0.1; // jitter = 0.9*old + 0.1*|sample-old|
  static constexpr double kJitterEwmaKeep = 0.9;
  static constexpr int kRetransmitSuppressMs = 80;

 private:
  struct SenderReportState {
    uint32_t last_rtp_timestamp = 0;
    uint32_t packets = 0;
    uint32_t octets = 0;
    std::chrono::steady_clock::time_point last_sr{};
  };

  void ReceiveLoop();
  void RetransmitPacket(uint32_t ssrc, uint32_t frame_id, uint16_t packet_id);
  void MaybeSendSenderReport(uint32_t ssrc, uint32_t rtp_timestamp,
                             uint32_t packets_just_sent, uint32_t octets_just_sent);
  bool SendDatagram(const uint8_t* data, size_t length);
  void UpdateEwmaRtt(double sample_ms);
  bool ConsumePacingTokens(size_t packet_count);

  std::string receiver_ip_;
  uint16_t receiver_port_ = 0;
  int socket_fd_ = -1;
  struct sockaddr_in dest_addr_{};
  std::map<uint32_t, SenderReportState> sr_state_;

  std::atomic<bool> running_{false};
  std::thread receive_thread_;

  mutable std::mutex send_mutex_;
  mutable std::mutex stats_mutex_;
  mutable std::mutex cache_mutex_;

  // Retransmission cache: ssrc -> frame_id -> packet_id -> RtpPacket
  std::map<uint32_t, std::map<uint32_t, std::map<uint16_t, RtpPacket>>> packet_cache_;
  std::map<uint32_t, uint32_t> last_sent_frame_id_;
  // Suppress duplicate retransmissions for the same packet within a short
  // receiver retry interval: ssrc -> frame -> packet -> last send time.
  std::map<uint32_t, std::map<uint32_t,
      std::map<uint16_t, std::chrono::steady_clock::time_point>>> last_retransmit_time_;


  PliCallback pli_callback_;
  FeedbackCallback feedback_callback_;

  // Phase 2: pacing token bucket (maxBurst 10 per 2ms) + EWMA RTT/jitter
  double pacing_tokens_ = static_cast<double>(kPacingMaxBurst);
  std::chrono::steady_clock::time_point pacing_last_refill_{};
  double ewma_rtt_ms_ = 0.0;
  double ewma_jitter_ms_ = 0.0;
  bool ewma_initialized_ = false;

  // Performance metrics
  uint32_t total_packets_sent_ = 0;
  uint32_t total_frames_sent_ = 0;
  uint32_t total_nacks_received_ = 0;
  uint32_t total_pli_received_ = 0;
  double last_rtt_ms_ = 0.0;
  double last_jitter_ms_ = 0.0;
  double last_loss_fraction_ = 0.0;
  std::chrono::steady_clock::time_point session_start_time_;
  std::chrono::steady_clock::time_point last_udp_drop_log_{};
  uint32_t udp_drops_since_log_ = 0;
  mutable std::chrono::steady_clock::time_point fps_sample_time_{};
  mutable uint32_t fps_sample_frames_ = 0;
  mutable double current_video_fps_ = 0.0;
};

} // namespace castcore

#endif // CASTCORE_CAST_TRANSPORT_H_
