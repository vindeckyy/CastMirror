#include "castcore/rtcp_parser.h"
#include "castcore/logger.h"
#include <cstring>
#include <map>

namespace castcore {

namespace {

inline uint16_t ReadUint16BE(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t ReadUint32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

// Expands an 8-bit truncated frame ID to a 32-bit frame ID closest to (and <=) reference
uint32_t ExpandFrameId(uint8_t truncated_id, uint32_t reference_id) {
  uint32_t candidate = (reference_id & ~0xFFu) | truncated_id;
  if (candidate > reference_id + 128) {
    if (candidate >= 256) candidate -= 256;
  } else if (candidate + 128 < reference_id) {
    candidate += 256;
  }
  return candidate;
}

uint32_t ReferenceFrameId(uint32_t media_ssrc,
                          uint32_t fallback,
                          const std::map<uint32_t, uint32_t>* last_frame_by_ssrc) {
  if (last_frame_by_ssrc) {
    auto it = last_frame_by_ssrc->find(media_ssrc);
    if (it != last_frame_by_ssrc->end()) {
      return it->second;
    }
  }
  return fallback;
}

bool ParseInternal(const uint8_t* data, size_t length,
                   uint32_t fallback_last_frame_id,
                   const std::map<uint32_t, uint32_t>* last_frame_by_ssrc,
                   RtcpFeedback& out_feedback) {
  // Phase 2 fuzz hardening: empty/truncated/SDES/unknown PT must not crash
  if (!data || length < 4) return false;
  // Limit total blocks to avoid infinite loop on malformed length=0 loops
  constexpr size_t kMaxBlocks = 64;
  size_t blocks_parsed = 0;

  size_t offset = 0;
  bool parsed_any = false;

  while (offset + 4 <= length && blocks_parsed < kMaxBlocks) {
    uint8_t b0 = data[offset];
    uint8_t pt = data[offset + 1];
    // Validate RTCP version (should be 2), but tolerate 0 as fuzz; just skip if not 2?
    uint8_t version = (b0 >> 6) & 0x03;
    // Allow fuzz corpus with version !=2 to be safely skipped, not crash
    if (version != 2) {
      // Try to still parse length to skip, but if length is unreasonable, break
      uint16_t words = ReadUint16BE(&data[offset + 2]);
      size_t block_len = (static_cast<size_t>(words) + 1) * 4;
      if (block_len < 4 || offset + block_len > length) {
        break;
      }
      offset += block_len;
      ++blocks_parsed;
      continue;
    }
    uint16_t words = ReadUint16BE(&data[offset + 2]);
    size_t block_len = (static_cast<size_t>(words) + 1) * 4;

    if (block_len < 4 || offset + block_len > length) {
      break; // Truncated / malformed packet -> stop parsing, return what we have
    }
    // Guard against block_len ==0 infinite loop (words==0xFFFF could overflow, but we already checked >length)
    if (block_len == 0) {
      break;
    }

    uint8_t count_or_subtype = b0 & 0x1F;
    const uint8_t* block = &data[offset];

    if (pt == 201) { // Receiver Report
      if (block_len >= 8) {
        out_feedback.receiver_ssrc = ReadUint32BE(&block[4]);
        if (count_or_subtype > 0 && block_len >= 32) {
          // Report block: ensure we don't read beyond block_len
          out_feedback.fraction_lost = static_cast<double>(block[12]) / 256.0;
          out_feedback.cumulative_lost = (static_cast<uint32_t>(block[13]) << 16) |
                                         (static_cast<uint32_t>(block[14]) << 8) |
                                         static_cast<uint32_t>(block[15]);
          out_feedback.jitter = ReadUint32BE(&block[20]);
        }
        parsed_any = true;
      }
    } else if (pt == 202) { // SDES - explicitly ignored but counted as parsed for robustness
      // SDES (Source Description) - not used for Cast, but fuzz corpus may contain it.
      // We treat it as successfully skipped, not as feedback. Do not set parsed_any.
      // Ensure we don't misinterpret SDES as CAST.
    } else if (pt == 204 || pt == 206) { // Application defined or Payload-specific
      if (block_len >= 12) {
        out_feedback.receiver_ssrc = ReadUint32BE(&block[4]);
        out_feedback.sender_ssrc = ReadUint32BE(&block[8]);

        if (count_or_subtype == 1) {
          // Picture Loss Indicator (PLI) - FMT 1 per RFC 4585
          out_feedback.picture_loss_indicator = true;
          parsed_any = true;
        } else if (block_len >= 20) {
          uint32_t magic = ReadUint32BE(&block[12]);
          if (magic == 0x43415354) { // 'CAST'
            uint32_t media_ssrc = out_feedback.sender_ssrc;
            uint32_t ref_fid = ReferenceFrameId(media_ssrc, fallback_last_frame_id,
                                               last_frame_by_ssrc);
            uint8_t ckpt_id_8 = block[16];
            uint8_t loss_fields_count = block[17];
            // Clamp loss_fields_count to prevent excessive iteration on fuzzed large value
            if (loss_fields_count > 32) loss_fields_count = 32;
            out_feedback.current_playout_delay_ms = ReadUint16BE(&block[18]);
            out_feedback.checkpoint_frame_id = ExpandFrameId(ckpt_id_8, ref_fid);

            size_t loss_offset = 20;
            for (int i = 0; i < loss_fields_count && loss_offset + 4 <= block_len; ++i) {
              uint8_t fid_8 = block[loss_offset];
              uint16_t pid = ReadUint16BE(&block[loss_offset + 1]);
              uint8_t bit_vector = block[loss_offset + 3];

              uint32_t full_fid = ExpandFrameId(fid_8, ref_fid);
              out_feedback.nacks.push_back({full_fid, pid});

              // Check PID bit vector for subsequent missing packets
              for (int b = 0; b < 8; ++b) {
                if ((bit_vector >> b) & 1) {
                  out_feedback.nacks.push_back({full_fid, static_cast<uint16_t>(pid + b + 1)});
                }
              }
              loss_offset += 4;
            }

            // Check for CST2 ACK bitvector - ensure we have at least 6 bytes header
            if (loss_offset + 6 <= block_len) {
              uint32_t cst2_magic = ReadUint32BE(&block[loss_offset]);
              if (cst2_magic == 0x43535432) { // 'CST2'
                uint8_t bvec_octets = block[loss_offset + 5];
                // Clamp bvec_octets to remaining bytes to avoid over-read on fuzz
                size_t remaining = block_len - (loss_offset + 6);
                if (bvec_octets > remaining) bvec_octets = static_cast<uint8_t>(remaining);
                if (bvec_octets > 32) bvec_octets = 32;
                size_t ack_offset = loss_offset + 6;
                for (int oct = 0; oct < bvec_octets && ack_offset + static_cast<size_t>(oct) < block_len; ++oct) {
                  uint8_t byte_val = block[ack_offset + oct];
                  for (int bit = 0; bit < 8; ++bit) {
                    if ((byte_val >> bit) & 1) {
                      uint32_t ack_fid = out_feedback.checkpoint_frame_id + 2 + (oct * 8) + bit;
                      out_feedback.acked_frames.push_back(ack_fid);
                    }
                  }
                }
              }
            }

            parsed_any = true;
          }
        }
      }
    } else if (pt == 207) { // Extended Report
      if (block_len >= 12 && block[4] == 4) { // Receiver Reference Time Report
        parsed_any = true;
      }
    } else {
      // Unknown PT (fuzz corpus) - safely skip block without crashing or marking parsed_any.
      // Unknown PT blocks are counted as processed but not as successful feedback.
    }

    offset += block_len;
    ++blocks_parsed;
  }

  return parsed_any;
}

} // namespace

bool RtcpParser::ParseCompoundPacket(const uint8_t* data, size_t length,
                                    uint32_t last_sent_frame_id,
                                    RtcpFeedback& out_feedback) {
  return ParseInternal(data, length, last_sent_frame_id, nullptr, out_feedback);
}

bool RtcpParser::ParseCompoundPacket(const uint8_t* data, size_t length,
                                    const std::map<uint32_t, uint32_t>& last_frame_by_ssrc,
                                    RtcpFeedback& out_feedback) {
  return ParseInternal(data, length, 0, &last_frame_by_ssrc, out_feedback);
}

} // namespace castcore
