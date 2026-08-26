#include "castcore/rtp_packetizer.h"
#include <algorithm>
#include <random>

namespace castcore {

namespace {

constexpr int kAdaptiveLatencyHeaderSize = 4;

} // namespace

RtpPacketizer::RtpPacketizer(uint8_t payload_type, uint32_t sender_ssrc, int max_packet_size)
    : payload_type_(payload_type),
      sender_ssrc_(sender_ssrc),
      max_packet_size_(max_packet_size) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint16_t> dis(0, 65535);
  sequence_number_ = dis(gen);
}

RtpPacketizer::~RtpPacketizer() = default;

std::vector<RtpPacket> RtpPacketizer::PacketizeFrame(const EncodedFrame& encrypted_frame) {
  std::vector<RtpPacket> packets;

  bool is_key_frame = (encrypted_frame.dependency == FrameDependency::kKeyFrame);
  int base_hdr_size = is_key_frame ? 18 : 19;
  bool include_adaptive_latency = (encrypted_frame.playout_delay.count() > 0);

  int header_size_pkt0 = base_hdr_size + (include_adaptive_latency ? kAdaptiveLatencyHeaderSize : 0);
  int header_size_rest = base_hdr_size;

  int max_payload_pkt0 = max_packet_size_ - header_size_pkt0;
  int max_payload_rest = max_packet_size_ - header_size_rest;

  size_t total_data_size = encrypted_frame.data.size();
  int num_packets = 1;

  if (total_data_size > static_cast<size_t>(max_payload_pkt0)) {
    size_t remaining = total_data_size - max_payload_pkt0;
    num_packets = 1 + static_cast<int>((remaining + max_payload_rest - 1) / max_payload_rest);
  }

  uint16_t max_packet_id = static_cast<uint16_t>(num_packets - 1);
  size_t data_offset = 0;

  for (uint16_t pid = 0; pid < num_packets; ++pid) {
    bool is_last_packet = (pid == max_packet_id);
    bool is_first_packet = (pid == 0);
    bool pkt_adaptive = (is_first_packet && include_adaptive_latency);
    int current_header_size = base_hdr_size + (pkt_adaptive ? kAdaptiveLatencyHeaderSize : 0);
    int current_max_payload = max_packet_size_ - current_header_size;

    size_t chunk_len = std::min(static_cast<size_t>(current_max_payload), total_data_size - data_offset);

    RtpPacket pkt;
    pkt.sequence_number = sequence_number_++;
    pkt.frame_id = encrypted_frame.frame_id;
    pkt.packet_id = pid;
    pkt.max_packet_id = max_packet_id;
    pkt.is_key_frame = is_key_frame;

    pkt.data.resize(current_header_size + chunk_len);
    uint8_t* p = pkt.data.data();

    // Byte 0: V=2, P=0, X=0, CC=0 -> 0x80
    p[0] = 0x80;
    // Byte 1: Marker bit on last packet + Payload Type
    p[1] = (is_last_packet ? 0x80 : 0x00) | (payload_type_ & 0x7F);
    // Bytes 2-3: Sequence number
    p[2] = static_cast<uint8_t>((pkt.sequence_number >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(pkt.sequence_number & 0xFF);
    // Bytes 4-7: RTP Timestamp
    p[4] = static_cast<uint8_t>((encrypted_frame.rtp_timestamp >> 24) & 0xFF);
    p[5] = static_cast<uint8_t>((encrypted_frame.rtp_timestamp >> 16) & 0xFF);
    p[6] = static_cast<uint8_t>((encrypted_frame.rtp_timestamp >> 8) & 0xFF);
    p[7] = static_cast<uint8_t>(encrypted_frame.rtp_timestamp & 0xFF);
    // Bytes 8-11: SSRC
    p[8] = static_cast<uint8_t>((sender_ssrc_ >> 24) & 0xFF);
    p[9] = static_cast<uint8_t>((sender_ssrc_ >> 16) & 0xFF);
    p[10] = static_cast<uint8_t>((sender_ssrc_ >> 8) & 0xFF);
    p[11] = static_cast<uint8_t>(sender_ssrc_ & 0xFF);

    // Cast Header:
    // Byte 12: Flags (Keyframe: 0x80, Dependent frame: 0x40) | (Extension count: 0 or 1)
    uint8_t flags = (is_key_frame ? 0x80 : 0x40);
    if (pkt_adaptive) {
      flags |= 0x01;
    }
    p[12] = flags;

    // Byte 13: Frame ID (lower 8 bits)
    p[13] = static_cast<uint8_t>(encrypted_frame.frame_id & 0xFF);
    // Bytes 14-15: Packet ID
    p[14] = static_cast<uint8_t>((pid >> 8) & 0xFF);
    p[15] = static_cast<uint8_t>(pid & 0xFF);
    // Bytes 16-17: Max Packet ID
    p[16] = static_cast<uint8_t>((max_packet_id >> 8) & 0xFF);
    p[17] = static_cast<uint8_t>(max_packet_id & 0xFF);

    size_t payload_write_pos = 18;
    if (!is_key_frame) {
      // Byte 18: Reference Frame ID (lower 8 bits) - only present when has_reference_frame_id (0x40) is set
      p[18] = static_cast<uint8_t>(encrypted_frame.referenced_frame_id & 0xFF);
      payload_write_pos = 19;
    }

    if (pkt_adaptive) {
      // Extension: Type 1, Size 2
      uint16_t ext_hdr = (1 << 10) | 2;
      p[payload_write_pos]     = static_cast<uint8_t>((ext_hdr >> 8) & 0xFF);
      p[payload_write_pos + 1] = static_cast<uint8_t>(ext_hdr & 0xFF);
      uint16_t delay_val = static_cast<uint16_t>(encrypted_frame.playout_delay.count());
      p[payload_write_pos + 2] = static_cast<uint8_t>((delay_val >> 8) & 0xFF);
      p[payload_write_pos + 3] = static_cast<uint8_t>(delay_val & 0xFF);
      payload_write_pos += 4;
    }

    if (chunk_len > 0) {
      std::copy(encrypted_frame.data.begin() + data_offset,
                encrypted_frame.data.begin() + data_offset + chunk_len,
                p + payload_write_pos);
      data_offset += chunk_len;
    }

    packets.push_back(std::move(pkt));
  }

  return packets;
}

} // namespace castcore
