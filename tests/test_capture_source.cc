#include <gtest/gtest.h>
#include "castcore/types.h"
#include "castcore/config.h"
#include "castcore/display_capture.h"
#include <filesystem>
#include <fstream>

using namespace castcore;

TEST(CaptureSourceTest, KindStringRoundTrip) {
  EXPECT_STREQ(CaptureSourceKindToString(CaptureSourceKind::kMonitor), "monitor");
  EXPECT_STREQ(CaptureSourceKindToString(CaptureSourceKind::kWindow), "window");
  EXPECT_EQ(CaptureSourceKindFromString("monitor"), CaptureSourceKind::kMonitor);
  EXPECT_EQ(CaptureSourceKindFromString("window"),  CaptureSourceKind::kWindow);
  EXPECT_EQ(CaptureSourceKindFromString("Window"),  CaptureSourceKind::kWindow);
  EXPECT_EQ(CaptureSourceKindFromString("WINDOW"),  CaptureSourceKind::kWindow);
  // Unknown strings default to monitor (safe fallback).
  EXPECT_EQ(CaptureSourceKindFromString("nope"),    CaptureSourceKind::kMonitor);
  EXPECT_EQ(CaptureSourceKindFromString(""),        CaptureSourceKind::kMonitor);
}

TEST(CaptureSourceTest, EqualityAndAccessors) {
  CaptureSource m{CaptureSourceKind::kMonitor, 1, "DP-1", 0, 0, 1920, 1080};
  CaptureSource w{CaptureSourceKind::kWindow, 0x400001u, "Terminal", 100, 100, 800, 600};
  EXPECT_TRUE(m.IsMonitor());
  EXPECT_FALSE(m.IsWindow());
  EXPECT_TRUE(w.IsWindow());
  EXPECT_FALSE(w.IsMonitor());
  EXPECT_EQ(m, (CaptureSource{CaptureSourceKind::kMonitor, 1, "other name"}));
  EXPECT_NE(m, w);
  EXPECT_NE(w, (CaptureSource{CaptureSourceKind::kWindow, 0x400002u, "Terminal"}));
}

TEST(CaptureSourceTest, SessionOptionsSourceDefaultsToNullopt) {
  SessionOptions opts;
  EXPECT_FALSE(opts.source.has_value());
  opts.source = CaptureSource{CaptureSourceKind::kWindow, 42, "Editor"};
  ASSERT_TRUE(opts.source.has_value());
  EXPECT_EQ(opts.source->kind, CaptureSourceKind::kWindow);
  EXPECT_EQ(opts.source->id, 42);
}

TEST(CaptureSourceTest, CapturedVideoFrameSourceLostDefaultFalse) {
  CapturedVideoFrame f;
  EXPECT_FALSE(f.source_lost);
  f.source_lost = true;
  EXPECT_TRUE(f.source_lost);
}

TEST(CaptureSourceTest, StreamStatsHasSourceKindField) {
  StreamStats s;
  EXPECT_TRUE(s.source_kind.empty());
  s.source_kind = CaptureSourceKindToString(CaptureSourceKind::kWindow);
  EXPECT_EQ(s.source_kind, "window");
}

// Config schema v2 -> v3 migration: a config written without source fields
// should load with last_source_kind="monitor" and last_source_id mirroring
// last_display_id.
TEST(CaptureSourceTest, ConfigV2ToV3Migration) {
  std::string path = "/tmp/castmirror_test_source_v2.json";
  {
    std::ofstream f(path);
    f << R"({
      "schema_version": 2,
      "last_device_id": "dev",
      "last_device_name": "TV",
      "last_device_ip": "10.0.0.5",
      "last_display_id": 1,
      "audio_enabled": true,
      "quality_preset": "Auto"
    })";
  }
  auto& store = ConfigStore::Instance();
  auto& cfg = store.Mutable();
  cfg = AppConfig{};  // reset
  ASSERT_TRUE(store.Load(path));
  EXPECT_EQ(cfg.schema_version, 3);
  EXPECT_EQ(cfg.last_source_kind, "monitor");
  EXPECT_EQ(cfg.last_source_id, 1);
  EXPECT_TRUE(cfg.last_source_name.empty());
  std::filesystem::remove(path);
}

TEST(CaptureSourceTest, ConfigRoundTripsSourceFields) {
  std::string path = "/tmp/castmirror_test_source_round.json";
  if (std::filesystem::exists(path)) std::filesystem::remove(path);
  auto& store = ConfigStore::Instance();
  auto& cfg = store.Mutable();
  cfg = AppConfig{};
  cfg.last_source_kind = "window";
  cfg.last_source_id = 0x400001;
  cfg.last_source_name = "Terminal";
  ASSERT_TRUE(store.Save(path));
  cfg.last_source_kind = "monitor";
  cfg.last_source_id = 0;
  cfg.last_source_name.clear();
  ASSERT_TRUE(store.Load(path));
  EXPECT_EQ(cfg.last_source_kind, "window");
  EXPECT_EQ(cfg.last_source_id, 0x400001);
  EXPECT_EQ(cfg.last_source_name, "Terminal");
  std::filesystem::remove(path);
}

// A backend that only implements the legacy Start(int,int) must still work
// through the source-aware Start(CaptureSource,int) default impl.
namespace {
class DummyCapture : public IDisplayCapture {
 public:
  using IDisplayCapture::Start;  // bring in Start(CaptureSource,int) default impl
  bool Start(int display_id, int target_fps) override {
    started_ = true;
    last_id_ = display_id;
    last_fps_ = target_fps;
    return true;
  }
  void Stop() override { started_ = false; }
  bool IsCapturing() const override { return started_; }
  std::vector<DisplayInfo> EnumerateDisplays() override { return {}; }
  void SetFrameCallback(FrameCallback) override {}
  int last_id_ = -1;
  int last_fps_ = -1;
  bool started_ = false;
};
}  // namespace

TEST(CaptureSourceTest, DefaultSourceStartForwardsToLegacy) {
  DummyCapture cap;
  CaptureSource src{CaptureSourceKind::kMonitor, 7, "HDMI-1"};
  ASSERT_TRUE(cap.Start(src, 30));
  EXPECT_EQ(cap.last_id_, 7);
  EXPECT_EQ(cap.last_fps_, 30);
  EXPECT_TRUE(cap.IsCapturing());
  cap.Stop();
}

TEST(CaptureSourceTest, DefaultWindowEnumerateEmptyAndUnsupported) {
  DummyCapture cap;
  EXPECT_TRUE(cap.EnumerateWindows().empty());
  EXPECT_FALSE(cap.SupportsWindowCapture());
  EXPECT_EQ(cap.ActiveSource().kind, CaptureSourceKind::kMonitor);
}
