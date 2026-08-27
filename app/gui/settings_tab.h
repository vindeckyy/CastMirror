#ifndef CASTMIRROR_GUI_SETTINGS_TAB_H_
#define CASTMIRROR_GUI_SETTINGS_TAB_H_

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

 private:
  void BuildUi();
  void UpdateBitrateLabel(uint32_t kbps);

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;

  // Picture Controls
  GtkWidget* bitrate_scale_ = nullptr;
  GtkWidget* bitrate_val_lbl_ = nullptr;
  GtkWidget* fps_spin_ = nullptr;
  GtkWidget* adaptive_switch_ = nullptr;

  // Sound Controls
  GtkWidget* audio_switch_ = nullptr;
  GtkWidget* audio_quality_combo_ = nullptr;
  GtkWidget* silence_switch_ = nullptr;

  // Latency Controls
  GtkWidget* delay_scale_ = nullptr;
  GtkWidget* low_latency_switch_ = nullptr;

  // Discovery Controls
  GtkWidget* subnet_scan_switch_ = nullptr;

  // Advanced Capture / Encode
  GtkWidget* force_x11_switch_ = nullptr;
  GtkWidget* force_software_switch_ = nullptr;

  // Desktop Integration
  GtkWidget* tray_switch_ = nullptr;
  GtkWidget* close_to_tray_switch_ = nullptr;
  GtkWidget* notify_switch_ = nullptr;

  bool updating_bitrate_ = false;
  guint bitrate_debounce_id_ = 0;
  uint32_t pending_bitrate_kbps_ = 0;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_SETTINGS_TAB_H_
