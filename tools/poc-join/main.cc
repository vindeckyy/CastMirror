#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace castcore;

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: PoC 0D - End-to-End Join    ";
  LOG_INFO << "===========================================";

  auto& engine = CastEngine::Instance();
  engine.Initialize();

  engine.SetOnStateChanged([](SessionState old_s, SessionState new_s, const std::string& msg) {
    LOG_INFO << "Engine State Changed: " << SessionStateToString(old_s) << " -> "
             << SessionStateToString(new_s) << " (" << msg << ")";
  });

  std::string target_device_ip = "127.0.0.1";
  uint16_t target_device_port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 8009;

  if (argc > 1) {
    target_device_ip = argv[1];
    CastDevice d;
    d.id = "target-dev";
    d.name = "Target Cast Device (" + target_device_ip + ")";
    d.model_name = "Chromecast Ultra";
    d.ip_address = target_device_ip;
    d.port = target_device_port;
    engine.GetDiscovery().AddOrUpdateDevice(d);
  } else {
    LOG_INFO << "Scanning LAN for Cast devices (3 seconds)...";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    auto devices = engine.GetDevices();
    if (!devices.empty()) {
      target_device_ip = devices[0].ip_address;
      LOG_INFO << "Auto-selected device: " << devices[0].name << " (" << target_device_ip << ")";
    } else {
      LOG_WARN << "No Cast device discovered on LAN. Using loopback 127.0.0.1";
      target_device_ip = "127.0.0.1";
      CastDevice loopback_dev;
      loopback_dev.id = "loopback-1";
      loopback_dev.name = "Simulated Loopback Cast Receiver";
      loopback_dev.model_name = "Chromecast Ultra";
      loopback_dev.ip_address = "127.0.0.1";
      loopback_dev.port = target_device_port;
      engine.GetDiscovery().AddOrUpdateDevice(loopback_dev);
    }
  }

  LOG_INFO << "Initiating End-to-End Display Cast to " << target_device_ip << "...";
  bool started = engine.StartCasting(target_device_ip, 0, QualityPreset::kBalanced, true);

  if (started) {
    LOG_INFO << "Cast Session Active! Streaming live display for 5 seconds...";
    for (int i = 0; i < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto stats = engine.GetStats();
      LOG_INFO << "[LIVE STATS] FPS: " << stats.current_fps
               << ", Bitrate: " << stats.bitrate_kbps << " kbps"
               << ", RTT: " << stats.round_trip_time_ms << " ms"
               << ", Loss: " << (stats.packet_loss_fraction * 100.0) << "%";
    }

    LOG_INFO << "Stopping Cast Session (hard 500ms budget)...";
    auto t0 = std::chrono::steady_clock::now();
    engine.StopCasting();
    auto t1 = std::chrono::steady_clock::now();
    double stop_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG_INFO << "Stop completed in " << stop_ms << " ms (Budget <= 500ms: "
             << (stop_ms <= 500.0 ? "PASSED" : "WARNING") << ")";
  } else {
    LOG_ERROR << "Failed to start Cast session.";
  }

  engine.Shutdown();
  return started ? 0 : 1;
}
