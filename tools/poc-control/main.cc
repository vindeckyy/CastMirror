#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include "castcore/cast_channel.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace castcore;

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "  CastMirror: PoC 0A - Control Plane Test  ";
  LOG_INFO << "===========================================";

  DeviceDiscovery discovery;
  discovery.Start();

  std::string target_ip;
  if (argc > 1) {
    target_ip = argv[1];
  } else {
    LOG_INFO << "Scanning for Cast devices on LAN for 3 seconds...";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    auto devices = discovery.GetDevices();
    if (devices.empty()) {
      LOG_WARN << "No Cast devices discovered automatically via mDNS.";
      LOG_INFO << "Using default fallback loopback 127.0.0.1 (or pass IP as argument)";
      target_ip = "127.0.0.1";
    } else {
      LOG_INFO << "Found " << devices.size() << " device(s):";
      for (size_t i = 0; i < devices.size(); ++i) {
        LOG_INFO << " [" << i << "] " << devices[i].name << " (" << devices[i].model_name
                 << ") at " << devices[i].ip_address << ":" << devices[i].port;
      }
      target_ip = devices[0].ip_address;
    }
  }

  uint16_t target_port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 8009;
  LOG_INFO << "Targeting Cast Device: " << target_ip << ":" << target_port;

  CastChannel channel;
  channel.SetMessageCallback([](const std::string& ns, const std::string& payload,
                                const std::string& src, const std::string& dest) {
    LOG_INFO << "[RECV] NS: " << ns << " from: " << src << " to: " << dest;
    LOG_INFO << "       Payload: " << payload;
  });

  if (!channel.Connect(target_ip, target_port)) {
    LOG_ERROR << "Failed to connect to " << target_ip << ":" << target_port << " (ensure device is on or fake-receiver is running)";
    return 1;
  }

  LOG_INFO << "Requesting Receiver Status...";
  channel.RequestReceiverStatus();
  std::this_thread::sleep_for(std::chrono::seconds(1));

  LOG_INFO << "Launching Mirroring App 0F5096E8...";
  channel.LaunchApp(kMirroringAudioVideoAppId);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  LOG_INFO << "PoC 0A Control Plane test passed successfully!";
  channel.Disconnect();
  discovery.Stop();
  return 0;
}
