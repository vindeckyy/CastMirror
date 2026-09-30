#include "castcore/audio_sessions.h"
#include "castcore/logger.h"

#include <algorithm>
#include <cctype>
#include <map>

#if defined(_WIN32)
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace castcore {

#if defined(_WIN32)
namespace {

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// The file name of a process's executable, or empty when it cannot be opened
// (a protected or elevated process).
std::string ExeNameOf(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  wchar_t path[MAX_PATH * 2];
  DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
  std::string name;
  if (QueryFullProcessImageNameW(process, 0, path, &size)) {
    std::wstring full(path, size);
    size_t slash = full.find_last_of(L"\\/");
    name = Utf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
  }
  CloseHandle(process);
  return name;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

std::vector<AudioApp> EnumerateAudioApps() {
  std::vector<AudioApp> apps;

  // COM may not be initialised on the calling thread (a UI thread is fine, a plain
  // worker is not). Initialise for the duration of the call and balance it.
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool own_com = SUCCEEDED(init);
  {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioSessionManager2> manager;
    Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
    int count = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) &&
        SUCCEEDED(manager->GetSessionEnumerator(&sessions)) &&
        SUCCEEDED(sessions->GetCount(&count))) {
      std::map<uint32_t, AudioApp> by_pid;
      for (int i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IAudioSessionControl> control;
        Microsoft::WRL::ComPtr<IAudioSessionControl2> control2;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2))) continue;
        // The system sounds session and the app's own process are not choices.
        if (control2->IsSystemSoundsSession() == S_OK) continue;
        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId()) continue;

        AudioApp app;
        app.pid = pid;
        app.exe_name = ExeNameOf(pid);
        if (app.exe_name.empty()) continue;  // cannot be targeted or identified
        LPWSTR display = nullptr;
        if (SUCCEEDED(control->GetDisplayName(&display)) && display) {
          app.title = Utf8(display);
          CoTaskMemFree(display);
        }
        if (app.title.empty() || app.title[0] == '@') {
          // Many apps leave the display name empty or a resource reference ("@%SystemRoot%...").
          std::string stem = app.exe_name;
          if (Lower(stem).size() > 4 && Lower(stem).substr(stem.size() - 4) == ".exe") stem.resize(stem.size() - 4);
          app.title = stem;
        }
        by_pid.emplace(pid, std::move(app));
      }
      for (auto& [pid, app] : by_pid) apps.push_back(std::move(app));
    } else {
      LOG_WARN << "Could not list audio sessions on the default playback device";
    }
  }
  if (own_com) CoUninitialize();

  std::sort(apps.begin(), apps.end(), [](const AudioApp& a, const AudioApp& b) {
    return Lower(a.title) < Lower(b.title);
  });
  return apps;
}

uint32_t FindAudioProcessByName(const std::string& exe_name) {
  if (exe_name.empty()) return 0;
  const std::string wanted = Lower(exe_name);
  for (const AudioApp& app : EnumerateAudioApps()) {
    if (Lower(app.exe_name) == wanted) return app.pid;
  }
  return 0;
}

#else

std::vector<AudioApp> EnumerateAudioApps() { return {}; }
uint32_t FindAudioProcessByName(const std::string&) { return 0; }

#endif

}  // namespace castcore
