#include <gtest/gtest.h>
#include "castcore/logger.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

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
  log.SetCallback([&](LogLevel, const std::string& msg) { seen_msg = msg; });
  LOG_INFO << "hello-ui-telemetry-test";
  EXPECT_NE(seen_msg.find("hello-ui-telemetry-test"), std::string::npos);
  EXPECT_NE(seen_msg.find("[INFO"), std::string::npos);
  log.SetCallback(nullptr);
}

TEST(LoggerTest, CallbackMayReenterLoggerWithoutDeadlock) {
  auto& log = Logger::Instance();
  LogLevel previous = log.GetMinLevel();
  log.SetMinLevel(LogLevel::kInfo);

  std::atomic<int> count{0};
  log.SetCallback([&](LogLevel, const std::string&) {
    if (count.fetch_add(1) == 0) {
      // Re-entering Logger from inside the callback used to deadlock on mutex_.
      (void)log.GetMinLevel();
      LOG_INFO << "nested-callback-log";
    }
  });
  LOG_INFO << "outer-callback-log";
  EXPECT_GE(count.load(), 2);

  log.SetCallback(nullptr);
  log.SetMinLevel(previous);
}

TEST(LoggerTest, MainLogRedactsIpWithoutCorruptingLongerDottedNumbers) {
  auto& log = Logger::Instance();
  LogLevel previous = log.GetMinLevel();
  log.SetMinLevel(LogLevel::kInfo);
  const std::string path = "/tmp/castmirror_test_redact.log";
  std::filesystem::remove(path);

  log.SetFileLogging(path);
  LOG_INFO << "peer=192.168.1.42:8009 oid=1.2.3.4.5 big=9999.1.2.3";
  log.SetFileLogging("");

  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string contents = ss.str();
  EXPECT_NE(contents.find("peer=[REDACTED]:8009"), std::string::npos);
  EXPECT_NE(contents.find("oid=1.2.3.4.5"), std::string::npos);
  EXPECT_NE(contents.find("big=9999.1.2.3"), std::string::npos);

  std::filesystem::remove(path);
  log.SetMinLevel(previous);
}
