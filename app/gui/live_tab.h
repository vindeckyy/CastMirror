#ifndef CASTMIRROR_GUI_LIVE_TAB_H_
#define CASTMIRROR_GUI_LIVE_TAB_H_

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

 private:
  void BuildUi();
  void UpdatePipelineDiagram(SessionState state, const StreamStats& stats);
  void UpdateLadderVisualization(int active_rung, int total_rungs, bool enabled);

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;
  GtkWidget* empty_card_ = nullptr;
  GtkWidget* live_container_ = nullptr;

  // Pipeline nodes
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

  // Health banner
  GtkWidget* health_banner_ = nullptr;
  GtkWidget* health_banner_lbl_ = nullptr;

  // Stat tiles
  GtkWidget* val_fps_ = nullptr;
  GtkWidget* val_bitrate_ = nullptr;
  GtkWidget* val_rtt_ = nullptr;
  GtkWidget* val_loss_ = nullptr;
  GtkWidget* val_delay_ = nullptr;
  GtkWidget* val_size_ = nullptr;
  GtkWidget* val_encoder_ = nullptr;
  GtkWidget* val_repairs_ = nullptr;
  GtkWidget* val_sent_ = nullptr;

  // Ladder
  GtkWidget* ladder_box_ = nullptr;
  GtkWidget* ladder_caption_lbl_ = nullptr;
  std::vector<GtkWidget*> ladder_rungs_;

  // Activity Timeline
  GtkWidget* timeline_list_ = nullptr;
  std::vector<std::string> timeline_items_;
  std::string last_health_hint_;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_LIVE_TAB_H_
