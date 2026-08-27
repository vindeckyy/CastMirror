#ifndef CASTMIRROR_GUI_GUI_APP_H_
#define CASTMIRROR_GUI_GUI_APP_H_

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <mutex>
#include "castcore/types.h"

namespace castcore::gui {

class CastTab;
class LiveTab;
class SettingsTab;
class LogsTab;
class TrayManager;

class GuiApp {
 public:
  GuiApp();
  ~GuiApp();

  void Run();
  void Quit();

  GtkWindow* GetWindow() const { return GTK_WINDOW(window_); }
  SessionState GetCurrentState() const { return current_state_; }

  void SwitchToTab(int tab_index);
  void TriggerCastAction();
  void TriggerRescan();

  void SyncAudioEnabled(bool enabled);
  void SyncBitrateSlider(uint32_t kbps);

  CastTab* GetCastTab() const { return cast_tab_.get(); }
  LiveTab* GetLiveTab() const { return live_tab_.get(); }
  SettingsTab* GetSettingsTab() const { return settings_tab_.get(); }
  LogsTab* GetLogsTab() const { return logs_tab_.get(); }
  TrayManager* GetTrayManager() const { return tray_manager_.get(); }

 private:
  void BuildUi();
  void SetupAccelerators();
  void SetupEngineCallbacks();
  void SetupLoggerCallback();
  void UpdateStateUi(SessionState new_state, const std::string& message);

  static gboolean OnStatsTimer(gpointer user_data);
  static gboolean OnConfigureEvent(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data);
  static gboolean OnDeleteEvent(GtkWidget* widget, GdkEvent* event, gpointer user_data);

  GtkWidget* window_ = nullptr;
  GtkWidget* header_box_ = nullptr;
  GtkWidget* header_subtitle_ = nullptr;
  GtkWidget* status_badge_ = nullptr;
  GtkWidget* status_badge_lbl_ = nullptr;
  GtkWidget* notebook_ = nullptr;
  GtkWidget* footer_box_ = nullptr;
  GtkWidget* cast_button_ = nullptr;
  GtkWidget* cast_button_lbl_ = nullptr;
  GtkWidget* spinner_ = nullptr;
  GtkWidget* view_live_btn_ = nullptr;

  std::unique_ptr<CastTab> cast_tab_;
  std::unique_ptr<LiveTab> live_tab_;
  std::unique_ptr<SettingsTab> settings_tab_;
  std::unique_ptr<LogsTab> logs_tab_;
  std::unique_ptr<TrayManager> tray_manager_;

  SessionState current_state_ = SessionState::kIdle;
  std::string last_state_message_;
  std::string last_device_name_;
  guint resize_debounce_timer_ = 0;
  guint stats_timer_id_ = 0;
  guint rescan_timer_id_ = 0;
  bool is_quitting_ = false;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_GUI_APP_H_
