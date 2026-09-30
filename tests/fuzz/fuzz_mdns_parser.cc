// libFuzzer harness for the mDNS response parser in DeviceDiscovery — the code
// path that consumes untrusted packets from every host on the LAN.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "castcore/device_discovery.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (data == nullptr) return 0;
  using namespace castcore;

  // A fresh instance per input: ProcessMdnsResponse merges into the device list,
  // so reusing one object would grow memory across a long fuzzing run.
  DeviceDiscovery discovery;
  discovery.ProcessMdnsResponse(data, size, "192.0.2.1");

  // The TXT layer is reachable directly too; feed it a couple of shapes derived
  // from the same bytes so the field parser gets the same coverage.
  std::vector<std::string> entries;
  entries.emplace_back(reinterpret_cast<const char*>(data), size);
  entries.emplace_back();
  if (size > 1) {
    entries.emplace_back(reinterpret_cast<const char*>(data + 1), size - 1);
  }
  DeviceDiscovery::ParseTxtRecord(entries);

  return 0;
}
