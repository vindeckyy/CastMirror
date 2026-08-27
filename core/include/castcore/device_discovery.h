#ifndef CASTCORE_DEVICE_DISCOVERY_H_
#define CASTCORE_DEVICE_DISCOVERY_H_

#include "castcore/types.h"
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
  void AddOrUpdateDevice(const CastDevice& device);
  void RemoveDevice(const std::string& device_id);

  // Utility to parse TXT record key-value pairs
  static std::map<std::string, std::string> ParseTxtRecord(const std::vector<std::string>& txt_entries);
  static CastDevice ParseFromMdnsData(const std::string& name,
                                      const std::string& ip,
                                      uint16_t port,
                                      const std::vector<std::string>& txt_entries);

 private:
  void DiscoveryLoop();
  void SendMdnsQuery(int socket_fd);
  void ProcessMdnsResponse(const uint8_t* buffer, size_t length, const std::string& sender_ip);
  void ProbeLocalSubnets();

  std::atomic<bool> running_{false};
  std::thread discovery_thread_;
  std::thread subnet_thread_;
  std::atomic<bool> force_mdns_query_{false};
  mutable std::mutex mutex_;
  std::vector<CastDevice> devices_;
  DevicesCallback callback_;
};

} // namespace castcore

#endif // CASTCORE_DEVICE_DISCOVERY_H_
