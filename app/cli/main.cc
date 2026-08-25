#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

using namespace castcore;

void PrintBanner() {
  std::cout << "\033[1;36m";
  std::cout << "=================================================================\n";
  std::cout << "        CastMirror - Native Chromecast Display Mirroring         \n";
  std::cout << "=================================================================\n";
  std::cout << "\033[0m";
}

void PrintHelp() {
  std::cout << "Usage: castmirror [options]\n\n"
            << "Options:\n"
            << "  --device <IP or ID>     Target Cast device IP or UUID\n"
            << "  --display <ID>          Display/Monitor index (default: 0)\n"
            << "  --preset <preset>       Quality preset: Auto, High, Balanced, Smooth (default: Auto)\n"
            << "  --no-audio              Disable audio mirroring\n"
            << "  --low-latency           Force 200ms target playout delay\n"
            << "  --help                  Show this help message\n\n";
}

int main(int argc, char** argv) {
  std::string target_device_arg;
  int display_id_arg = 0;
  QualityPreset preset_arg = QualityPreset::kAuto;
  bool audio_enabled_arg = true;
  bool interactive_mode = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintHelp();
      return 0;
    } else if (arg == "--device" && i + 1 < argc) {
      target_device_arg = argv[++i];
      interactive_mode = false;
    } else if (arg == "--display" && i + 1 < argc) {
      display_id_arg = std::stoi(argv[++i]);
    } else if (arg == "--preset" && i + 1 < argc) {
      preset_arg = QualityPresetFromString(argv[++i]);
    } else if (arg == "--no-audio") {
      audio_enabled_arg = false;
    }
  }

  PrintBanner();

  auto& engine = CastEngine::Instance();
  engine.Initialize();

  if (!interactive_mode && !target_device_arg.empty()) {
    std::cout << "Initiating Cast to " << target_device_arg << "...\n";
    bool ok = engine.StartCasting(target_device_arg, display_id_arg, preset_arg, audio_enabled_arg);
    if (!ok) {
      std::cerr << "Failed to start casting to " << target_device_arg << "\n";
      engine.Shutdown();
      return 1;
    }

    std::cout << "Streaming display. Press Ctrl+C or Enter to stop...\n";
    while (engine.GetState() == SessionState::kStreaming) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto stats = engine.GetStats();
      std::cout << "\r[LIVE] FPS: " << std::fixed << std::setprecision(1) << stats.current_fps
                << " | Bitrate: " << (stats.bitrate_kbps / 1000.0) << " Mbps"
                << " | RTT: " << stats.round_trip_time_ms << " ms"
                << " | Loss: " << (stats.packet_loss_fraction * 100.0) << "%"
                << " | Target Delay: " << stats.target_delay_ms << "ms" << std::flush;
    }
    std::cout << "\nStopping Cast session...\n";
    engine.StopCasting();
    engine.Shutdown();
    return 0;
  }

  // Interactive TUI / Menu Loop
  std::cout << "Scanning for Google Cast devices on your network...\n";
  engine.StartDiscovery();
  std::this_thread::sleep_for(std::chrono::seconds(2));

  while (true) {
    auto devices = engine.GetDevices();
    auto displays = engine.GetDisplays();
    const auto& cfg = engine.GetConfig();

    std::cout << "\n\033[1;33m--- Discovered Cast Devices ---\033[0m\n";
    if (devices.empty()) {
      std::cout << "  (No devices found yet. Press [R] to rescan or [A] to add manual IP)\n";
    } else {
      for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << devices[i].name
                  << " \033[32m(" << devices[i].model_name << ")\033[0m"
                  << " @ " << devices[i].ip_address
                  << " [" << DeviceStatusToString(devices[i].status) << "]\n";
      }
    }

    std::cout << "\n\033[1;33m--- Displays ---\033[0m\n";
    for (const auto& d : displays) {
      std::cout << "  Display " << d.id << ": " << d.name << " (" << d.width << "x" << d.height
                << " @ " << d.refresh_rate << "Hz)" << (d.is_primary ? " [Primary]" : "") << "\n";
    }

    std::cout << "\n\033[1;33m--- Current Settings ---\033[0m\n";
    std::cout << "  Quality Preset : " << QualityPresetToString(cfg.quality_preset) << "\n";
    std::cout << "  Audio Mirroring: " << (cfg.audio_enabled ? "Enabled" : "Disabled") << "\n";
    std::cout << "  Target Delay   : " << cfg.target_delay_ms << " ms\n";
    if (!cfg.last_device_name.empty()) {
      std::cout << "  Last Device    : " << cfg.last_device_name << " (" << cfg.last_device_ip << ")\n";
    }

    std::cout << "\n\033[1;32mActions:\033[0m\n";
    std::cout << "  [1-" << std::max<size_t>(1, devices.size()) << "] Select device & Cast\n";
    std::cout << "  [L] Cast to Last Device (" << (cfg.last_device_name.empty() ? "None" : cfg.last_device_name) << ")\n";
    std::cout << "  [P] Change Quality Preset (Auto / High / Balanced / Smooth)\n";
    std::cout << "  [M] Toggle Audio Mirroring\n";
    std::cout << "  [A] Add Device by IP manually\n";
    std::cout << "  [R] Refresh / Rescan\n";
    std::cout << "  [Q] Quit\n";
    std::cout << "Choose an option: " << std::flush;

    std::string input;
    if (!(std::cin >> input)) break;

    if (input == "Q" || input == "q") {
      break;
    } else if (input == "R" || input == "r") {
      engine.GetDiscovery().TriggerScan();
      std::this_thread::sleep_for(std::chrono::seconds(1));
    } else if (input == "P" || input == "p") {
      auto& mcfg = ConfigStore::Instance().Mutable();
      if (mcfg.quality_preset == QualityPreset::kAuto) mcfg.quality_preset = QualityPreset::kHigh;
      else if (mcfg.quality_preset == QualityPreset::kHigh) mcfg.quality_preset = QualityPreset::kBalanced;
      else if (mcfg.quality_preset == QualityPreset::kBalanced) mcfg.quality_preset = QualityPreset::kSmooth;
      else mcfg.quality_preset = QualityPreset::kAuto;
      ConfigStore::Instance().Save();
    } else if (input == "M" || input == "m") {
      auto& mcfg = ConfigStore::Instance().Mutable();
      mcfg.audio_enabled = !mcfg.audio_enabled;
      ConfigStore::Instance().Save();
    } else if (input == "A" || input == "a") {
      std::cout << "Enter Cast device IP address: ";
      std::string manual_ip;
      std::cin >> manual_ip;
      if (!manual_ip.empty()) {
        CastDevice d;
        d.id = manual_ip;
        d.name = "Custom TV (" + manual_ip + ")";
        d.model_name = "Cast Device";
        d.ip_address = manual_ip;
        d.port = 8009;
        engine.GetDiscovery().AddOrUpdateDevice(d);
      }
    } else if (input == "L" || input == "l") {
      std::cout << "\nConnecting to last device...\n";
      if (engine.StartCastingLastDevice()) {
        std::cout << "\n\033[1;32m[LIVE] Mirroring active! Press Enter to stop casting.\033[0m\n";
        std::cin.ignore();
        std::cin.get();
        engine.StopCasting();
      }
    } else {
      try {
        size_t idx = std::stoul(input);
        if (idx >= 1 && idx <= devices.size()) {
          const auto& dev = devices[idx - 1];
          std::cout << "\nConnecting to " << dev.name << "...\n";
          if (engine.StartCasting(dev.id, cfg.last_display_id, cfg.quality_preset, cfg.audio_enabled)) {
            std::cout << "\n\033[1;32m[LIVE] Mirroring active! Press Enter to stop casting.\033[0m\n";
            std::cin.ignore();
            std::cin.get();
            engine.StopCasting();
          }
        }
      } catch (...) {}
    }
  }

  engine.Shutdown();
  std::cout << "Goodbye!\n";
  return 0;
}
