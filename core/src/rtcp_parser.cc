#include "castcore/rtcp_parser.h"
#include "castcore/logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

namespace castcore {

namespace {

inline uint16_t ReadUint16BE(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t ReadUint32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
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

uint32_t ReferenceFrameId(uint32_t media_ssrc, uint32_t fallback,
                          const std::map<uint32_t, uint32_t>* last_frame_by_ssrc) {
  if (last_frame_by_ssrc) {
    auto it = last_frame_by_ssrc->find(media_ssrc);
    if (it != last_frame_by_ssrc->end()) {
      return it->second;
    }
  }
  return fallback;
}

bool ParseInternal(const uint8_t* data, size_t length, uint32_t fallback_last_frame_id,
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
      break;  // Truncated / malformed packet -> stop parsing, return what we have
    }
    // Guard against block_len ==0 infinite loop (words==0xFFFF could overflow, but we already checked >length)
    if (block_len == 0) {
      break;
    }

    uint8_t count_or_subtype = b0 & 0x1F;
    const uint8_t* block = &data[offset];

    if (pt == 201) {  // Receiver Report
      if (block_len >= 8) {
        out_feedback.receiver_ssrc = ReadUint32BE(&block[4]);
        if (count_or_subtype > 0 && block_len >= 32) {
          // Report block: ensure we don't read beyond block_len
          out_feedback.has_report_block = true;
          out_feedback.fraction_lost = static_cast<double>(block[12]) / 256.0;
          out_feedback.cumulative_lost = (static_cast<uint32_t>(block[13]) << 16) |
                                         (static_cast<uint32_t>(block[14]) << 8) |
                                         static_cast<uint32_t>(block[15]);
          out_feedback.jitter = ReadUint32BE(&block[20]);

          // RFC 3550 6.4.1 round-trip time. LSR (report block bytes 24..27) is
          // the middle 32 bits of the NTP timestamp from our last Sender Report;
          // DLSR (bytes 28..31) is the delay between the receiver receiving that
          // SR and emitting this RR, in 1/65536 s. Therefore
          //   rtt = (now_mid32 - LSR - DLSR) / 65536 s
          // with now_mid32 = ((unix_seconds + 2208988800) << 16) | ntp_fraction.
          const uint32_t lsr = ReadUint32BE(&block[24]);
          const uint32_t dlsr = ReadUint32BE(&block[28]);
          if (lsr != 0) {  // 0 => receiver has not seen a Sender Report from us yet
            const auto now_epoch = std::chrono::system_clock::now().time_since_epoch();
            const uint64_t now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now_epoch).count());
            const uint32_t unix_seconds = static_cast<uint32_t>(now_us / 1000000ULL);
            const uint32_t fraction =
                static_cast<uint32_t>(((now_us % 1000000ULL) * 65536ULL) / 1000000ULL);
            const uint32_t now_mid32 = ((unix_seconds + 2208988800u) << 16) | (fraction & 0xFFFFu);
            // Signed modular difference: a negative result is a wrapped or bogus
            // DLSR and is ignored rather than reported as a huge RTT.
            const int32_t delta = static_cast<int32_t>(now_mid32 - lsr - dlsr);
            if (delta > 0) {
              double rtt_ms = (static_cast<double>(delta) / 65536.0) * 1000.0;
              constexpr double kMaxSaneRttMs = 3000.0;  // clamp bogus values
              if (rtt_ms > kMaxSaneRttMs) rtt_ms = kMaxSaneRttMs;
              out_feedback.rtt_ms = rtt_ms;
              out_feedback.has_rtt = true;
            }
          }
        }
        parsed_any = true;
      }
    } else if (pt == 202) {  // SDES - explicitly ignored but counted as parsed for robustness
      // SDES (Source Description) - not used for Cast, but fuzz corpus may contain it.
      // We treat it as successfully skipped, not as feedback. Do not set parsed_any.
      // Ensure we don't misinterpret SDES as CAST.
    } else if (pt == 204 || pt == 206) {  // Application defined or Payload-specific
      if (block_len >= 12) {
        out_feedback.receiver_ssrc = ReadUint32BE(&block[4]);
        out_feedback.sender_ssrc = ReadUint32BE(&block[8]);

        if (count_or_subtype == 1 && pt == 206) {
          // Picture Loss Indicator (PLI) - PT 206 (Payload-Specific Feedback),
          // FMT 1 per RFC 4585. PT 204 is Application-Defined; the same FMT
          // value there is NOT a PLI and must not trigger a keyframe.
          out_feedback.picture_loss_indicator = true;
          parsed_any = true;
        } else if (block_len >= 20) {
          uint32_t magic = ReadUint32BE(&block[12]);
          if (magic == 0x43415354) {  // 'CAST'
            uint32_t media_ssrc = out_feedback.sender_ssrc;
            uint32_t ref_fid =
                ReferenceFrameId(media_ssrc, fallback_last_frame_id, last_frame_by_ssrc);
            uint8_t ckpt_id_8 = block[16];
            // Spec range is 0..255 loss fields. The walk below is bounded by
            // block_len (itself bounded by the datagram length), so all 255 can
            // be consumed without reading past the message.
            const uint8_t loss_fields_count = block[17];
            out_feedback.current_playout_delay_ms = ReadUint16BE(&block[18]);
            out_feedback.has_playout_delay = true;
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
              if (cst2_magic == 0x43535432) {  // 'CST2'
                uint8_t bvec_octets = block[loss_offset + 5];
                // Spec range is 2..254 octets. Clamp to what this message
                // actually carries so a large or truncated count cannot read
                // out of bounds.
                const size_t remaining = block_len - (loss_offset + 6);
                const size_t max_octets = std::min<size_t>(remaining, 254);
                if (static_cast<size_t>(bvec_octets) > max_octets) {
                  bvec_octets = static_cast<uint8_t>(max_octets);
                }
                size_t ack_offset = loss_offset + 6;
                for (int oct = 0;
                     oct < bvec_octets && ack_offset + static_cast<size_t>(oct) < block_len;
                     ++oct) {
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
    } else if (pt == 207) {  // Extended Report
      // block[0..3] is the XR header and block[4..7] the report author SSRC, so
      // the first report block's type (BT) lives at block[8]. BT 4 is the
      // Receiver Reference Time Report (RFC 3611 4.1).
      if (block_len >= 12 && block[8] == 4) {
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

}  // namespace

bool RtcpParser::ParseCompoundPacket(const uint8_t* data, size_t length,
                                     uint32_t last_sent_frame_id, RtcpFeedback& out_feedback) {
  return ParseInternal(data, length, last_sent_frame_id, nullptr, out_feedback);
}

bool RtcpParser::ParseCompoundPacket(const uint8_t* data, size_t length,
                                     const std::map<uint32_t, uint32_t>& last_frame_by_ssrc,
                                     RtcpFeedback& out_feedback) {
  return ParseInternal(data, length, 0, &last_frame_by_ssrc, out_feedback);
}

}  // namespace castcore
