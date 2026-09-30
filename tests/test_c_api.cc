#include <gtest/gtest.h>
#include "castcore/c_api.h"
#include "castcore/cast_engine.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

TEST(CApiTest, LifecycleAndStateTransitions) {
  EXPECT_TRUE(castmirror_init());
  EXPECT_EQ(castmirror_get_state(), CASTMIRROR_STATE_IDLE);

  // Stats retrieval in idle state
  CastMirrorStreamStats stats{};
  EXPECT_TRUE(castmirror_get_stats(&stats));

  // Bitrate and delay runtime adjustments
  castmirror_set_bitrate(5000);
  castmirror_set_playout_delay(150);
  castmirror_set_freeze(true);
  castmirror_set_freeze(false);
  castmirror_set_muted(true);
  castmirror_set_muted(false);

  // Discovery controls
  castmirror_start_discovery();
  int dev_count = castmirror_get_device_count();
  EXPECT_GE(dev_count, 0);

  CastMirrorDeviceInfo dev_info{};
  EXPECT_FALSE(castmirror_get_device_info(-1, &dev_info));
  EXPECT_FALSE(castmirror_get_device_info(999, &dev_info));

  castmirror_stop_discovery();
  castmirror_shutdown();
}

TEST(CApiTest, DisplayAndWindowEnumeration) {
  ASSERT_TRUE(castmirror_init());

#if defined(_WIN32)
  // Windows display enumeration goes through DXGI, which reports no outputs at
  // all when the process has no interactive desktop or GPU access: a service or
  // session-0 logon, a remote session, or a headless build agent. A zero count
  // there says nothing about the enumeration code, so the assertions below are
  // only meaningful in an interactive session. (GitHub's Windows runners are
  // session 0, so this is also what lets the suite be wired into Windows CI.)
  DWORD session_id = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id) && session_id == 0) {
    GTEST_SKIP() << "session 0 has no interactive desktop; DXGI cannot enumerate outputs";
  }
#endif

  int display_count = castmirror_get_display_count();
  EXPECT_GT(display_count, 0);

  CastMirrorDisplayInfo display{};
  EXPECT_TRUE(castmirror_get_display_info(0, &display));
  EXPECT_GT(display.width, 0);
  EXPECT_GT(display.height, 0);
  EXPECT_FALSE(castmirror_get_display_info(-1, &display));
  EXPECT_FALSE(castmirror_get_display_info(display_count, &display));
  EXPECT_FALSE(castmirror_get_display_info(0, nullptr));

  int window_count = castmirror_get_window_count();
  EXPECT_GE(window_count, 0);
  if (window_count > 0) {
    CastMirrorWindowInfo window{};
    EXPECT_TRUE(castmirror_get_window_info(0, &window));
    EXPECT_FALSE(castmirror_get_window_info(window_count, &window));
  }
  EXPECT_FALSE(castmirror_get_window_info(0, nullptr));
  (void)castmirror_window_capture_supported();

  castmirror_shutdown();
}

// The managed client verifies this version before its first call and refuses to
// run on a mismatch (CastCoreBridge.VerifyNativeAbi). The literal is deliberate:
// bumping the ABI means updating CastCoreBridge.ExpectedAbiVersion and this test
// in the same commit as the struct change.
TEST(CApiTest, AbiVersionMatchesTheManagedClient) {
  EXPECT_EQ(castmirror_abi_version(), CASTMIRROR_ABI_VERSION);
  EXPECT_EQ(castmirror_abi_version(), 2u);
}

TEST(CApiTest, LastErrorBufferContract) {
  ASSERT_TRUE(castmirror_init());

  char buffer[64];
  EXPECT_EQ(castmirror_get_last_error(nullptr, sizeof(buffer)), -1);
  EXPECT_EQ(castmirror_get_last_error(buffer, 0), -1);

  int len = castmirror_get_last_error(buffer, sizeof(buffer));
  EXPECT_GE(len, 0);
  EXPECT_EQ(buffer[len], '\0');

  // Tiny buffer must still be NUL-terminated.
  char tiny[4] = {'x', 'x', 'x', 'x'};
  int tiny_len = castmirror_get_last_error(tiny, sizeof(tiny));
  EXPECT_GE(tiny_len, 0);
  EXPECT_LE(tiny_len, 3);
  EXPECT_EQ(tiny[tiny_len], '\0');

  castmirror_shutdown();
}

