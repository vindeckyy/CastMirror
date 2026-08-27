#include <gtk/gtk.h>
#include "gui_app.h"
#include "css_loader.h"
#include "notify.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
extern "C" int XInitThreads(void);
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

static int g_gui_lock_fd = -1;

static bool AcquireGuiInstanceLock() {
  const char* home = std::getenv("HOME");
  std::string dir = (home ? std::string(home) : "/tmp") + "/.config/castmirror";
  mkdir(dir.c_str(), 0755);
  std::string lock_path = dir + "/castmirror-gui.lock";
  g_gui_lock_fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (g_gui_lock_fd < 0) return true;
  if (flock(g_gui_lock_fd, LOCK_EX | LOCK_NB) != 0) {
    close(g_gui_lock_fd);
    g_gui_lock_fd = -1;
    return false;
  }
  if (ftruncate(g_gui_lock_fd, 0) == 0) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(getpid()));
    (void)write(g_gui_lock_fd, buf, static_cast<size_t>(n));
  }
  return true;
}

static void ReleaseGuiInstanceLock() {
  if (g_gui_lock_fd >= 0) {
    flock(g_gui_lock_fd, LOCK_UN);
    close(g_gui_lock_fd);
    g_gui_lock_fd = -1;
  }
}
#endif

int main(int argc, char** argv) {
#if !defined(_WIN32)
  // Must run before gtk_init so X11 capture can share the display with GTK.
  XInitThreads();
  if (!AcquireGuiInstanceLock()) {
    gtk_init(&argc, &argv);
    GtkWidget* dialog = gtk_message_dialog_new(
        nullptr,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "CastMirror is already running.");
    gtk_window_set_title(GTK_WINDOW(dialog), "CastMirror");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return 1;
  }
#endif

  gtk_init(&argc, &argv);
  castcore::gui::ApplyApplicationTheme();
  castcore::gui::NotificationManager::Initialize();

  auto& engine = castcore::CastEngine::Instance();
  engine.Initialize();
  engine.StartDiscovery();

  castcore::gui::GuiApp app;
  app.Run();

#if !defined(_WIN32)
  ReleaseGuiInstanceLock();
#endif
  return 0;
}
