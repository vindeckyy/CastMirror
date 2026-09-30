// libFuzzer harness for castcore::RtcpParser — the RTCP receiver-feedback
// parser. core/src/rtcp_parser.cc claims "fuzz hardening" in several comments
// (the bounds checks around :48, :60, :102, :122, :150, :176); this is the
// harness that actually feeds it arbitrary bytes.

#include <cstddef>
#include <cstdint>
#include <map>

#include "castcore/rtcp_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr) return 0;
  using namespace castcore;

  RtcpFeedback feedback;
  RtcpParser::ParseCompoundPacket(data, size, /*last_sent_frame_id=*/0u, feedback);

  // The SSRC-keyed overload takes a different branch in the NACK handling.
  std::map<uint32_t, uint32_t> last_frame_by_ssrc;
  last_frame_by_ssrc[0u] = 0u;
  last_frame_by_ssrc[1u] = 1u;
  last_frame_by_ssrc[0xFFFFFFFFu] = 0u;
  RtcpFeedback feedback_by_ssrc;
  RtcpParser::ParseCompoundPacket(data, size, last_frame_by_ssrc, feedback_by_ssrc);

  return 0;
}