TEST(CApiTest, ConfigJsonRoundTrip) {
  ASSERT_TRUE(castmirror_init());

  int needed = castmirror_get_config_json(nullptr, 0);
  ASSERT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed));
  int written = castmirror_get_config_json(buf.data(), needed);
  ASSERT_GT(written, 0);
  EXPECT_LE(written, needed - 1);
  EXPECT_EQ(buf[static_cast<size_t>(written)], '\0');

  nlohmann::json original = nlohmann::json::parse(std::string(buf.data(), static_cast<size_t>(written)));
  for (const char* key : {"quality_preset", "capture_fps", "audio_bitrate_bps",
                          "adaptive_resolution_enabled", "ui_theme"}) {
    EXPECT_TRUE(original.contains(key)) << key;
  }

  // Malformed and non-object payloads are rejected without touching the config.
  EXPECT_FALSE(castmirror_set_config_json(nullptr));
  EXPECT_FALSE(castmirror_set_config_json("{not json"));
  EXPECT_FALSE(castmirror_set_config_json("[1,2,3]"));

  nlohmann::json patch = {
      {"capture_fps", 30},
      {"ui_theme", "dark"},
      {"adaptive_resolution_enabled", false},
      {"audio_bitrate_bps", 96000},
  };
  ASSERT_TRUE(castmirror_set_config_json(patch.dump().c_str()));

  needed = castmirror_get_config_json(nullptr, 0);
  std::vector<char> after_buf(static_cast<size_t>(needed));
  written = castmirror_get_config_json(after_buf.data(), needed);
  ASSERT_GT(written, 0);
  nlohmann::json after = nlohmann::json::parse(std::string(after_buf.data(), static_cast<size_t>(written)));
  EXPECT_EQ(after["capture_fps"].get<int>(), 30);
  EXPECT_EQ(after["ui_theme"].get<std::string>(), "dark");
  EXPECT_FALSE(after["adaptive_resolution_enabled"].get<bool>());
  EXPECT_EQ(after["audio_bitrate_bps"].get<uint32_t>(), 96000u);
  // Keys that were not part of the patch survive the merge.
  EXPECT_EQ(after["quality_preset"].get<std::string>(), original["quality_preset"].get<std::string>());
  EXPECT_EQ(after["target_delay_ms"].get<int>(), original["target_delay_ms"].get<int>());

  // Unknown keys are ignored, not fatal.
  EXPECT_TRUE(castmirror_set_config_json(R"({"not_a_real_key": 42})"));

  // Restore whatever the user (or a previous test) had.
  ASSERT_TRUE(castmirror_set_config_json(original.dump().c_str()));

  castmirror_shutdown();
}

