// Coverage for DeviceDiscovery's mDNS / TXT parsers.
//
// These parsers consume untrusted input: any host on the LAN can multicast an
// mDNS response to 224.0.0.251:5353. A regression in the field mapping or in
// the SRV-port coercion silently changes which device the UI offers, so the
// cases below pin the exact behaviour rather than the happy path only.

#include <gtest/gtest.h>
#include "castcore/device_discovery.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace castcore;

namespace {

constexpr uint32_t kDefaultCaps = kCapVideoOut | kCapAudioOut;

// --- Minimal DNS wire-format builder -----------------------------------------

void AppendU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendU32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Uncompressed domain name: length-prefixed labels, root terminator.
void AppendName(std::vector<uint8_t>& b, const std::string& dotted) {
  size_t start = 0;
  while (start < dotted.size()) {
    size_t dot = dotted.find('.', start);
    if (dot == std::string::npos) dot = dotted.size();
    const std::string label = dotted.substr(start, dot - start);
    b.push_back(static_cast<uint8_t>(label.size()));
    b.insert(b.end(), label.begin(), label.end());
    start = dot + 1;
  }
  b.push_back(0x00);
}

// TYPE TXT (16): a sequence of length-prefixed strings.
void AppendTxtRecord(std::vector<uint8_t>& b, const std::string& name,
                     const std::vector<std::string>& strings) {
  AppendName(b, name);
  AppendU16(b, 16);
  AppendU16(b, 0x8001);  // IN | cache-flush
  AppendU32(b, 120);
  std::vector<uint8_t> rdata;
  for (const auto& s : strings) {
    rdata.push_back(static_cast<uint8_t>(s.size()));
    rdata.insert(rdata.end(), s.begin(), s.end());
  }
  AppendU16(b, static_cast<uint16_t>(rdata.size()));
  b.insert(b.end(), rdata.begin(), rdata.end());
}

// TYPE SRV (33): priority(2) weight(2) port(2) target-name.
void AppendSrvRecord(std::vector<uint8_t>& b, const std::string& name, uint16_t port) {
  AppendName(b, name);
  AppendU16(b, 33);
  AppendU16(b, 0x8001);
  AppendU32(b, 120);
  AppendU16(b, 7);  // prio + weight + port + root
  AppendU16(b, 0);
  AppendU16(b, 0);
  AppendU16(b, port);
  b.push_back(0x00);
}

std::vector<uint8_t> MdnsHeader(uint16_t qd, uint16_t an, uint16_t ns, uint16_t ar) {
  std::vector<uint8_t> b;
  AppendU16(b, 0);       // ID
  AppendU16(b, 0x8400);  // response, authoritative
  AppendU16(b, qd);
  AppendU16(b, an);
  AppendU16(b, ns);
  AppendU16(b, ar);
  return b;
}

const char* kInstance = "Living Room TV._googlecast._tcp.local";

}  // namespace

// ---------------------------------------------------------------------------
// ParseTxtRecord
// ---------------------------------------------------------------------------

TEST(DeviceDiscoveryTest, ParseTxtRecordMapsCastFields) {
  auto txt = DeviceDiscovery::ParseTxtRecord(
      {"id=abcd1234", "fn=Living Room TV", "md=Chromecast Ultra", "ca=5", "st=1",
       "ve=05", "rs=mirroring"});
  ASSERT_EQ(txt.size(), 7u);
  EXPECT_EQ(txt["id"], "abcd1234");
  EXPECT_EQ(txt["fn"], "Living Room TV");
  EXPECT_EQ(txt["md"], "Chromecast Ultra");
  EXPECT_EQ(txt["ca"], "5");
  EXPECT_EQ(txt["st"], "1");
  EXPECT_EQ(txt["ve"], "05");
  EXPECT_EQ(txt["rs"], "mirroring");
}

TEST(DeviceDiscoveryTest, ParseTxtRecordKeepsFirstDuplicateKey) {
  auto txt = DeviceDiscovery::ParseTxtRecord({"fn=First", "fn=Second"});
  ASSERT_EQ(txt.count("fn"), 1u);
  EXPECT_EQ(txt["fn"], "First");
}

TEST(DeviceDiscoveryTest, ParseTxtRecordSkipsOversizeAndMalformedEntries) {
  const std::string oversize_value = "fn=" + std::string(300, 'x');  // > 255: skipped
  const std::string oversize_key = std::string(80, 'k') + "=v";      // key > 64: skipped
  const std::string bare_oversize_key(80, 'k');                      // key > 64: skipped
  auto txt = DeviceDiscovery::ParseTxtRecord(
      {oversize_value, oversize_key, bare_oversize_key, "=novalue", "fn=ok"});
  ASSERT_EQ(txt.size(), 1u);
  EXPECT_EQ(txt["fn"], "ok");
}

TEST(DeviceDiscoveryTest, ParseTxtRecordKeyWithoutValueIsEmptyString) {
  auto txt = DeviceDiscovery::ParseTxtRecord({"flag"});
  ASSERT_EQ(txt.count("flag"), 1u);
  EXPECT_EQ(txt["flag"], "");
}

