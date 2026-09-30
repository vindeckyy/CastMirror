#include "castcore/cast_engine.h"
#include "castcore/cast_channel.h"
#include "castcore/config.h"
#include "castcore/device_auth.h"
#include "castcore/logger.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

using namespace castcore;

namespace {

// Exit status contract (documented in castmirror.1):
//   0  the requested work completed
//   1  the cast could not be started / a connection failed
//   2  the command line was not understood
//   3  the session started but ended in SessionState::kFailed
constexpr int kExitUsage = 2;
constexpr int kExitSessionFailed = 3;

std::atomic<bool> g_interrupted{false};

#if defined(_WIN32)
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      // Ask the session to stop instead of letting the default handler kill
      // the process: an abrupt exit skips StopCasting() and the config save in
      // Shutdown(), leaving the receiver to time the session out.
      g_interrupted.store(true);
      return TRUE;
    default: return FALSE;
  }
}
#endif

void InstallConsoleInterruptHandler() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
  std::signal(SIGINT, [](int) { g_interrupted.store(true); });
  std::signal(SIGTERM, [](int) { g_interrupted.store(true); });
#endif
}

// True once the user asked to stop, either with Ctrl+C or by pressing Enter.
// Non-blocking, so the caller's status line keeps refreshing while waiting.
bool StopRequested() {
  if (g_interrupted.load()) return true;
#if defined(_WIN32)
  while (_kbhit()) {
    const int ch = _getch();
    if (ch == '\r' || ch == '\n') return true;
  }
#else
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(STDIN_FILENO, &read_set);
  timeval timeout{0, 0};
  if (select(STDIN_FILENO + 1, &read_set, nullptr, nullptr, &timeout) > 0) {
    char buffer[64];
    if (::read(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {
      for (char c : buffer) {
        if (c == '\n' || c == '\r') return true;
      }
    }
  }
#endif
  return false;
}

[[noreturn]] void UsageError(const std::string& message) {
  std::cerr << "castmirror: " << message << "\n"
            << "Run 'castmirror --help' for the list of options.\n";
  std::exit(kExitUsage);
}

// Strict integer parse: rejects trailing junk, signs (when min_value >= 0) and
// anything outside [min_value, max_value]. The previous std::stoul/stoi calls
// swallowed every failure, so `--display abc` cast the wrong monitor and
// `--bitrate -1` wrapped around to 4294967295 kbps.
bool ParseInteger(const std::string& text, long long min_value, long long max_value,
                  long long* out) {
  if (text.empty()) return false;
  long long value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) return false;
  if (value < min_value || value > max_value) return false;
  *out = value;
  return true;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto l = static_cast<unsigned char>(a[i]);
    const auto r = static_cast<unsigned char>(b[i]);
    if (std::tolower(l) != std::tolower(r)) return false;
  }
  return true;
}

}  // namespace

void PrintBanner() {
  std::cout << "\033[1;36m";
  std::cout << "=================================================================\n";
  std::cout << "        CastMirror - Native Chromecast Display Mirroring         \n";
  std::cout << "=================================================================\n";
  std::cout << "\033[0m";
}

void PrintHelp() {
  std::cout
      << "Usage: castmirror [options]\n\n"
      << "Options:\n"
      << "  --device <IP or ID>     Target Cast device IP or UUID\n"
      << "  --display <ID>          Display/Monitor index (default: 0)\n"
      << "  --window <ID>           Share a single window by ID (use --list-windows to find IDs)\n"
      << "  --list-windows          List available windows and exit\n"
      << "  --list-displays         List available displays and exit\n"
      << "  --preset <preset>       Quality preset: Auto, High, Balanced, Smooth, Game, Cinema "
         "(default: Auto)\n"
      << "  --no-audio              Disable audio mirroring\n"
      << "  --bitrate <kbps>        Custom video bitrate in kbps\n"
      << "  --codec <h264|vp8>      Select video codec (h264 or vp8, default: h264)\n"
      << "  --low-latency           Force 200ms target playout delay\n"
      << "  --no-verify             Bypass Cast device certificate verification (dev escape "
         "hatch)\n"
      << "  --auth-probe <IP>       Connect to a device and run the device-auth challenge, then "
         "exit\n"
      << "  --help                  Show this help message\n\n";
}

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Window capture maps GDI window and cursor rectangles onto DXGI output
  // rectangles and the duplicated image, both of which are in physical pixels.
  // A DPI-unaware process gets virtualized rects instead, so on a scaled
  // display the crop is wrong and the cursor is drawn at the wrong offset. The
  // WinUI client declares PerMonitorV2 in app.manifest; the console tool has no
  // manifest, so opt in here, before any window or cursor API runs.
  if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    SetProcessDPIAware();  // Windows 8.1 and earlier: system-DPI awareness.
  }
