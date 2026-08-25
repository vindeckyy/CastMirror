#include "castcore/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstring>

namespace castcore {

namespace {

const char* LevelToString(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO ";
    case LogLevel::kWarn:  return "WARN ";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kFatal: return "FATAL";
  }
  return "UNKNOWN";
}

const char* LevelColor(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "\033[36m"; // Cyan
    case LogLevel::kInfo:  return "\033[32m"; // Green
    case LogLevel::kWarn:  return "\033[33m"; // Yellow
    case LogLevel::kError: return "\033[31m"; // Red
    case LogLevel::kFatal: return "\033[35m"; // Magenta
  }
  return "\033[0m";
}

const char* BaseName(const char* path) {
  const char* p = std::strrchr(path, '/');
  if (!p) p = std::strrchr(path, '\\');
  return p ? p + 1 : path;
}

} // namespace

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

Logger::Logger() = default;

Logger::~Logger() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_stream_.is_open()) {
    file_stream_.close();
  }
}

void Logger::SetMinLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_level_ = level;
}

LogLevel Logger::GetMinLevel() const {
  return min_level_;
}

void Logger::SetFileLogging(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_stream_.is_open()) {
    file_stream_.close();
  }
  file_stream_.open(file_path, std::ios::out | std::ios::app);
}

void Logger::SetCallback(LogCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& message) {
  if (level < min_level_) return;

  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &in_time_t);
#else
  localtime_r(&in_time_t, &tm_buf);
#endif

  std::ostringstream ss;
  ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S.")
     << std::setfill('0') << std::setw(3) << ms.count() << " "
     << "[" << LevelToString(level) << "] "
     << "[" << BaseName(file) << ":" << line << "] "
     << message;

  std::string formatted = ss.str();

  std::lock_guard<std::mutex> lock(mutex_);

  // Terminal output with ANSI colors
  std::cout << LevelColor(level) << formatted << "\033[0m" << std::endl;

  if (file_stream_.is_open()) {
    file_stream_ << formatted << std::endl;
  }

  if (callback_) {
    callback_(level, message);
  }
}

} // namespace castcore