// A known key whose value cannot be stored as its field's type must be
// rejected, not coerced: nlohmann's get<uint32_t>() turns -1 into 4294967295
// and get<int>() truncates 4294967296 to 0, so a client that sent nonsense used
// to have a *different* number persisted under the same key with nothing to
// indicate it. The log names the rejected key; the stored value is untouched.
TEST(CApiTest, ConfigJsonRejectsUnstorableValuesInsteadOfCoercing) {
  ASSERT_TRUE(castmirror_init());

  int needed = castmirror_get_config_json(nullptr, 0);
  ASSERT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed));
  int written = castmirror_get_config_json(buf.data(), needed);
  ASSERT_GT(written, 0);
  const nlohmann::json original =
      nlohmann::json::parse(std::string(buf.data(), static_cast<size_t>(written)));

  // Establish known values first so a rejected key is distinguishable from a key
  // that was never set.
  const nlohmann::json good = {
      {"max_bitrate_kbps", 12000},
      {"capture_fps", 30},
      {"ui_theme", "light"},
      {"audio_enabled", true},
  };
  ASSERT_TRUE(castmirror_set_config_json(good.dump().c_str()));

  const nlohmann::json hostile = {
      {"max_bitrate_kbps", -1},        // wraps to 4294967295, then clamps to 25000
      {"capture_fps", 4294967296LL},   // truncates to 0
      {"ui_theme", 7},                 // not a string
      {"audio_enabled", "yes"},        // not a boolean
  };
  ASSERT_TRUE(castmirror_set_config_json(hostile.dump().c_str()));

  needed = castmirror_get_config_json(nullptr, 0);
  std::vector<char> after_buf(static_cast<size_t>(needed));
  written = castmirror_get_config_json(after_buf.data(), needed);
  ASSERT_GT(written, 0);
  const nlohmann::json after =
      nlohmann::json::parse(std::string(after_buf.data(), static_cast<size_t>(written)));
  EXPECT_EQ(after["max_bitrate_kbps"].get<uint32_t>(), 12000u);
  EXPECT_EQ(after["capture_fps"].get<int>(), 30);
  EXPECT_EQ(after["ui_theme"].get<std::string>(), "light");
  EXPECT_TRUE(after["audio_enabled"].get<bool>());

  // Restore whatever the user (or a previous test) had.
  ASSERT_TRUE(castmirror_set_config_json(original.dump().c_str()));

  castmirror_shutdown();
}

TEST(CApiTest, SelfTestReportsSubsystems) {
  ASSERT_TRUE(castmirror_init());

  int needed = castmirror_self_test(nullptr, 0);
  ASSERT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed));
  int written = castmirror_self_test(buf.data(), needed);
  ASSERT_GT(written, 0);
  EXPECT_EQ(buf[static_cast<size_t>(written)], '\0');

  nlohmann::json result = nlohmann::json::parse(std::string(buf.data(), static_cast<size_t>(written)));
  for (const char* key : {"capture", "encoder", "audio", "network"}) {
    ASSERT_TRUE(result.contains(key)) << key;
    EXPECT_TRUE(result[key].contains("ok")) << key;
    EXPECT_TRUE(result[key]["ok"].is_boolean()) << key;
    EXPECT_TRUE(result[key]["detail"].is_string()) << key;
  }

  castmirror_shutdown();
}

