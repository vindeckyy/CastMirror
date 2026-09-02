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
            << "  --window <ID>           Share a single window by ID (use --list-windows to find IDs)\n"
            << "  --list-windows          List available windows and exit\n"
            << "  --list-displays         List available displays and exit\n"
            << "  --preset <preset>       Quality preset: Auto, High, Balanced, Smooth (default: Auto)\n"
            << "  --no-audio              Disable audio mirroring\n"
            << "  --bitrate <kbps>        Custom video bitrate in kbps\n"
            << "  --codec <h264|vp8>      Select video codec (h264 or vp8, default: h264)\n"
            << "  --low-latency           Force 200ms target playout delay\n"
            << "  --no-verify             Bypass Cast device certificate verification (dev escape hatch)\n"
            << "  --help                  Show this help message\n\n";
}

int main(int argc, char** argv) {
  std::string target_device_arg;
  int display_id_arg = 0;
  std::string window_id_arg;
  bool list_windows = false;
  bool list_displays = false;
  QualityPreset preset_arg = QualityPreset::kAuto;
  VideoCodec video_codec_arg = VideoCodec::kH264;
  bool audio_enabled_arg = true;
  uint32_t bitrate_arg = 0;
  int target_delay_arg = 0;
  bool verify_device_cert = true;
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
      try { display_id_arg = std::stoi(argv[++i]); } catch (...) {}
    } else if (arg == "--window" && i + 1 < argc) {
      window_id_arg = argv[++i];
      interactive_mode = false;
    } else if (arg == "--list-windows") {
      list_windows = true;
    } else if (arg == "--list-displays") {
      list_displays = true;
    } else if (arg == "--preset" && i + 1 < argc) {
      preset_arg = QualityPresetFromString(argv[++i]);
    } else if (arg == "--no-audio") {
      audio_enabled_arg = false;
    } else if (arg == "--bitrate" && i + 1 < argc) {
      try { bitrate_arg = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    } else if (arg == "--codec" && i + 1 < argc) {
      std::string c = argv[++i];
      if (c == "vp8" || c == "VP8") {
        video_codec_arg = VideoCodec::kVP8;
      } else {
        video_codec_arg = VideoCodec::kH264;
      }
    } else if (arg == "--low-latency") {
      target_delay_arg = 200;
    } else if (arg == "--no-verify") {
      verify_device_cert = false;
    }
  }

  PrintBanner();

  auto& engine = CastEngine::Instance();
  engine.Initialize();

  // --list-windows / --list-displays: print and exit.
  if (list_windows) {
    auto windows = engine.GetWindows();
    if (windows.empty()) {
      std::cout << "No windows available (window capture may be unsupported on this backend).\n";
    } else {
      std::cout << "\n\033[1;33m--- Available Windows ---\033[0m\n";
      for (const auto& w : windows) {
        std::cout << "  [" << w.id << "] \033[1m" << w.title << "\033[0m";
        if (!w.app_class.empty()) std::cout << "  \033[36m" << w.app_class << "\033[0m";
        if (w.width > 0 && w.height > 0) std::cout << "  " << w.width << "x" << w.height;
        std::cout << "\n";
      }
    }
    engine.Shutdown();
    return 0;
  }
  if (list_displays) {
    auto displays = engine.GetDisplays();
    std::cout << "\n\033[1;33m--- Available Displays ---\033[0m\n";
    for (const auto& d : displays) {
      std::cout << "  [" << d.id << "] " << d.name << " (" << d.width << "x" << d.height
                << " @ " << d.refresh_rate << "Hz)" << (d.is_primary ? " [Primary]" : "") << "\n";
    }
    engine.Shutdown();
    return 0;
  }

  if (!interactive_mode && !target_device_arg.empty()) {
    // --window and --display are mutually exclusive.
    if (!window_id_arg.empty()) {
      int win_id = 0;
      try { win_id = std::stoi(window_id_arg); } catch (...) {
        std::cerr << "Invalid window ID: " << window_id_arg << "\n";
        engine.Shutdown();
        return 1;
      }
      // Resolve window title for stats/persistence.
      std::string win_title;
      for (const auto& w : engine.GetWindows()) {
        if (w.id == win_id) { win_title = w.title; break; }
      }
      CaptureSource source{CaptureSourceKind::kWindow, win_id, win_title};
      std::cout << "Initiating Cast of window [" << win_id << "]"
                << (win_title.empty() ? "" : (" (" + win_title + ")"))
                << " to " << target_device_arg << "...\n";
      SessionOptions opts;
      opts.preset = preset_arg;
      opts.enable_audio = audio_enabled_arg;
      opts.video_codec = video_codec_arg;
      opts.video_bitrate_kbps = bitrate_arg;
      if (target_delay_arg > 0) opts.target_delay_ms = target_delay_arg;
      opts.verify_device_cert = verify_device_cert;
      bool ok = engine.StartCasting(target_device_arg, source, opts);
      if (!ok) {
        std::cerr << "Failed to start casting to " << target_device_arg << "\n";
        engine.Shutdown();
        return 1;
      }
    } else {
      std::cout << "Initiating Cast to " << target_device_arg << "...\n";
      SessionOptions opts;
      opts.preset = preset_arg;
      opts.enable_audio = audio_enabled_arg;
      opts.video_codec = video_codec_arg;
      opts.video_bitrate_kbps = bitrate_arg;
      if (target_delay_arg > 0) opts.target_delay_ms = target_delay_arg;
      opts.verify_device_cert = verify_device_cert;
      bool ok = engine.StartCasting(target_device_arg, display_id_arg, opts);
      if (!ok) {
        std::cerr << "Failed to start casting to " << target_device_arg << "\n";
        engine.Shutdown();
        return 1;
      }
    }

    std::cout << "Streaming. Press Ctrl+C or Enter to stop...\n";
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

    // Show windows if the backend supports window capture.
    auto windows = engine.GetWindows();
    if (engine.WindowCaptureSupported() && !windows.empty()) {
      std::cout << "\n\033[1;33m--- Windows ---\033[0m\n";
      for (const auto& w : windows) {
        std::cout << "  Window " << w.id << ": " << w.title;
        if (!w.app_class.empty()) std::cout << " (" << w.app_class << ")";
        std::cout << "\n";
      }
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
    if (engine.WindowCaptureSupported() && !windows.empty()) {
      std::cout << "  [W] Cast a Window (pick from the list above)\n";
    }
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
    } else if (input == "W" || input == "w") {
      if (!engine.WindowCaptureSupported() || windows.empty()) {
        std::cout << "Window capture is not available on this backend.\n";
        continue;
      }
      std::cout << "Enter window ID: ";
      std::string win_input;
      std::cin >> win_input;
      try {
        int win_id = std::stoi(win_input);
        std::string win_title;
        for (const auto& w : windows) {
          if (w.id == win_id) { win_title = w.title; break; }
        }
        if (win_title.empty()) {
          std::cout << "Window ID " << win_id << " not found.\n";
          continue;
        }
        if (devices.empty()) {
          std::cout << "No Cast device selected. Use [1-N] or [A] first.\n";
          continue;
        }
        // Use the first device (or last device if set).
        std::string dev_id = !cfg.last_device_id.empty() ? cfg.last_device_id : devices[0].id;
        CaptureSource source{CaptureSourceKind::kWindow, win_id, win_title};
        std::cout << "\nCasting window '" << win_title << "' to " << dev_id << "...\n";
        SessionOptions opts;
        opts.preset = cfg.quality_preset;
        opts.enable_audio = cfg.audio_enabled;
        if (engine.StartCasting(dev_id, source, opts)) {
          std::cout << "\n\033[1;32m[LIVE] Mirroring window! Press Enter to stop casting.\033[0m\n";
          std::cin.ignore();
          std::cin.get();
          engine.StopCasting();
        }
      } catch (...) {
        std::cout << "Invalid window ID.\n";
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
