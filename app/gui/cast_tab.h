#ifndef CASTMIRROR_GUI_CAST_TAB_H_
#define CASTMIRROR_GUI_CAST_TAB_H_

#include <adwaita.h>
#include <gtk/gtk.h>
#include <string>
#include <unordered_map>
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
  void RefreshWindows();
  void UpdateSessionState(SessionState state, const std::string& message);

  void SetControlsSensitive(bool sensitive);
  void SetScanInProgress(bool scanning);

  std::string GetSelectedDeviceId() const { return selected_device_id_; }
  std::string GetSelectedDeviceName() const;
  int GetSelectedDisplayId() const { return selected_display_id_; }
  std::string GetSelectedDisplayName() const;
  bool HasSelectedDisplay() const { return has_selected_display_; }
  bool HasSelectedSource() const;
  CaptureSource GetSelectedSource() const;
  bool IsSelectedDeviceRemovable() const;
  QualityPreset GetSelectedPreset() const { return selected_preset_; }
  bool GetAudioEnabled() const;

  void OnAddIpClicked();
  void RemoveSelectedDevice();
  void SyncInlineBitrate(uint32_t kbps);

 private:
  struct DeviceRowWidgets {
    GtkWidget* row = nullptr;
    GtkWidget* title_lbl = nullptr;
    GtkWidget* sub1_lbl = nullptr;
    GtkWidget* sub2_lbl = nullptr;
    GtkWidget* status_pill = nullptr;
    GtkWidget* status_dot = nullptr;
    GtkWidget* status_lbl = nullptr;
    GtkWidget* select_icon = nullptr;
    int rank = 0;
    CastDevice device;
  };

  struct DisplayRowWidgets {
    GtkWidget* row = nullptr;
    GtkWidget* title_lbl = nullptr;
    GtkWidget* sub_lbl = nullptr;
    GtkWidget* primary_pill = nullptr;
    GtkWidget* select_icon = nullptr;
    int rank = 0;
    DisplayInfo display;
  };

  struct WindowRowWidgets {
    GtkWidget* row = nullptr;
    GtkWidget* title_lbl = nullptr;
    GtkWidget* sub_lbl = nullptr;
    GtkWidget* select_icon = nullptr;
    int rank = 0;
    WindowInfo window;
  };

  void BuildUi();
  void UpdateBitrateNote();
  void UpdateDeviceSectionState();
  DeviceRowWidgets CreateDeviceRow(const CastDevice& dev, int rank);
  DisplayRowWidgets CreateDisplayRow(const DisplayInfo& disp, int rank);
  WindowRowWidgets CreateWindowRow(const WindowInfo& win, int rank);
  int GetDeviceRank(const std::string& id) const;
  int GetDisplayRank(int id) const;
  void UpdateSourceToggleVisibility();
  void OnSourceKindChanged(CaptureSourceKind kind);
  void OnWindowRowSelected(GtkListBox* box, GtkListBoxRow* row);
  void StartWindowRefreshTimer();
  void StopWindowRefreshTimer();

  void OnDeviceRowSelected(GtkListBox* box, GtkListBoxRow* row);
  void OnDisplayRowSelected(GtkListBox* box, GtkListBoxRow* row);
  void OnPresetChanged(QualityPreset preset);
  void SaveSelectedDeviceProfile();

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;
  GtkWidget* dev_section_ = nullptr;
  GtkWidget* dev_header_actions_ = nullptr;
  GtkWidget* rescan_btn_ = nullptr;
  GtkWidget* add_ip_btn_ = nullptr;
  GtkWidget* remove_btn_ = nullptr;
  GtkWidget* dev_loading_box_ = nullptr;
  GtkWidget* dev_empty_box_ = nullptr;
  GtkWidget* device_list_box_ = nullptr;

  GtkWidget* disp_section_ = nullptr;
  GtkWidget* disp_empty_box_ = nullptr;
  GtkWidget* display_list_box_ = nullptr;
  GtkWidget* wayland_banner_ = nullptr;

  // Source selector (Screen / Window segmented toggle + window list).
  GtkWidget* source_toggle_box_ = nullptr;
  GtkWidget* source_screen_btn_ = nullptr;
  GtkWidget* source_window_btn_ = nullptr;
  GtkWidget* window_list_box_ = nullptr;
  GtkWidget* window_empty_box_ = nullptr;
  guint window_refresh_timer_id_ = 0;

  GtkWidget* preset_flow_box_ = nullptr;
  GtkWidget* preset_auto_btn_ = nullptr;
  GtkWidget* preset_high_btn_ = nullptr;
  GtkWidget* preset_balanced_btn_ = nullptr;
  GtkWidget* preset_smooth_btn_ = nullptr;
  GtkWidget* preset_game_btn_ = nullptr;
  GtkWidget* preset_cinema_btn_ = nullptr;
  GtkWidget* bitrate_note_lbl_ = nullptr;
  GtkWidget* inline_bitrate_scale_ = nullptr;
  GtkWidget* inline_bitrate_val_lbl_ = nullptr;

  std::vector<CastDevice> devices_;
  std::vector<DisplayInfo> displays_;
  std::vector<WindowInfo> windows_;
  std::unordered_map<std::string, DeviceRowWidgets> device_row_widgets_;
  std::unordered_map<int, DisplayRowWidgets> display_row_widgets_;
  std::unordered_map<int, WindowRowWidgets> window_row_widgets_;

  std::string selected_device_id_;
  int selected_display_id_ = 0;
  bool has_selected_display_ = false;
  CaptureSourceKind selected_source_kind_ = CaptureSourceKind::kMonitor;
  int selected_window_id_ = 0;
  std::string selected_window_name_;
  bool has_selected_window_ = false;
  QualityPreset selected_preset_ = QualityPreset::kAuto;
  bool updating_ui_ = false;
  bool scan_in_progress_ = false;
  bool session_controls_sensitive_ = true;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_CAST_TAB_H_