TEST(CApiTest, StreamStatsCarryPipelineDetails) {
  ASSERT_TRUE(castmirror_init());

  CastMirrorStreamStats stats{};
  ASSERT_TRUE(castmirror_get_stats(&stats));
  EXPECT_GE(stats.current_framerate, 0);
  EXPECT_GE(stats.adaptive_rung_count, 0);
  // The string fields are NUL-terminated char arrays (empty while idle).
  EXPECT_EQ(stats.encoder_name[sizeof(stats.encoder_name) - 1], '\0');
  EXPECT_EQ(stats.capture_backend[sizeof(stats.capture_backend) - 1], '\0');
  EXPECT_EQ(stats.display_name[sizeof(stats.display_name) - 1], '\0');

  // The size-query contract holds for a too-small buffer too.
  char tiny[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  int len = castmirror_get_config_json(tiny, sizeof(tiny));
  EXPECT_GE(len, 0);
  EXPECT_LE(len, static_cast<int>(sizeof(tiny)) - 1);
  EXPECT_EQ(tiny[len], '\0');

  castmirror_shutdown();
}

// ---------------------------------------------------------------------------
// Callback registration: the entire ABI surface app/winui drives the core with.
// ---------------------------------------------------------------------------

namespace {

// The three C callbacks are plain function pointers, so the probe they write to
// has file scope. `expected_user_data` is set by each test before registering,
// which is what makes the user_data round-trip observable: a callback that
// received somebody else's pointer records a mismatch and does not count.
struct CallbackProbe {
  std::atomic<void*> expected_user_data{nullptr};
  std::atomic<int> state_calls{0};
  std::atomic<int> device_calls{0};
  std::atomic<int> stats_calls{0};
  std::atomic<int> user_data_mismatches{0};
  std::atomic<int> last_state{-1};
  std::atomic<int> last_device_count{-1};

  void Reset() {
    state_calls.store(0);
    device_calls.store(0);
    stats_calls.store(0);
    user_data_mismatches.store(0);
    last_state.store(-1);
    last_device_count.store(-1);
  }

  bool OwnsUserData(void* user_data) {
    if (user_data == nullptr || user_data != expected_user_data.load()) {
      user_data_mismatches.fetch_add(1);
      return false;
    }
    return true;
  }
} g_probe;

void OnStateCallback(CastMirrorState state, const char* message, void* user_data) {
  (void)message;
  if (!g_probe.OwnsUserData(user_data)) return;
  g_probe.last_state.store(static_cast<int>(state));
  g_probe.state_calls.fetch_add(1);
}

void OnDevicesCallback(int count, void* user_data) {
  if (!g_probe.OwnsUserData(user_data)) return;
  g_probe.last_device_count.store(count);
  g_probe.device_calls.fetch_add(1);
}

void OnStatsCallback(const CastMirrorStreamStats* stats, void* user_data) {
  if (!g_probe.OwnsUserData(user_data)) return;
  (void)stats;
  g_probe.stats_calls.fetch_add(1);
}

}  // namespace

TEST(CApiTest, CallbacksReceiveUserDataAcrossLifecycle) {
  ASSERT_TRUE(castmirror_init());

  g_probe.Reset();
  int user_token = 0;  // stack-local sentinel: must come back verbatim
  g_probe.expected_user_data.store(&user_token);

  castmirror_set_state_callback(&OnStateCallback, &user_token);
  castmirror_set_devices_callback(&OnDevicesCallback, &user_token);
  castmirror_set_stats_callback(&OnStatsCallback, &user_token);

  castmirror_start_discovery();

  // The state callback is wired StateMachine -> CastEngine -> C. Failed is
  // reachable from every state, so this transition is deterministic.
  auto& engine = castcore::CastEngine::Instance();
  ASSERT_TRUE(engine.GetStateMachine().TransitionTo(castcore::SessionState::kFailed,
                                                    "callback registration probe"));
  EXPECT_GE(g_probe.state_calls.load(), 1);
  EXPECT_EQ(g_probe.last_state.load(), CASTMIRROR_STATE_FAILED);
  engine.GetStateMachine().TransitionTo(castcore::SessionState::kIdle, "probe cleanup");

  // The devices callback is wired DeviceDiscovery -> CastEngine -> C.
  castcore::CastDevice device;
  device.id = "callback-probe-device";
  device.name = "Callback Probe TV";
  device.model_name = "Chromecast Ultra";
  device.ip_address = "192.0.2.10";
  device.port = 8009;
  device.capabilities = castcore::kCapVideoOut | castcore::kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(device);
  EXPECT_GE(g_probe.device_calls.load(), 1);
  EXPECT_GE(g_probe.last_device_count.load(), 1);

  // The documented client lifecycle: init -> start_discovery -> self_test ->
  // shutdown. None of this may disturb the registered callbacks, and whatever
  // does fire must carry the original user_data.
  const int needed = castmirror_self_test(nullptr, 0);
  EXPECT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed > 1 ? needed : 1));
  EXPECT_GT(castmirror_self_test(buf.data(), buf.size()), 0);
  castmirror_stop_discovery();

  // NOTE: the stats callback is registered and torn down exactly like the other
  // two, but CastEngine::stats_cb_ is set by SetOnStatsUpdated and never invoked,
  // so no stats tick can be provoked from outside the core. The round-trip check
  // above still covers it: if it ever fires with the wrong pointer it will be
  // counted as a mismatch.

  EXPECT_EQ(g_probe.user_data_mismatches.load(), 0)
      << "user_data did not survive the round-trip through the C ABI";

  // Unregister before shutdown so nothing calls into a dead probe afterwards.
  castmirror_set_state_callback(nullptr, nullptr);
  castmirror_set_devices_callback(nullptr, nullptr);
  castmirror_set_stats_callback(nullptr, nullptr);
  g_probe.expected_user_data.store(nullptr);

  castmirror_shutdown();
}

