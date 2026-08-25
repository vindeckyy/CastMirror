#ifndef CASTCORE_LOGGER_H_
#define CASTCORE_LOGGER_H_

#include <string>
#include <sstream>
#include <mutex>
#include <functional>
#include <memory>
#include <fstream>

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
  void SetCallback(LogCallback callback);

  void Log(LogLevel level, const char* file, int line, const std::string& message);

 private:
  Logger();
  ~Logger();

  LogLevel min_level_ = LogLevel::kInfo;
  std::mutex mutex_;
  std::ofstream file_stream_;
  LogCallback callback_;
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
