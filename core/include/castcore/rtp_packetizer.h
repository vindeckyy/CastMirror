#ifndef CASTCORE_RTP_PACKETIZER_H_
#define CASTCORE_RTP_PACKETIZER_H_

#include "castcore/types.h"
#include <vector>
#include <cstdint>

namespace castcore {

struct RtpPacket {
  std::vector<uint8_t> data;
  uint16_t sequence_number = 0;
  uint32_t frame_id = 0;
  uint16_t packet_id = 0;
  uint16_t max_packet_id = 0;
  bool is_key_frame = false;
};

class RtpPacketizer {
 public:
  RtpPacketizer(uint8_t payload_type, uint32_t sender_ssrc, int max_packet_size = 1460);
  ~RtpPacketizer();

  std::vector<RtpPacket> PacketizeFrame(const EncodedFrame& encrypted_frame);

  int GetMaxPacketSize() const { return max_packet_size_; }
  uint32_t GetSenderSsrc() const { return sender_ssrc_; }
  uint8_t GetPayloadType() const { return payload_type_; }

 private:
  uint8_t payload_type_ = 96;
  uint32_t sender_ssrc_ = 1;
  int max_packet_size_ = 1460;
  uint16_t sequence_number_ = 0;
};

} // namespace castcore

#endif // CASTCORE_RTP_PACKETIZER_H_
