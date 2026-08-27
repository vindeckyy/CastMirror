#include "castcore/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <filesystem>

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
    case LogLevel::kDebug: return "\033[36m";
    case LogLevel::kInfo:  return "\033[32m";
    case LogLevel::kWarn:  return "\033[33m";
    case LogLevel::kError: return "\033[31m";
    case LogLevel::kFatal: return "\033[35m";
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
  if (session_stream_.is_open()) {
    session_stream_.close();
  }
}

void Logger::SetMinLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_level_ = level;
}

LogLevel Logger::GetMinLevel() const {
  return min_level_;
}

void Logger::RotateMainLogIfNeededLocked() {
  if (!file_stream_.is_open() || main_log_path_.empty()) {
    return;
  }
  log_write_count_++;
  if ((log_write_count_ % 64) != 0) {
    return;
  }
  file_stream_.flush();
  std::error_code ec;
  auto sz = std::filesystem::file_size(main_log_path_, ec);
  if (ec || sz < kMaxMainLogBytes) {
    return;
  }
  file_stream_.close();
  std::string rotated = main_log_path_ + ".old";
  std::filesystem::remove(rotated, ec);
  std::filesystem::rename(main_log_path_, rotated, ec);
  file_stream_.open(main_log_path_, std::ios::out | std::ios::trunc);
}

void Logger::SetFileLogging(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_stream_.is_open()) {
    file_stream_.close();
  }
  main_log_path_ = file_path;
  log_write_count_ = 0;
  try {
    if (std::filesystem::exists(file_path) &&
        std::filesystem::file_size(file_path) >= kMaxMainLogBytes) {
      std::string rotated = file_path + ".old";
      std::error_code ec;
      std::filesystem::remove(rotated, ec);
      std::filesystem::rename(file_path, rotated, ec);
    }
  } catch (...) {}
  file_stream_.open(file_path, std::ios::out | std::ios::app);
}

void Logger::SetSessionFileLogging(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_stream_.is_open()) {
    session_stream_.close();
  }
  session_stream_.open(file_path, std::ios::out | std::ios::trunc);
}

void Logger::ClearSessionFileLogging() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_stream_.is_open()) {
    session_stream_.close();
  }
}

void Logger::SetCallback(LogCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool to_console = level >= min_level_;
  const bool to_main = file_stream_.is_open() && level >= LogLevel::kInfo;
  const bool to_session = session_stream_.is_open();
  const bool to_cb = static_cast<bool>(callback_) && level >= min_level_;
  if (!to_console && !to_main && !to_session && !to_cb) {
    return;
  }

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

  if (to_console) {
    std::cout << LevelColor(level) << formatted << "\033[0m" << std::endl;
  }

  if (to_main) {
    file_stream_ << formatted << std::endl;
    RotateMainLogIfNeededLocked();
  }

  if (to_session) {
    session_stream_ << formatted << std::endl;
  }

  if (to_cb) {
    callback_(level, formatted);
  }
}

} // namespace castcore
