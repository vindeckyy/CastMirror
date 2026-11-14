#ifndef CASTCORE_DEVICE_DISCOVERY_H_
#define CASTCORE_DEVICE_DISCOVERY_H_

#include "castcore/types.h"
#include <cstddef>
#include <set>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <map>

namespace castcore {

class DeviceDiscovery {
 public:
  using DevicesCallback = std::function<void(const std::vector<CastDevice>& devices)>;

  DeviceDiscovery();
  ~DeviceDiscovery();

  bool Start();
  void Stop();
  bool IsRunning() const;

  void TriggerScan();

  std::vector<CastDevice> GetDevices() const;
  std::optional<CastDevice> FindDeviceById(const std::string& id) const;
  std::optional<CastDevice> FindDeviceByIp(const std::string& ip) const;

  void SetCallback(DevicesCallback callback);

  // Manual / Mock device registration (useful for tests and static LAN configs)
  // from_mdns is true when the device came from an mDNS TXT record: I trust
  // that id over a cloud id from the subnet probe, so it can replace one.
  void AddOrUpdateDevice(const CastDevice& device, bool from_mdns = false);
  void RemoveDevice(const std::string& device_id);

  // I call an id UUID style when it looks like a real Cast id: hex digits and
  // dashes only, at least 8 hex digits, and not an IPv4 address. A bare cloud
  // id or a short label does not count, so I never mistake one for a UUID.
  static bool IsIpAddress(const std::string& value);
  static bool IsUuidStyleId(const std::string& value);

  // I stop the periodic mDNS re-query once the list has been unchanged for
  // kSettleUnchangedRounds query rounds with at least one device found.
  // Rounds are 3 seconds apart, so that is about 30 seconds of stability.
  // I still listen passively after that, and TriggerScan starts me up again.
  static constexpr int kSettleUnchangedRounds = 10;
  static bool ShouldSettleDiscovery(int unchanged_rounds, std::size_t device_count);

  // Utility to parse TXT record key-value pairs
  static std::map<std::string, std::string> ParseTxtRecord(
      const std::vector<std::string>& txt_entries);
  static CastDevice ParseFromMdnsData(const std::string& name, const std::string& ip, uint16_t port,
                                      const std::vector<std::string>& txt_entries);

  // Parse one mDNS response packet and merge any device it describes into the
  // device list. Public so this parser — which consumes untrusted LAN input —
  // can be unit-tested directly. Normally only reached from DiscoveryLoop().
  void ProcessMdnsResponse(const uint8_t* buffer, size_t length, const std::string& sender_ip);

  // Drops devices that mDNS found but has not heard from within `ttl`, so a TV that
  // was switched off leaves the list. Devices added by address or by the subnet
  // probe are kept: nothing re-announces them. Returns how many were removed.
  size_t ExpireStaleDevices(std::chrono::steady_clock::time_point now, std::chrono::seconds ttl);

 private:
  void DiscoveryLoop();
  void SendMdnsQuery(int socket_fd);
  void ProbeLocalSubnets();

  std::atomic<bool> running_{false};
  std::thread discovery_thread_;
  std::thread subnet_thread_;
  std::atomic<bool> force_mdns_query_{false};
  mutable std::mutex mutex_;
  std::vector<CastDevice> devices_;
  std::set<std::string> mdns_ids_;  // ids seen through mDNS; guarded by mutex_
  DevicesCallback callback_;
};

}  // namespace castcore

#endif  // CASTCORE_DEVICE_DISCOVERY_H_