TEST(DeviceDiscoveryTest, ParseTxtRecordIgnoresEmptyEntries) {
  EXPECT_TRUE(DeviceDiscovery::ParseTxtRecord({""}).empty());
  EXPECT_TRUE(DeviceDiscovery::ParseTxtRecord({}).empty());
}

// ---------------------------------------------------------------------------
// ParseFromMdnsData
// ---------------------------------------------------------------------------

TEST(DeviceDiscoveryTest, ParseFromMdnsDataMapsTxtOverSrvName) {
  CastDevice dev = DeviceDiscovery::ParseFromMdnsData(
      "instance-name", "10.0.0.9", 8009,
      {"id=uuid-1", "fn=Bedroom", "md=Chromecast with Google TV", "ca=5", "st=1"});
  EXPECT_EQ(dev.id, "uuid-1");
  EXPECT_EQ(dev.name, "Bedroom");
  EXPECT_EQ(dev.model_name, "Chromecast with Google TV");
  EXPECT_EQ(dev.ip_address, "10.0.0.9");
  EXPECT_EQ(dev.capabilities, 5u);
  EXPECT_EQ(dev.status, DeviceStatus::kBusy);
}

TEST(DeviceDiscoveryTest, ParseFromMdnsDataFallsBackWhenIdMissing) {
  CastDevice dev = DeviceDiscovery::ParseFromMdnsData("", "192.168.1.42", 8009, {"fn=Kitchen"});
  EXPECT_EQ(dev.id, "192.168.1.42");        // id falls back to the sender IP
  EXPECT_EQ(dev.name, "Kitchen");           // name comes from 'fn'
  EXPECT_EQ(dev.model_name, "Chromecast");  // 'md' missing
  EXPECT_EQ(dev.capabilities, kDefaultCaps);
  EXPECT_EQ(dev.status, DeviceStatus::kReady);
}

TEST(DeviceDiscoveryTest, ParseFromMdnsDataNameFallsBackToChromecastIp) {
  CastDevice dev = DeviceDiscovery::ParseFromMdnsData("", "10.0.0.7", 8009, {"id=x"});
  EXPECT_EQ(dev.name, "Chromecast-10.0.0.7");
  EXPECT_EQ(dev.model_name, "Chromecast");
}

TEST(DeviceDiscoveryTest, ParseFromMdnsDataMalformedCaFallsBackToDefaults) {
  CastDevice dev = DeviceDiscovery::ParseFromMdnsData("", "10.0.0.9", 8009, {"ca=not-a-number"});
  EXPECT_EQ(dev.capabilities, kDefaultCaps);
}

TEST(DeviceDiscoveryTest, ParseFromMdnsDataStatusOnlyConsidersExactOne) {
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.9", 8009, {"st=1"}).status,
            DeviceStatus::kBusy);
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.9", 8009, {"st=0"}).status,
            DeviceStatus::kReady);
  // Anything other than the literal "1" is treated as idle.
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.9", 8009, {"st=true"}).status,
            DeviceStatus::kReady);
}

// The SRV port is coerced onto the Cast V2 control port: only 8009/8008 survive
// (device_discovery.cc:488). A regression here silently retargets the UI.
TEST(DeviceDiscoveryTest, SrvPortIsCoercedToCastControlPort) {
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.5", 8009, {}).port, 8009);
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.5", 8008, {}).port, 8008);
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.5", 10001, {}).port, 8009);
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.5", 7000, {}).port, 8009);
  EXPECT_EQ(DeviceDiscovery::ParseFromMdnsData("", "10.0.0.5", 0, {}).port, 8009);
}

// ---------------------------------------------------------------------------
// ProcessMdnsResponse
// ---------------------------------------------------------------------------

TEST(DeviceDiscoveryTest, ProcessMdnsResponseLandsDeviceWithCoercedPort) {
  std::vector<uint8_t> pkt = MdnsHeader(0, 2, 0, 0);
  AppendSrvRecord(pkt, kInstance, 8081);  // non-control SRV port -> coerced to 8009
  AppendTxtRecord(pkt, kInstance,
                  {"id=uuid-mdns", "fn=Living Room TV", "md=Chromecast Ultra", "ca=5", "st=0"});

  DeviceDiscovery discovery;
  discovery.ProcessMdnsResponse(pkt.data(), pkt.size(), "192.168.7.7");

  auto devices = discovery.GetDevices();
  ASSERT_EQ(devices.size(), 1u);
  const CastDevice& dev = devices.front();
  EXPECT_EQ(dev.id, "uuid-mdns");
  EXPECT_EQ(dev.name, "Living Room TV");
  EXPECT_EQ(dev.model_name, "Chromecast Ultra");
  EXPECT_EQ(dev.ip_address, "192.168.7.7");
  EXPECT_EQ(dev.port, 8009);
  EXPECT_EQ(dev.capabilities, 5u);
  EXPECT_EQ(dev.status, DeviceStatus::kReady);
}

