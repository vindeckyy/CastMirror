#ifndef CASTCORE_LOGGER_H_
#define CASTCORE_LOGGER_H_

#include <string>
#include <sstream>
#include <mutex>
#include <functional>
#include <memory>
#include <fstream>
#include <cstdint>
#include <atomic>

namespace castcore {

enum class LogLevel {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
  kFatal = 4
};

class Logger {
 public:
  using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

  static Logger& Instance();

  void SetMinLevel(LogLevel level);
  LogLevel GetMinLevel() const;

  void SetFileLogging(const std::string& file_path);
  void SetSessionFileLogging(const std::string& file_path);
  void ClearSessionFileLogging();
  void SetCallback(LogCallback callback);

  static constexpr uint64_t kMaxMainLogBytes = 8ull * 1024ull * 1024ull;
  static constexpr uint64_t kMaxJsonLogBytes = 8ull * 1024ull * 1024ull;

  // Structured diagnostics JSON sidecar (Phase 0.2)
  static std::string GetDefaultJsonPath();
  void SetJsonLogging(const std::string& file_path);
  void ClearJsonLogging();
  void SetVerboseJsonEnabled(bool enabled);
  bool IsVerboseJsonEnabled() const;
  void LogJson(const std::string& json_line);
  void LogBreadcrumb(uint32_t frame_id, int64_t encode_ms, uint32_t udp_bytes, double rtt_ms, uint32_t nack_count);
  void LogBreadcrumbEx(uint32_t frame_id, int64_t encode_ms, uint32_t udp_bytes, double rtt_ms, uint32_t nack_count,
                       const std::string& pipeline, const std::string& stage);

  void Log(LogLevel level, const char* file, int line, const std::string& message);

 private:
  Logger();
  ~Logger();

  LogLevel min_level_ = LogLevel::kInfo;
  mutable std::mutex mutex_;
  std::ofstream file_stream_;
  std::ofstream session_stream_;
  std::string main_log_path_;
  LogCallback callback_;
  uint32_t log_write_count_ = 0;

  // JSON sidecar
  std::ofstream json_stream_;
  std::string json_log_path_;
  uint32_t json_write_count_ = 0;
  std::atomic<bool> verbose_json_enabled_{false};

  void RotateMainLogIfNeededLocked();
  void RotateJsonLogIfNeededLocked();
  std::string RedactIpAddresses(const std::string& input) const;
  bool ShouldEmitJson() const;
};

class LogMessage {
 public:
  LogMessage(LogLevel level, const char* file, int line)
      : level_(level), file_(file), line_(line) {}

  ~LogMessage() {
    Logger::Instance().Log(level_, file_, line_, stream_.str());
  }

  template <typename T>
  LogMessage& operator<<(const T& val) {
    stream_ << val;
    return *this;
  }

 private:
  LogLevel level_;
  const char* file_;
  int line_;
  std::ostringstream stream_;
};

} // namespace castcore

#define LOG_DEBUG castcore::LogMessage(castcore::LogLevel::kDebug, __FILE__, __LINE__)
#define LOG_INFO  castcore::LogMessage(castcore::LogLevel::kInfo,  __FILE__, __LINE__)
#define LOG_WARN  castcore::LogMessage(castcore::LogLevel::kWarn,  __FILE__, __LINE__)
#define LOG_ERROR castcore::LogMessage(castcore::LogLevel::kError, __FILE__, __LINE__)
#define LOG_FATAL castcore::LogMessage(castcore::LogLevel::kFatal, __FILE__, __LINE__)

#endif // CASTCORE_LOGGER_H_
