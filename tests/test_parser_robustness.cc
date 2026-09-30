// Seeded mutation fuzzing of the parsers that read bytes straight off the LAN.
//
// The libFuzzer harnesses in tests/fuzz find deeper bugs but need a Clang target
// that supports them, which the MinGW toolchain on Windows does not. This runs the
// same entry points on every platform, in every normal test run and in the
// UBSan build, with a fixed seed so a failure reproduces. It asserts one thing:
// hostile input never crashes, hangs or triggers undefined behaviour.

#include <gtest/gtest.h>

#include "castcore/device_discovery.h"
#include "castcore/rtcp_parser.h"

#include <cstdint>
#include <random>
#include <vector>

using namespace castcore;

namespace {

using Bytes = std::vector<uint8_t>;

void Put16(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

// A compound RTCP packet made of plausible blocks with random bodies. The header
// fields (version, count, packet type, length in words) are mostly valid so the
// parser gets past the first checks and into the block handlers.
Bytes RandomRtcp(std::mt19937& rng) {
  static const uint8_t kTypes[] = {200, 201, 202, 203, 205, 206, 207};
  Bytes packet;
  const int blocks = 1 + static_cast<int>(rng() % 4);
  for (int i = 0; i < blocks; ++i) {
    const uint8_t type = kTypes[rng() % sizeof(kTypes)];
    const uint8_t count = static_cast<uint8_t>(rng() % 32);
    const uint16_t words = static_cast<uint16_t>(rng() % 40);
    packet.push_back(static_cast<uint8_t>(0x80 | count));
    packet.push_back(type);
    Put16(packet, words);
    // Usually the body matches the declared length; sometimes it doesn't.
    size_t body = (rng() % 4 == 0) ? rng() % 200 : static_cast<size_t>(words) * 4;
    for (size_t j = 0; j < body; ++j)
      packet.push_back(static_cast<uint8_t>(rng()));
  }
  return packet;
}

void Mutate(Bytes& b, std::mt19937& rng) {
  if (b.empty()) return;
  switch (rng() % 5) {
    case 0: b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
    case 1: b.resize(rng() % (b.size() + 1)); break;  // truncate
    case 2: b[rng() % b.size()] = static_cast<uint8_t>(rng()); break;  // overwrite
    case 3: b.insert(b.begin() + rng() % (b.size() + 1), static_cast<uint8_t>(rng())); break;
    case 4: b.erase(b.begin() + rng() % b.size()); break;
  }
}

// A DNS-shaped packet: header with arbitrary counts, then records with names that
// use labels, compression pointers (including loops and forward jumps), and
// RDLENGTH values that lie about the space left.
Bytes RandomMdns(std::mt19937& rng) {
  Bytes p;
  Put16(p, 0);
  Put16(p, (rng() % 3 == 0) ? static_cast<uint16_t>(rng()) : 0x8400);
  for (int i = 0; i < 4; ++i)
    Put16(p, static_cast<uint16_t>(rng() % 6));
  const int records = static_cast<int>(rng() % 8);
  for (int r = 0; r < records; ++r) {
    const int labels = static_cast<int>(rng() % 4);
    for (int l = 0; l < labels; ++l) {
      const uint8_t len =
          (rng() % 8 == 0) ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>(rng() % 12);
      p.push_back(len);
      for (uint8_t c = 0; c < (len & 0x3F); ++c)
        p.push_back(static_cast<uint8_t>('a' + rng() % 26));
    }
    if (rng() % 4 == 0) {
      p.push_back(0xC0);
      p.push_back(static_cast<uint8_t>(rng()));
    } else
      p.push_back(0);
    static const uint16_t kTypes[] = {1, 12, 16, 28, 33};
    Put16(p, kTypes[rng() % 5]);
    Put16(p, 0x8001);
    p.push_back(0);
    p.push_back(0);
    p.push_back(0);
    p.push_back(120);
    const uint16_t rdlength =
        (rng() % 3 == 0) ? static_cast<uint16_t>(rng()) : static_cast<uint16_t>(rng() % 40);
    Put16(p, rdlength);
    const size_t body = (rng() % 3 == 0) ? rng() % 60 : rdlength;
    for (size_t i = 0; i < body && i < 200; ++i) {
      // TXT-looking bytes some of the time so the key=value path runs.
      p.push_back((rng() % 2) ? static_cast<uint8_t>(rng())
                              : static_cast<uint8_t>("id=fn=md=ca="[rng() % 11]));
    }
  }
  return p;
}

}  // namespace

TEST(ParserRobustnessTest, RtcpParserSurvivesGeneratedAndMutatedPackets) {
  std::mt19937 rng(0xCA57);
  int parsed = 0;
  for (int i = 0; i < 60000; ++i) {
    Bytes packet = RandomRtcp(rng);
    for (int m = rng() % 4; m > 0; --m)
      Mutate(packet, rng);
    RtcpFeedback fb;
    if (RtcpParser::ParseCompoundPacket(
            packet.data(), packet.size(), static_cast<uint32_t>(rng()), fb)) {
      ++parsed;
      // Whatever it accepted must be internally sane.
      EXPECT_LE(fb.nacks.size(), 100000u);
      EXPECT_GE(fb.fraction_lost, 0.0);
      EXPECT_LE(fb.fraction_lost, 1.0);
    }
  }
  EXPECT_GT(parsed, 100) << "the generator should reach the block handlers, not just fail early";
}

TEST(ParserRobustnessTest, RtcpParserHandlesEmptyAndTinyInputs) {
  RtcpFeedback fb;
  EXPECT_FALSE(RtcpParser::ParseCompoundPacket(nullptr, 0, 0, fb));
  for (size_t n = 0; n < 12; ++n) {
    Bytes zeros(n, 0);
    Bytes ones(n, 0xFF);
    RtcpParser::ParseCompoundPacket(zeros.data(), zeros.size(), 0, fb);
    RtcpParser::ParseCompoundPacket(ones.data(), ones.size(), 0xFFFFFFFFu, fb);
  }
}

TEST(ParserRobustnessTest, MdnsParserSurvivesGeneratedAndMutatedPackets) {
  std::mt19937 rng(0xD05);
  DeviceDiscovery discovery;
  for (int i = 0; i < 40000; ++i) {
    Bytes packet = RandomMdns(rng);
    for (int m = rng() % 4; m > 0; --m)
      Mutate(packet, rng);
    discovery.ProcessMdnsResponse(packet.data(), packet.size(), "192.0.2.1");
    if (i % 5000 == 0) {
      // The list must stay bounded and well-formed however much garbage arrives.
      for (const auto& d : discovery.GetDevices()) {
        EXPECT_FALSE(d.ip_address.empty());
        EXPECT_TRUE(d.port == 8009 || d.port == 8008);
      }
    }
  }
}

TEST(ParserRobustnessTest, TxtParserHandlesHostileEntries) {
  std::mt19937 rng(7);
  for (int i = 0; i < 20000; ++i) {
    std::vector<std::string> entries;
    const int n = static_cast<int>(rng() % 12);
    for (int e = 0; e < n; ++e) {
      std::string s;
      const size_t len = (rng() % 10 == 0) ? 300 + rng() % 300 : rng() % 40;
      for (size_t c = 0; c < len; ++c)
        s.push_back(static_cast<char>(rng() % 3 == 0 ? '=' : rng()));
      entries.push_back(std::move(s));
    }
    auto map = DeviceDiscovery::ParseTxtRecord(entries);
    for (const auto& [k, v] : map) {
      EXPECT_LE(k.size(), 64u);
      EXPECT_LE(v.size(), 255u);
    }
  }
}