#endif

  std::string target_device_arg;
  std::string auth_probe_arg;
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

  // Only values the user actually typed become overrides. Everything else comes
  // from the saved configuration (see BuildSessionOptions), because the engine
  // persists a session's options as the user's new defaults: a flag that
  // silently defaulted here used to overwrite a saved setting.
  bool preset_given = false;
  bool codec_given = false;
  bool audio_given = false;
  bool bitrate_given = false;
  bool delay_given = false;
  bool verify_given = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value_for = [&](const char* name) -> std::string {
      if (i + 1 >= argc) UsageError(std::string(name) + " requires a value");
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintHelp();
      return 0;
    } else if (arg == "--device") {
      target_device_arg = value_for("--device");
      interactive_mode = false;
    } else if (arg == "--display") {
      const std::string value = value_for("--display");
      long long parsed = 0;
      if (!ParseInteger(value, 0, std::numeric_limits<int>::max(), &parsed)) {
        UsageError("invalid --display value '" + value + "' (expected a non-negative display id)");
      }
      display_id_arg = static_cast<int>(parsed);
    } else if (arg == "--window") {
      const std::string value = value_for("--window");
      long long parsed = 0;
      if (!ParseInteger(value, 0, std::numeric_limits<int>::max(), &parsed)) {
        UsageError("invalid --window value '" + value +
                   "' (expected a window id from --list-windows)");
      }
      window_id_arg = value;
      interactive_mode = false;
    } else if (arg == "--list-windows") {
      list_windows = true;
    } else if (arg == "--list-displays") {
      list_displays = true;
    } else if (arg == "--preset") {
      const std::string value = value_for("--preset");
      static const struct {
        const char* name;
        QualityPreset preset;
      } kPresets[] = {
          {"auto", QualityPreset::kAuto},
          {"high", QualityPreset::kHigh},
          {"balanced", QualityPreset::kBalanced},
          {"smooth", QualityPreset::kSmooth},
          {"game", QualityPreset::kGame},
          {"cinema", QualityPreset::kCinema},
      };
      bool matched = false;
      for (const auto& entry : kPresets) {
        if (EqualsIgnoreCase(value, entry.name)) {
          preset_arg = entry.preset;
          matched = true;
          break;
        }
      }
      if (!matched) {
        UsageError("unknown --preset '" + value +
                   "' (expected Auto, High, Balanced, Smooth, Game or Cinema)");
      }
      preset_given = true;
    } else if (arg == "--no-audio") {
      audio_enabled_arg = false;
      audio_given = true;
    } else if (arg == "--bitrate") {
      const std::string value = value_for("--bitrate");
      long long parsed = 0;
      // Upper bound is the documented slider maximum by a wide margin, well
      // inside uint32_t: anything larger is a typo, not an intent.
      if (!ParseInteger(value, 1, 100000, &parsed)) {
        UsageError("invalid --bitrate value '" + value + "' (expected 1-100000 kbps)");
      }
      bitrate_arg = static_cast<uint32_t>(parsed);
      bitrate_given = true;
    } else if (arg == "--codec") {
      const std::string value = value_for("--codec");
      if (EqualsIgnoreCase(value, "h264")) {
        video_codec_arg = VideoCodec::kH264;
      } else if (EqualsIgnoreCase(value, "vp8")) {
        video_codec_arg = VideoCodec::kVP8;
      } else {
        UsageError("unknown --codec '" + value + "' (expected h264 or vp8)");
      }
      codec_given = true;
    } else if (arg == "--low-latency") {
      target_delay_arg = 200;
      delay_given = true;
    } else if (arg == "--no-verify") {
      verify_device_cert = false;
      verify_given = true;
    } else if (arg == "--auth-probe") {
      auth_probe_arg = value_for("--auth-probe");
      interactive_mode = false;
    } else {
      UsageError("unknown option '" + arg + "'");
    }
  }

  PrintBanner();

  if (!auth_probe_arg.empty()) {
    std::cout << "\nRunning device authentication probe against " << auth_probe_arg << "...\n";
    CastChannel channel;
    // --no-verify is the documented dev escape hatch; honouring it here as well
    // keeps the probe usable against a device with a self-signed certificate.
    channel.SetVerifyDeviceCert(verify_device_cert);
    if (!channel.Connect(auth_probe_arg, 8009)) {
      std::cerr << "TCP/TLS connection failed.\n";
      return 1;
    }
    bool ok = channel.AuthenticateDevice(5000);
    std::cout << (ok ? "\033[1;32mDevice authentication OK\033[0m\n"
                     : "\033[1;31mDevice authentication FAILED\033[0m (see log above)\n");
    channel.Disconnect();
    return ok ? 0 : 2;
  }

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
    if (displays.empty()) {
      std::cout << "\n\033[1;33m--- Available Displays ---\033[0m\n"
#if defined(_WIN32)
                << "  (none) This session has no capturable desktop. DXGI returns no outputs\n"
                   "  when the process runs without an interactive desktop or GPU access\n"
                   "  (a service/session-0 logon, RDP, or a headless VM). Run CastMirror from\n"
                   "  a local desktop session.\n";
#else
                << "  (none) No display capture backend is available in this session.\n";
#endif
    } else {
      std::cout << "\n\033[1;33m--- Available Displays ---\033[0m\n";
      for (const auto& d : displays) {
        std::cout << "  [" << d.id << "] " << d.name << " (" << d.width << "x" << d.height << " @ "
                  << d.refresh_rate << "Hz)" << (d.is_primary ? " [Primary]" : "") << "\n";
      }
    }
    engine.Shutdown();
    return 0;
  }

  if (!interactive_mode && !target_device_arg.empty()) {
    // One assembly path for both source kinds: the CLI only overrides what the
    // user actually typed, and every other field comes from the saved
    // configuration (BuildSessionOptions, config.h).
    SessionOverrides overrides;
    if (preset_given) overrides.preset = preset_arg;
    if (audio_given) overrides.enable_audio = audio_enabled_arg;
    if (codec_given) overrides.video_codec = video_codec_arg;
    if (bitrate_given) overrides.video_bitrate_kbps = bitrate_arg;
    if (delay_given) overrides.target_delay_ms = target_delay_arg;
    if (verify_given) overrides.verify_device_cert = verify_device_cert;

    const auto& cfg = ConfigStore::Instance().Get();
    const bool cast_window = !window_id_arg.empty();
    bool started = false;
    if (cast_window) {
      int win_id = 0;
      long long parsed = 0;
      // Already validated at parse time; re-parsed here to keep the value in
      // the same integer type the capture source uses.
      if (!ParseInteger(window_id_arg, 0, std::numeric_limits<int>::max(), &parsed)) {
        std::cerr << "Invalid window ID: " << window_id_arg << "\n";
        engine.Shutdown();
        return kExitUsage;
      }
      win_id = static_cast<int>(parsed);
      // Resolve window title for stats/persistence.
      std::string win_title;
      for (const auto& w : engine.GetWindows()) {
        if (w.id == win_id) {
          win_title = w.title;
          break;
        }
      }
      CaptureSource source{CaptureSourceKind::kWindow, win_id, win_title};
      overrides.source = source;
      std::cout << "Initiating Cast of window [" << win_id << "]"
                << (win_title.empty() ? "" : (" (" + win_title + ")")) << " to "
                << target_device_arg << "...\n";
      started = engine.StartCasting(target_device_arg, source, BuildSessionOptions(cfg, overrides));
    } else {
      std::cout << "Initiating Cast to " << target_device_arg << "...\n";
      started = engine.StartCasting(
          target_device_arg, display_id_arg, BuildSessionOptions(cfg, overrides));
    }

    if (!started) {
      std::cerr << "Failed to start casting to " << target_device_arg << "\n";
      const std::string reason = engine.GetLastError();
      if (!reason.empty()) std::cerr << reason << "\n";
      engine.Shutdown();
      return 1;
    }

    std::cout << "Streaming. Press Ctrl+C or Enter to stop...\n";
    InstallConsoleInterruptHandler();
    while (engine.GetState() == SessionState::kStreaming && !StopRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto stats = engine.GetStats();
      std::cout << "\r[LIVE] FPS: " << std::fixed << std::setprecision(1) << stats.current_fps
                << " | Bitrate: " << (stats.bitrate_kbps / 1000.0) << " Mbps"
                << " | RTT: " << stats.round_trip_time_ms << " ms"
                << " | Loss: " << (stats.packet_loss_fraction * 100.0) << "%"
                << " | Target Delay: " << stats.target_delay_ms << "ms" << std::flush;
    }
    std::cout << "\nStopping Cast session...\n";
    const SessionState final_state = engine.GetState();
    const std::string final_error = engine.GetLastError();
    engine.StopCasting();
    engine.Shutdown();
    if (final_state == SessionState::kFailed) {
      // A session that ended in Failed is a failure even though nothing threw;
      // a script needs to see that in the exit status.
      std::cerr << "Cast session failed";
      if (!final_error.empty()) std::cerr << ": " << final_error;
      std::cerr << "\n";
      return kExitSessionFailed;
    }
    return 0;
  }

  // Interactive TUI / Menu Loop
  std::cout << "Scanning for Google Cast devices on your network...\n";
  engine.StartDiscovery();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  InstallConsoleInterruptHandler();

  while (true) {
    if (StopRequested()) break;
    auto devices = engine.GetDevices();
    auto displays = engine.GetDisplays();
    const auto& cfg = engine.GetConfig();

    std::cout << "\n\033[1;33m--- Discovered Cast Devices ---\033[0m\n";
    if (devices.empty()) {
      std::cout << "  (No devices found yet. Press [R] to rescan or [A] to add manual IP)\n";
    } else {
      for (size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << devices[i].name << " \033[32m("
                  << devices[i].model_name << ")\033[0m"
                  << " @ " << devices[i].ip_address << " ["
                  << DeviceStatusToString(devices[i].status) << "]\n";
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
      std::cout << "  Last Device    : " << cfg.last_device_name << " (" << cfg.last_device_ip
                << ")\n";
    }

    std::cout << "\n\033[1;32mActions:\033[0m\n";
    std::cout << "  [1-" << std::max<size_t>(1, devices.size()) << "] Select device & Cast\n";
    std::cout << "  [L] Cast to Last Device ("
              << (cfg.last_device_name.empty() ? "None" : cfg.last_device_name) << ")\n";
    std::cout << "  [P] Change Quality Preset (Auto / High / Balanced / Smooth / Game / Cinema)\n";
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
      // Cycle mirrors the GUI preset order (app/gui/cast_tab.cc: Auto, High,
      // Balanced, Smooth, Game, Cinema).
      if (mcfg.quality_preset == QualityPreset::kAuto)
        mcfg.quality_preset = QualityPreset::kHigh;
      else if (mcfg.quality_preset == QualityPreset::kHigh)
        mcfg.quality_preset = QualityPreset::kBalanced;
      else if (mcfg.quality_preset == QualityPreset::kBalanced)
        mcfg.quality_preset = QualityPreset::kSmooth;
      else if (mcfg.quality_preset == QualityPreset::kSmooth)
        mcfg.quality_preset = QualityPreset::kGame;
      else if (mcfg.quality_preset == QualityPreset::kGame)
        mcfg.quality_preset = QualityPreset::kCinema;
      else
        mcfg.quality_preset = QualityPreset::kAuto;
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
          if (w.id == win_id) {
            win_title = w.title;
            break;
          }
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
          if (engine.StartCasting(
                  dev.id, cfg.last_display_id, cfg.quality_preset, cfg.audio_enabled)) {
            std::cout
                << "\n\033[1;32m[LIVE] Mirroring active! Press Enter to stop casting.\033[0m\n";
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
