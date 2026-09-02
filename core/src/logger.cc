#include "castcore/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <atomic>

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
  if (json_stream_.is_open()) {
    json_stream_.close();
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

void Logger::RotateJsonLogIfNeededLocked() {
  if (!json_stream_.is_open() || json_log_path_.empty()) {
    return;
  }
  json_write_count_++;
  if ((json_write_count_ % 32) != 0) {
    return;
  }
  json_stream_.flush();
  std::error_code ec;
  auto sz = std::filesystem::file_size(json_log_path_, ec);
  if (ec || sz < kMaxJsonLogBytes) {
    return;
  }
  json_stream_.close();
  std::string rotated = json_log_path_ + ".old";
  std::filesystem::remove(rotated, ec);
  std::filesystem::rename(json_log_path_, rotated, ec);
  json_stream_.open(json_log_path_, std::ios::out | std::ios::trunc);
  json_write_count_ = 0;
}

std::string Logger::RedactIpAddresses(const std::string& input) const {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  const size_t n = input.size();
  while (i < n) {
    if (std::isdigit(static_cast<unsigned char>(input[i]))) {
      size_t j = i;
      bool ok = true;
      int groups = 0;
      size_t scan = j;
      for (int g = 0; g < 4; ++g) {
        if (scan >= n || !std::isdigit(static_cast<unsigned char>(input[scan]))) { ok = false; break; }
        int val = 0;
        int digits = 0;
        while (scan < n && std::isdigit(static_cast<unsigned char>(input[scan]))) {
          val = val * 10 + (input[scan] - '0');
          ++digits;
          ++scan;
          if (digits > 3) { ok = false; break; }
        }
        if (!ok) break;
        if (val > 255 || digits == 0) { ok = false; break; }
        ++groups;
        if (g < 3) {
          if (scan >= n || input[scan] != '.') { ok = false; break; }
          ++scan;
        }
      }
      if (ok && groups == 4) {
        if (scan < n && (std::isdigit(static_cast<unsigned char>(input[scan])) || input[scan] == '.')) {
          // Not a clean boundary, treat as not IP
          out.push_back(input[i]);
          ++i;
          continue;
        }
        // Ensure previous char is not digit/dot (to avoid matching part of longer number)
        // Already at start of digit run, so fine.
        out += "[REDACTED]";
        i = scan;
        continue;
      }
    }
    out.push_back(input[i]);
    ++i;
  }
  return out;
}

bool Logger::ShouldEmitJson() const {
  if (verbose_json_enabled_.load()) return true;
  const char* env = std::getenv("CASTMIRROR_VERBOSE_JSON");
  if (env && (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 || std::strcmp(env, "TRUE") == 0)) {
    return true;
  }
  return false;
}

std::string Logger::GetDefaultJsonPath() {
#if defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  std::string base = appdata ? std::string(appdata) : "C:\\ProgramData";
  return base + "\\CastMirror\\castmirror.ndjson";
#else
  const char* home = std::getenv("HOME");
  std::string base = home ? std::string(home) : "/tmp";
  return base + "/.config/castmirror/castmirror.ndjson";
#endif
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
    std::filesystem::create_directories(std::filesystem::path(file_path).parent_path());
  } catch (...) {}
  file_stream_.open(file_path, std::ios::out | std::ios::app);
}

void Logger::SetSessionFileLogging(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_stream_.is_open()) {
    session_stream_.close();
  }
  try {
    std::filesystem::create_directories(std::filesystem::path(file_path).parent_path());
  } catch (...) {}
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

void Logger::SetJsonLogging(const std::string& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (json_stream_.is_open()) {
    json_stream_.close();
  }
  json_log_path_ = file_path;
  json_write_count_ = 0;
  try {
    std::filesystem::create_directories(std::filesystem::path(file_path).parent_path());
    if (std::filesystem::exists(file_path) &&
        std::filesystem::file_size(file_path) >= kMaxJsonLogBytes) {
      std::string rotated = file_path + ".old";
      std::error_code ec;
      std::filesystem::remove(rotated, ec);
      std::filesystem::rename(file_path, rotated, ec);
    }
  } catch (...) {}
  json_stream_.open(file_path, std::ios::out | std::ios::app);
}

