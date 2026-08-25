#ifndef CASTCORE_RTCP_PARSER_H_
#define CASTCORE_RTCP_PARSER_H_

#include "castcore/types.h"
#include <vector>
#include <cstdint>
#include <chrono>

namespace castcore {

struct PacketNack {
  uint32_t frame_id = 0;
  uint16_t packet_id = 0;
};

struct RtcpFeedback {
  uint32_t receiver_ssrc = 0;
  uint32_t sender_ssrc = 0;
  bool picture_loss_indicator = false;
  uint32_t checkpoint_frame_id = 0;
  int current_playout_delay_ms = 400;
  std::vector<PacketNack> nacks;
  std::vector<uint32_t> acked_frames;
  double fraction_lost = 0.0;
  uint32_t cumulative_lost = 0;
  uint32_t jitter = 0;
  double rtt_ms = 0.0;
};

class RtcpParser {
 public:
  static bool ParseCompoundPacket(const uint8_t* data, size_t length,
                                  uint32_t last_sent_frame_id,
                                  RtcpFeedback& out_feedback);
};

} // namespace castcore

#endif // CASTCORE_RTCP_PARSER_H_
