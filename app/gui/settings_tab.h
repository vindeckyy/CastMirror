#ifndef CASTMIRROR_GUI_SETTINGS_TAB_H_
#define CASTMIRROR_GUI_SETTINGS_TAB_H_

#include <adwaita.h>
#include <gtk/gtk.h>
#include "castcore/types.h"

namespace castcore::gui {

class GuiApp;

class SettingsTab {
 public:
  explicit SettingsTab(GuiApp* app);
  ~SettingsTab();

  GtkWidget* GetRootWidget() const { return root_widget_; }

  void UpdateSessionState(SessionState state);
  void SyncBitrateSlider(uint32_t kbps);
  void SyncAudioSwitch(bool active);
  void SyncSilenceSwitch(bool active);

 private:
  void BuildUi();
  void UpdateBitrateLabel(uint32_t kbps);
  void UpdateDelayLabel(int ms);
  void UpdateDependentSensitivities();
  void RunSelfTestDialog();

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;

  // Picture & encoding
  GtkWidget* bitrate_row_ = nullptr;
  GtkWidget* bitrate_scale_ = nullptr;
  GtkWidget* bitrate_val_lbl_ = nullptr;
  GtkWidget* fps_row_ = nullptr;

  // Audio
  GtkWidget* audio_row_ = nullptr;
  GtkWidget* audio_quality_row_ = nullptr;
  GtkWidget* silence_row_ = nullptr;

  // Latency & buffering
  GtkWidget* delay_row_ = nullptr;
  GtkWidget* delay_scale_ = nullptr;
  GtkWidget* delay_val_lbl_ = nullptr;
  GtkWidget* latency_hud_row_ = nullptr;

  // Device discovery
  GtkWidget* subnet_scan_row_ = nullptr;

  // Advanced
  GtkWidget* force_x11_row_ = nullptr;
  GtkWidget* force_software_row_ = nullptr;

  // Desktop integration
  GtkWidget* tray_row_ = nullptr;
  GtkWidget* close_to_tray_row_ = nullptr;
  GtkWidget* notify_row_ = nullptr;

  bool syncing_controls_ = false;
  guint bitrate_debounce_id_ = 0;
  uint32_t pending_bitrate_kbps_ = 0;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_SETTINGS_TAB_H_
