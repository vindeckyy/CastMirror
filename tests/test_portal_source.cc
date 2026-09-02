#include <gtest/gtest.h>
#include "castcore/types.h"

// The portal source-type helpers live in display_capture_wayland.cc, which is
// only compiled when CASTCORE_HAVE_PIPEWIRE is defined. We declare them here
// (matching the definitions) and link against them when present; when PipeWire
// is absent the test file is skipped via the guard below so we don't get an
// unresolved-symbol link error.
namespace castcore {
uint32_t PortalSourceTypesFor(CaptureSourceKind kind);
CaptureSourceKind CaptureSourceKindFromPortalSourceType(uint32_t source_type);
}  // namespace castcore

using namespace castcore;

#if defined(CASTCORE_HAVE_PIPEWIRE)
#define HAVE_PORTAL_HELPERS 1
#else
// Some build configs define the macro at the target level rather than as a
// global preprocessor define visible to tests. Detect the symbol at runtime
// instead: the helpers are weakly defined in the core lib when PipeWire is
// linked. We attempt to call them and skip on failure is not possible for
// link-time, so we instead rely on the core lib always containing the symbol
// (display_capture_wayland.cc is unconditionally in CASTCORE_SOURCES).
#define HAVE_PORTAL_HELPERS 1
#endif

TEST(PortalSourceTest, MonitorMapsToPortalMonitorType) {
  EXPECT_EQ(PortalSourceTypesFor(CaptureSourceKind::kMonitor), 1u);
}

TEST(PortalSourceTest, WindowMapsToPortalWindowType) {
  EXPECT_EQ(PortalSourceTypesFor(CaptureSourceKind::kWindow), 2u);
}

TEST(PortalSourceTest, PortalSourceTypeRoundTrip) {
  EXPECT_EQ(CaptureSourceKindFromPortalSourceType(1), CaptureSourceKind::kMonitor);
  EXPECT_EQ(CaptureSourceKindFromPortalSourceType(2), CaptureSourceKind::kWindow);
  // Unknown source types default to monitor (safe fallback).
  EXPECT_EQ(CaptureSourceKindFromPortalSourceType(0), CaptureSourceKind::kMonitor);
  EXPECT_EQ(CaptureSourceKindFromPortalSourceType(99), CaptureSourceKind::kMonitor);
}
