#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

using namespace castcore;

static void PrintHelp(const char* prog) {
  std::cout << "Usage: " << prog << " [options] [ip] [port] [duration]\n"
            << "PoC 0D - End-to-End Join (Cast to fake-receiver or real device)\n"
            << "\nPositional (legacy):\n"
            << "  ip                 target Cast device IP (default 127.0.0.1)\n"
            << "  port               TLS port (default 8009)\n"
            << "  duration           streaming duration seconds (default 5, 0=infinite)\n"
            << "\nOptions:\n"
            << "  --ip IP            same as positional ip\n"
            << "  --port PORT        same as positional port\n"
            << "  --duration SEC     same as positional duration\n"
            << "  --json PATH        write JSON metrics to PATH\n"
            << "  --help, -h         show this help\n"
            << "\nExamples:\n"
            << "  " << prog << " 127.0.0.1 28009 10\n"
            << "  " << prog << " --ip 127.0.0.1 --port 28009 --duration 10\n"
            << "  " << prog << " 127.0.0.1 8009 5\n";
}

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: PoC 0D - End-to-End Join    ";
  LOG_INFO << "===========================================";

  // Check for --help early (avoid treating --help as IP)
  for (int i=1;i<argc;++i) {
    std::string a=argv[i];
    if (a=="--help" || a=="-h") { PrintHelp(argv[0]); return 0; }
  }

  std::string target_device_ip = "127.0.0.1";
  uint16_t target_device_port = 8009;
  int duration_sec = 5;
  std::string json_path;
  bool ip_set = false;
  bool port_set = false;
  bool duration_set = false;

  // Parse options and positionals
  std::vector<std::string> positionals;
  for (int i=1;i<argc;++i) {
    std::string arg = argv[i];
    if (arg=="--ip" && i+1<argc) {
      target_device_ip = argv[++i];
      ip_set = true;
    } else if (arg=="--port" && i+1<argc) {
      try { target_device_port = static_cast<uint16_t>(std::stoi(argv[++i])); port_set=true; } catch(...) {}
    } else if ((arg=="--duration" || arg=="-d") && i+1<argc) {
      try { duration_sec = std::stoi(argv[++i]); duration_set=true; } catch(...) {}
    } else if (arg=="--json" && i+1<argc) {
      json_path = argv[++i];
    } else if (arg.rfind("--",0)==0) {
      LOG_WARN << "Unknown option: " << arg;
    } else {
      positionals.push_back(arg);
    }
  }
  // Legacy positional fallback: [ip] [port] [duration]
  // Only apply if not already set via flags
  if (!ip_set && positionals.size() >= 1) {
    target_device_ip = positionals[0];
    ip_set = true;
  }
  if (!port_set && positionals.size() >= 2) {
    try { target_device_port = static_cast<uint16_t>(std::stoi(positionals[1])); } catch(...) {}
  }
  if (!duration_set && positionals.size() >= 3) {
    try { duration_sec = std::stoi(positionals[2]); } catch(...) {}
  }
  // Also handle case where port/duration given as single positional that is numeric and ip not set?
  // e.g., "poc-join 28009" should not be treated as IP; but legacy always had IP first, so keep.

  // Also handle stray duration like "10s"
  // If duration string had trailing 's', stoi will parse prefix anyway

  auto& engine = CastEngine::Instance();
  engine.Initialize();

  engine.SetOnStateChanged([](SessionState old_s, SessionState new_s, const std::string& msg) {
    LOG_INFO << "Engine State Changed: " << SessionStateToString(old_s) << " -> "
             << SessionStateToString(new_s) << " (" << msg << ")";
  });

  if (ip_set) {
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

  LOG_INFO << "Initiating End-to-End Display Cast to " << target_device_ip << ":" << target_device_port << "...";
  bool started = engine.StartCasting(target_device_ip, 0, QualityPreset::kBalanced, true);

  int effective_duration = duration_sec;

  double stop_ms = 0.0;
  if (started) {
    LOG_INFO << "Cast Session Active! Streaming live display for " << effective_duration << " seconds...";
    for (int i = 0; (effective_duration <= 0 || i < effective_duration) && engine.GetState() == SessionState::kStreaming; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto stats = engine.GetStats();
      LOG_INFO << "[LIVE STATS] FPS: " << stats.current_fps
               << ", Bitrate: " << stats.bitrate_kbps << " kbps"
               << ", RTT: " << stats.round_trip_time_ms << " ms"
               << ", Loss: " << (stats.packet_loss_fraction * 100.0) << "%"
               << ", Packets: " << stats.packets_sent
               << ", Frames: " << stats.frames_sent;
    }

    LOG_INFO << "Stopping Cast Session (hard 500ms budget)...";
    auto t0 = std::chrono::steady_clock::now();
    engine.StopCasting();
    auto t1 = std::chrono::steady_clock::now();
    stop_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG_INFO << "Stop completed in " << stop_ms << " ms (Budget <= 500ms: "
             << (stop_ms <= 500.0 ? "PASSED" : "WARNING") << ")";
  } else {
    LOG_ERROR << "Failed to start Cast session.";
  }

  // Gather final stats for bench harness
  StreamStats final_stats = engine.GetStats();
  // If we failed, try to get transport stats if available else zeros
  double udp_pps = 0.0;
  if (effective_duration > 0) {
    udp_pps = final_stats.packets_sent / static_cast<double>(effective_duration);
  } else if (final_stats.packets_sent > 0) {
    udp_pps = static_cast<double>(final_stats.packets_sent);
  }
  double rtt_ms = final_stats.round_trip_time_ms;
  double loss_fraction = final_stats.packet_loss_fraction;
  double fps = final_stats.current_fps;

  LOG_INFO << "BENCH_METRIC udp_pps=" << udp_pps << " rtt_ms=" << rtt_ms
           << " loss_fraction=" << loss_fraction << " stop_ms=" << stop_ms
           << " fps=" << fps << " packets=" << final_stats.packets_sent
           << " frames=" << final_stats.frames_sent;
  LOG_INFO << "BENCH_JOIN_CSV " << udp_pps << "," << rtt_ms << "," << loss_fraction << "," << stop_ms << "," << fps;
  std::cout << "BENCH_JOIN udp_pps=" << udp_pps << " rtt_ms=" << rtt_ms
            << " loss_fraction=" << loss_fraction << " stop_ms=" << stop_ms
            << " fps=" << fps << " packets=" << final_stats.packets_sent << " frames=" << final_stats.frames_sent << std::endl;

  if (!json_path.empty()) {
    try {
      FILE* f = fopen(json_path.c_str(), "w");
      if (f) {
        fprintf(f, "{\"ip\":\"%s\",\"port\":%u,\"duration_sec\":%d,"
                   "\"udp_pps\":%.3f,\"rtt_ms\":%.3f,\"loss_fraction\":%.6f,"
                   "\"stop_ms\":%.3f,\"fps\":%.3f,\"packets_sent\":%u,\"frames_sent\":%u}\n",
                target_device_ip.c_str(), target_device_port, effective_duration,
                udp_pps, rtt_ms, loss_fraction, stop_ms, fps,
                final_stats.packets_sent, final_stats.frames_sent);
        fclose(f);
        LOG_INFO << "Wrote JSON metrics to " << json_path;
      }
    } catch(...) {}
  }

  engine.Shutdown();
  return started ? 0 : 1;
}
