#ifndef CASTMIRROR_GUI_LIVE_TAB_H_
#define CASTMIRROR_GUI_LIVE_TAB_H_

#include <adwaita.h>
#include <gtk/gtk.h>
#include <string>
#include <vector>
#include "castcore/types.h"

namespace castcore::gui {

class GuiApp;

class LiveTab {
 public:
  explicit LiveTab(GuiApp* app);
  ~LiveTab();

  GtkWidget* GetRootWidget() const { return root_widget_; }

  void UpdateStats(const StreamStats& stats);
  void UpdateSessionState(SessionState state, const std::string& message);
  void AppendActivityEvent(const std::string& message);
  void ResetSessionValues();
  const StreamStats& LastStats() const { return last_stats_; }

 private:
  void BuildUi();
  void UpdatePipelineDiagram(SessionState state, const StreamStats& stats);
  void UpdateLadderVisualization(int active_rung, int total_rungs, bool enabled);
  void SetHealthState(const std::string& text, const char* state_class, const char* icon_name);

  void OnGoToCastClicked();
  void OnBackToCastClicked();

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;  // GtkStack

  // Pages in GtkStack
  GtkWidget* empty_page_ = nullptr;   // AdwStatusPage
  GtkWidget* failed_page_ = nullptr;  // AdwStatusPage
  GtkWidget* session_scroller_ = nullptr;  // GtkScrolledWindow

  // Hero section
  GtkWidget* hero_card_ = nullptr;
  GtkWidget* hero_icon_ = nullptr;
  GtkWidget* hero_title_lbl_ = nullptr;
  GtkWidget* hero_subtitle_lbl_ = nullptr;
  GtkWidget* hero_status_pill_ = nullptr;
  GtkWidget* hero_status_dot_ = nullptr;
  GtkWidget* hero_status_lbl_ = nullptr;

  // Pipeline diagram
  GtkWidget* pipe_card_ = nullptr;
  GtkWidget* pipe_screen_node_ = nullptr;
  GtkWidget* pipe_screen_sub_ = nullptr;
  GtkWidget* pipe_capture_node_ = nullptr;
  GtkWidget* pipe_capture_sub_ = nullptr;
  GtkWidget* pipe_encode_node_ = nullptr;
  GtkWidget* pipe_encode_sub_ = nullptr;
  GtkWidget* pipe_network_node_ = nullptr;
  GtkWidget* pipe_network_sub_ = nullptr;
  GtkWidget* pipe_tv_node_ = nullptr;
  GtkWidget* pipe_tv_sub_ = nullptr;

  // Health card
  GtkWidget* health_card_ = nullptr;
  GtkWidget* health_icon_ = nullptr;
  GtkWidget* health_lbl_ = nullptr;

  // 9 Stat tiles
  GtkWidget* val_fps_ = nullptr;
  GtkWidget* val_bitrate_ = nullptr;
  GtkWidget* val_rtt_ = nullptr;
  GtkWidget* val_loss_ = nullptr;
  GtkWidget* val_delay_ = nullptr;
  GtkWidget* val_size_ = nullptr;
  GtkWidget* val_encoder_ = nullptr;
  GtkWidget* val_repairs_ = nullptr;
  GtkWidget* val_sent_ = nullptr;

  // Adaptive quality ladder
  GtkWidget* ladder_box_ = nullptr;
  GtkWidget* ladder_caption_lbl_ = nullptr;
  std::vector<GtkWidget*> ladder_rungs_;

  // Activity timeline
  GtkWidget* timeline_list_ = nullptr;
  std::vector<std::string> timeline_items_;
  std::string last_health_hint_;

  // Cached state
  StreamStats last_stats_{};
  std::string last_device_name_;
  SessionState current_ui_state_ = SessionState::kIdle;
  bool failure_visible_ = false;
};

}  // namespace castcore::gui


#endif  // CASTMIRROR_GUI_LIVE_TAB_H_
