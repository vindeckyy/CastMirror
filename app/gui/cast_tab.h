#ifndef CASTMIRROR_GUI_CAST_TAB_H_
#define CASTMIRROR_GUI_CAST_TAB_H_

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include "castcore/types.h"

namespace castcore::gui {

class GuiApp;

class CastTab {
 public:
  explicit CastTab(GuiApp* app);
  ~CastTab();

  GtkWidget* GetRootWidget() const { return root_widget_; }

  void RefreshDevices();
  void RefreshDisplays();
  void UpdateSessionState(SessionState state, const std::string& message);

  void SetControlsSensitive(bool sensitive);
  void SyncAudioSwitch(bool active);

  std::string GetSelectedDeviceId() const { return selected_device_id_; }
  int GetSelectedDisplayId() const { return selected_display_id_; }
  QualityPreset GetSelectedPreset() const { return selected_preset_; }
  bool GetAudioEnabled() const;

 private:
  void BuildUi();
  void UpdateBitrateNote();
  void OnDeviceRowSelected(GtkListBox* box, GtkListBoxRow* row);
  void OnDisplayRowSelected(GtkListBox* box, GtkListBoxRow* row);
  void OnPresetChanged(QualityPreset preset);
  void OnAddIpClicked();

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;
  GtkWidget* device_list_box_ = nullptr;
  GtkWidget* empty_device_card_ = nullptr;
  GtkWidget* display_list_box_ = nullptr;
  GtkWidget* wayland_note_lbl_ = nullptr;
  GtkWidget* preset_auto_btn_ = nullptr;
  GtkWidget* preset_high_btn_ = nullptr;
  GtkWidget* preset_balanced_btn_ = nullptr;
  GtkWidget* preset_smooth_btn_ = nullptr;
  GtkWidget* bitrate_note_lbl_ = nullptr;
  GtkWidget* audio_switch_ = nullptr;
  GtkWidget* rescan_btn_ = nullptr;
  GtkWidget* add_ip_btn_ = nullptr;
  GtkWidget* remove_btn_ = nullptr;

  std::vector<CastDevice> devices_;
  std::vector<DisplayInfo> displays_;
  std::string selected_device_id_;
  int selected_display_id_ = 0;
  QualityPreset selected_preset_ = QualityPreset::kAuto;
  bool updating_ui_ = false;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_CAST_TAB_H_
