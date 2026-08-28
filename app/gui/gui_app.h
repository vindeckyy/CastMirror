#ifndef CASTMIRROR_GUI_GUI_APP_H_
#define CASTMIRROR_GUI_GUI_APP_H_

#include <adwaita.h>
#include <memory>
#include <string>
#include "castcore/types.h"

namespace castcore::gui {

class CastTab;
class LiveTab;
class SettingsTab;
class LogsTab;
class TrayManager;

class GuiApp {
 public:
  explicit GuiApp(AdwApplication* application);
  ~GuiApp();

  GuiApp(const GuiApp&) = delete;
  GuiApp& operator=(const GuiApp&) = delete;

  void Present();
  void Quit();
  void Shutdown();

  GtkWindow* GetWindow() const { return GTK_WINDOW(window_); }
  SessionState GetCurrentState() const { return current_state_; }

  void SwitchToPage(const char* page_name);
  void TriggerCastAction();
  void TriggerRescan();

  void SyncAudioEnabled(bool enabled);
  void SyncSilenceHost(bool enabled);
  void SyncBitrateSlider(uint32_t kbps);

  void ShowToast(const std::string& title);
  void OnDestinationSelectionChanged();
  void PushModalActionBlock();
  void PopModalActionBlock();
  bool IsActiveSessionAudioEnabled() const { return active_session_audio_enabled_; }

  CastTab* GetCastTab() const { return cast_tab_.get(); }
  LiveTab* GetLiveTab() const { return live_tab_.get(); }
  SettingsTab* GetSettingsTab() const { return settings_tab_.get(); }
  LogsTab* GetLogsTab() const { return logs_tab_.get(); }
  TrayManager* GetTrayManager() const { return tray_manager_.get(); }

 private:
  void BuildUi();
  void SetupActions();
  void SetupEngineCallbacks();
  void SetupLoggerCallback();
  void UpdateStateUi(SessionState new_state, const std::string& message);
  void RefreshWindowActionSensitivity();
  void SaveWindowGeometry();
  void UpdateViewLiveVisibility();
  void SetFooterStatus(const std::string& text);
  void ApplyPrimaryAction(const char* label,
                          const char* icon_name,
                          bool destructive,
                          bool sensitive);
  void PresentAlert(const char* heading, const char* body);
  void ShowAboutDialog();
  void OpenConfigFolder();
  void OpenLogsFolder();
  bool IsSessionActive() const;
  const char* VisiblePageName() const;
  void OnPageAction(const char* page_name);

  static gboolean OnStatsTimer(gpointer user_data);
  static gboolean OnCloseRequest(GtkWindow* window, gpointer user_data);

  AdwApplication* application_ = nullptr;
  GtkWidget* window_ = nullptr;
  GtkWidget* toast_overlay_ = nullptr;
  GtkWidget* header_title_ = nullptr;
  GtkWidget* status_badge_ = nullptr;
  GtkWidget* status_badge_dot_ = nullptr;
  GtkWidget* status_badge_lbl_ = nullptr;
  GtkWidget* view_stack_ = nullptr;
  GtkWidget* view_switcher_ = nullptr;
  GtkWidget* footer_status_label_ = nullptr;
  GtkWidget* cast_button_ = nullptr;
  GtkWidget* cast_button_icon_ = nullptr;
  GtkWidget* cast_button_lbl_ = nullptr;
  GtkWidget* spinner_ = nullptr;
  GtkWidget* view_live_btn_ = nullptr;

  GSimpleAction* action_cast_ = nullptr;
  GSimpleAction* action_rescan_ = nullptr;
  GSimpleAction* action_add_ip_ = nullptr;
  GSimpleAction* action_remove_device_ = nullptr;
  GSimpleAction* action_page_ = nullptr;

  std::unique_ptr<CastTab> cast_tab_;
  std::unique_ptr<LiveTab> live_tab_;
  std::unique_ptr<SettingsTab> settings_tab_;
  std::unique_ptr<LogsTab> logs_tab_;
  std::unique_ptr<TrayManager> tray_manager_;

  SessionState current_state_ = SessionState::kIdle;
  std::string last_state_message_;
  std::string last_failed_message_;
  std::string last_device_name_;
  bool active_session_audio_enabled_ = false;
  bool scan_in_progress_ = false;
  bool first_presented_ = false;
  int modal_action_block_count_ = 0;
  guint stats_timer_id_ = 0;
  guint rescan_timer_id_ = 0;
  bool is_quitting_ = false;
  bool shutdown_done_ = false;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_GUI_APP_H_
