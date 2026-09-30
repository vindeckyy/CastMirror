// Test entry point: keeps the suite away from the user's real configuration.
//
// Several tests drive castmirror_init()/castmirror_shutdown() and mutate the
// process-wide ConfigStore, which persists to the default config path. Point
// that path at a throwaway directory so running the tests can never rewrite
// the settings of the machine they run on.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define GETPID _getpid
static void SetEnvVar(const char* name, const std::string& value) {
  _putenv_s(name, value.c_str());
}
#else
#include <unistd.h>
#define GETPID getpid
static void SetEnvVar(const char* name, const std::string& value) {
  setenv(name, value.c_str(), 1);
}
#endif

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("castmirror-tests-" + std::to_string(static_cast<long long>(GETPID())));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  SetEnvVar("CASTMIRROR_CONFIG_DIR", dir.string());
  // Test machines (and CI runners) may have no capturable desktop or audio
  // endpoint; sessions may then fall back to generated capture. Real sessions
  // never do.
  SetEnvVar("CASTMIRROR_ALLOW_SYNTHETIC_CAPTURE", "1");

  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();

  fs::remove_all(dir, ec);
  return result;
}