TEST(DeviceDiscoveryTest, ProcessMdnsResponseHonoursPort8008) {
  std::vector<uint8_t> pkt = MdnsHeader(0, 2, 0, 0);
  AppendSrvRecord(pkt, kInstance, 8008);
  AppendTxtRecord(pkt, kInstance, {"id=uuid-8008", "fn=Legacy", "ca=5"});

  DeviceDiscovery discovery;
  discovery.ProcessMdnsResponse(pkt.data(), pkt.size(), "192.168.7.8");

  auto devices = discovery.GetDevices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices.front().port, 8008);
}

TEST(DeviceDiscoveryTest, ProcessMdnsResponseIgnoresShortAndTruncatedInput) {
  DeviceDiscovery discovery;
  const uint8_t garbage[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  discovery.ProcessMdnsResponse(garbage, sizeof(garbage), "10.0.0.1");  // < 12 bytes: no-op
  EXPECT_TRUE(discovery.GetDevices().empty());

  std::vector<uint8_t> truncated = MdnsHeader(0, 1, 0, 0);
  AppendName(truncated, kInstance);  // name, then the record is cut off mid-header
  discovery.ProcessMdnsResponse(truncated.data(), truncated.size(), "10.0.0.1");
  EXPECT_TRUE(discovery.GetDevices().empty());

  // A record whose RDLENGTH points past the buffer must be dropped, not read.
  std::vector<uint8_t> lying = MdnsHeader(0, 1, 0, 0);
  AppendName(lying, kInstance);
  AppendU16(lying, 16);        // TXT
  AppendU16(lying, 0x8001);
  AppendU32(lying, 120);
  AppendU16(lying, 4096);      // RDLENGTH far beyond the payload
  lying.push_back(0x02);
  discovery.ProcessMdnsResponse(lying.data(), lying.size(), "10.0.0.1");
  EXPECT_TRUE(discovery.GetDevices().empty());
}

TEST(DeviceDiscoveryTest, ProcessMdnsResponseUsesFallbackScanWhenNoStructuredTxt) {
  // No TXT RR (ANCOUNT counts only the SRV), so the sliding-window TXT scan in
  // ProcessMdnsResponse must still recover the key/value strings.
  std::vector<uint8_t> pkt = MdnsHeader(0, 1, 0, 0);
  AppendSrvRecord(pkt, kInstance, 8009);
  // The fallback scan reads printable runs, so each key/value is NUL-terminated
  // exactly as a length-prefixed TXT string would be on the wire.
  const std::string raw = std::string("id=uuid-fallback") + '\0' + "fn=Patio" + '\0' +
                          "md=Chromecast" + '\0' + "ca=5" + '\0';
  pkt.insert(pkt.end(), raw.begin(), raw.end());

  DeviceDiscovery discovery;
  discovery.ProcessMdnsResponse(pkt.data(), pkt.size(), "192.168.7.9");

  auto devices = discovery.GetDevices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices.front().id, "uuid-fallback");
  EXPECT_EQ(devices.front().name, "Patio");
  EXPECT_EQ(devices.front().model_name, "Chromecast");
}

TEST(DeviceDiscoveryTest, ProcessMdnsResponseIgnoresQueriesAndNonCastTxt) {
  DeviceDiscovery discovery;

  // A query (QR bit clear) that happens to carry a TXT answer is not a device.
  std::vector<uint8_t> query = MdnsHeader(0, 2, 0, 0);
  query[2] = 0x00;
  query[3] = 0x00;
  AppendSrvRecord(query, kInstance, 8009);
  AppendTxtRecord(query, kInstance, {"id=uuid-q", "fn=Query Echo"});
  discovery.ProcessMdnsResponse(query.data(), query.size(), "192.168.7.20");
  EXPECT_TRUE(discovery.GetDevices().empty());

  // A response whose TXT carries neither id nor fn (a printer, say).
  std::vector<uint8_t> printer = MdnsHeader(0, 2, 0, 0);
  AppendSrvRecord(printer, kInstance, 8009);
  AppendTxtRecord(printer, kInstance, {"rp=ipp/print", "ty=Office Printer"});
  discovery.ProcessMdnsResponse(printer.data(), printer.size(), "192.168.7.21");
  EXPECT_TRUE(discovery.GetDevices().empty());
}

TEST(DeviceDiscoveryTest, ProcessMdnsResponsePrefersTheARecordAddress) {
  std::vector<uint8_t> pkt = MdnsHeader(0, 3, 0, 0);
  AppendSrvRecord(pkt, kInstance, 8009);
  AppendTxtRecord(pkt, kInstance, {"id=uuid-a", "fn=Bedroom"});
  AppendName(pkt, "Bedroom.local");
  AppendU16(pkt, 1);       // A
  AppendU16(pkt, 0x8001);
  AppendU32(pkt, 120);
  AppendU16(pkt, 4);
  for (uint8_t b : {uint8_t{192}, uint8_t{168}, uint8_t{7}, uint8_t{33}}) pkt.push_back(b);

  DeviceDiscovery discovery;
  discovery.ProcessMdnsResponse(pkt.data(), pkt.size(), "192.168.7.1");  // e.g. a reflector
  auto devices = discovery.GetDevices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices.front().ip_address, "192.168.7.33");
}