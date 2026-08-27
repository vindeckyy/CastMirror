#include <gtest/gtest.h>
#include "castcore/logger.h"
#include <atomic>

using namespace castcore;

TEST(LoggerTest, DebugDroppedWhenMinIsInfo) {
  auto& log = Logger::Instance();
  LogLevel previous = log.GetMinLevel();
  log.SetMinLevel(LogLevel::kInfo);
  log.ClearSessionFileLogging();

  std::atomic<int> seen{0};
  log.SetCallback([&](LogLevel, const std::string&) { seen++; });
  LOG_DEBUG << "should be dropped";
  EXPECT_EQ(seen.load(), 0);
  LOG_INFO << "should be delivered";
  EXPECT_GE(seen.load(), 1);

  log.SetCallback(nullptr);
  log.SetMinLevel(previous);
}

TEST(LoggerTest, CallbackReceivesFormattedLine) {
  auto& log = Logger::Instance();
  std::string seen_msg;
  log.SetCallback([&](LogLevel, const std::string& msg) {
    seen_msg = msg;
  });
  LOG_INFO << "hello-ui-telemetry-test";
  EXPECT_NE(seen_msg.find("hello-ui-telemetry-test"), std::string::npos);
  EXPECT_NE(seen_msg.find("[INFO"), std::string::npos);
  log.SetCallback(nullptr);
}
