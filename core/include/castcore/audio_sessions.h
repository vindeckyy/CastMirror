#ifndef CASTCORE_AUDIO_SESSIONS_H_
#define CASTCORE_AUDIO_SESSIONS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace castcore {

// An application that has an audio session on the default playback device: a
// browser, a music player, a game. Used to let the user share one app's sound
// instead of everything the PC plays.
struct AudioApp {
  uint32_t pid = 0;
  std::string exe_name;  // "chrome.exe": stable across runs, so this is what is saved
  std::string title;  // what to show the user, e.g. the session's display name
};

// Lists apps with an audio session (active or idle) on the default render device,
// one entry per process, sorted by title. Empty on platforms without the feature.
std::vector<AudioApp> EnumerateAudioApps();

// The process id of a running app with an audio session, matched on exe name
// (case-insensitive). 0 when it is not running or has no session yet.
uint32_t FindAudioProcessByName(const std::string& exe_name);

}  // namespace castcore

#endif  // CASTCORE_AUDIO_SESSIONS_H_