void Logger::ClearJsonLogging() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (json_stream_.is_open()) {
    json_stream_.close();
  }
  json_log_path_.clear();
  json_write_count_ = 0;
}

void Logger::SetVerboseJsonEnabled(bool enabled) {
  std::string path_to_open;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    verbose_json_enabled_.store(enabled);
    if (enabled && !json_stream_.is_open()) {
      if (json_log_path_.empty()) {
        json_log_path_ = GetDefaultJsonPath();
      }
      path_to_open = json_log_path_;
    }
    if (!enabled) {
      // Keep stream open but LogJson will gate on ShouldEmitJson; optionally keep file.
      // Do not close immediately to avoid churn; but if env var not set, writes will be dropped.
    }
  }
  if (!path_to_open.empty()) {
    // Open outside lock to avoid nested lock issues, but SetJsonLogging will lock again
    SetJsonLogging(path_to_open);
  }
  if (enabled) {
    LOG_INFO << "Verbose JSON diagnostics enabled -> " << GetDefaultJsonPath();
  }
}

bool Logger::IsVerboseJsonEnabled() const {
  return ShouldEmitJson();
}

void Logger::LogJson(const std::string& json_line) {
  if (!ShouldEmitJson()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!json_stream_.is_open()) {
    std::string path = json_log_path_.empty() ? GetDefaultJsonPath() : json_log_path_;
    json_log_path_ = path;
    try {
      std::filesystem::create_directories(std::filesystem::path(path).parent_path());
      if (std::filesystem::exists(path) &&
          std::filesystem::file_size(path) >= kMaxJsonLogBytes) {
        std::string rotated = path + ".old";
        std::error_code ec;
        std::filesystem::remove(rotated, ec);
        std::filesystem::rename(path, rotated, ec);
      }
    } catch (...) {}
    json_stream_.open(path, std::ios::out | std::ios::app);
    json_write_count_ = 0;
    if (!json_stream_.is_open()) {
      return;
    }
  }
  json_stream_ << json_line << "\n";
  json_stream_.flush();
  RotateJsonLogIfNeededLocked();
}

void Logger::LogBreadcrumb(uint32_t frame_id, int64_t encode_ms, uint32_t udp_bytes, double rtt_ms, uint32_t nack_count) {
  LogBreadcrumbEx(frame_id, encode_ms, udp_bytes, rtt_ms, nack_count,
                  "capture->gpu->encode->crypto->rtp->udp", "udp");
}

void Logger::LogBreadcrumbEx(uint32_t frame_id, int64_t encode_ms, uint32_t udp_bytes, double rtt_ms, uint32_t nack_count,
                             const std::string& pipeline, const std::string& stage) {
  if (!ShouldEmitJson()) return;
  auto now = std::chrono::system_clock::now();
  int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  std::ostringstream ss;
  ss << "{\"ts\":" << ts_ms
     << ",\"frame_id\":" << frame_id
     << ",\"pipeline\":\"" << pipeline << "\""
     << ",\"stage\":\"" << stage << "\""
     << ",\"encode_ms\":" << encode_ms
     << ",\"udp_bytes\":" << udp_bytes
     << ",\"rtt_ms\":" << rtt_ms
     << ",\"nack_count\":" << nack_count
     << "}";
  LogJson(ss.str());
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
  std::string redacted = RedactIpAddresses(formatted);

  if (to_console) {
    std::cout << LevelColor(level) << formatted << "\033[0m" << std::endl;
  }

  if (to_main) {
    file_stream_ << redacted << std::endl;
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