// The WinUI client queries the required size with (NULL, 0) and then fills a
// buffer of exactly that size. If the two calls disagree the client receives
// truncated JSON, its deserialiser throws, and it silently falls back to
// defaults. These cases pin the contract that makes the two-step safe.
TEST(CApiTest, SizeQueryThenFillContractHoldsForConfigAndSelfTest) {
  ASSERT_TRUE(castmirror_init());

  const int config_size = castmirror_get_config_json(nullptr, 0);
  ASSERT_GT(config_size, 1);
  // The document must be byte-stable between the query and the fill.
  EXPECT_EQ(castmirror_get_config_json(nullptr, 0), config_size);
  // NULL out_buf means "report the size" regardless of buf_len.
  EXPECT_EQ(castmirror_get_config_json(nullptr, config_size), config_size);
  EXPECT_EQ(castmirror_get_config_json(nullptr, -1), config_size);

  std::vector<char> config(static_cast<size_t>(config_size));
  const int config_written = castmirror_get_config_json(config.data(), config_size);
  ASSERT_GE(config_written, 0);
  ASSERT_LT(config_written, config_size) << "the size query must leave room for the NUL";
  EXPECT_EQ(config[static_cast<size_t>(config_written)], '\0');
  EXPECT_EQ(std::string(config.data()).size(), static_cast<size_t>(config_written));
  // The query returns a bound, not the exact length (c_api.h says so), so the
  // document is not required to fill the buffer. What must hold is that the
  // bound is large enough for the whole document: prove it by comparing the
  // bytes a query-sized buffer yields against a deliberately oversized one.
  std::vector<char> config_large(static_cast<size_t>(config_size) + 4096);
  const int config_large_written =
      castmirror_get_config_json(config_large.data(), static_cast<int>(config_large.size()));
  ASSERT_GE(config_large_written, 0);
  EXPECT_EQ(std::string(config.data()), std::string(config_large.data()))
      << "a buffer sized from the query must never truncate the document";
  EXPECT_NO_THROW({
    nlohmann::json parsed = nlohmann::json::parse(config.data());
    EXPECT_TRUE(parsed.is_object());
  });

  // Degenerate buffers still NUL-terminate and report zero characters written.
  char one[1] = {'x'};
  EXPECT_EQ(castmirror_get_config_json(one, 1), 0);
  EXPECT_EQ(one[0], '\0');

  const int selftest_size = castmirror_self_test(nullptr, 0);
  ASSERT_GT(selftest_size, 1);
  EXPECT_EQ(castmirror_self_test(nullptr, 0), selftest_size);

  std::vector<char> selftest(static_cast<size_t>(selftest_size));
  const int selftest_written = castmirror_self_test(selftest.data(), selftest_size);
  ASSERT_GE(selftest_written, 0);
  // Same bound-not-length contract as the config document; unlike the config
  // the self-test re-probes the hardware on every call, so its two documents
  // are expected to differ in content and are compared by shape, not bytes.
  ASSERT_LT(selftest_written, selftest_size)
      << "a buffer sized from the query must never truncate the document";
  EXPECT_EQ(selftest[static_cast<size_t>(selftest_written)], '\0');
  EXPECT_EQ(std::string(selftest.data()).size(), static_cast<size_t>(selftest_written));
  EXPECT_NO_THROW({
    nlohmann::json parsed = nlohmann::json::parse(selftest.data());
    ASSERT_TRUE(parsed.is_object());
    // The WinUI diagnostics pane renders exactly these four subsystems.
    for (const char* key : {"capture", "encoder", "audio", "network"}) {
      ASSERT_TRUE(parsed.contains(key)) << key;
      EXPECT_TRUE(parsed[key]["ok"].is_boolean()) << key;
      EXPECT_TRUE(parsed[key]["detail"].is_string()) << key;
    }
  });

  char one_selftest[1] = {'x'};
  EXPECT_EQ(castmirror_self_test(one_selftest, 1), 0);
  EXPECT_EQ(one_selftest[0], '\0');

  castmirror_shutdown();
}
