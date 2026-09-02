#ifndef CASTMIRROR_GUI_LOGS_TAB_H_
#define CASTMIRROR_GUI_LOGS_TAB_H_

#include <adwaita.h>
#include <gtk/gtk.h>
#include <deque>
#include <mutex>
#include <string>
#include "castcore/logger.h"

namespace castcore::gui {

class GuiApp;

class LogsTab {
 public:
  explicit LogsTab(GuiApp* app);
  ~LogsTab();

  LogsTab(const LogsTab&) = delete;
  LogsTab& operator=(const LogsTab&) = delete;

  GtkWidget* GetRootWidget() const { return root_widget_; }

  void OnLogMessage(LogLevel level, const std::string& formatted_line);
  void FlushPendingLogs();

 private:
  void BuildUi();
  void SeedInitialLogs();
  void UpdateBufferState();
  void OnCopyClicked();
  void OnClearClicked();
  void OnFilterChanged();
  void OnCopyLast100Clicked();
  void ApplyFilter();
  bool IsJsonSidecarPresent() const;
  void UpdateCopyLast100Sensitivity();

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;
  GtkWidget* text_view_ = nullptr;
  GtkTextBuffer* text_buffer_ = nullptr;
  GtkWidget* level_dropdown_ = nullptr;
  GtkWidget* filter_entry_ = nullptr;
  GtkWidget* copy_button_ = nullptr;
  GtkWidget* copy_last_100_button_ = nullptr;
  GtkWidget* folder_button_ = nullptr;
  GtkWidget* clear_button_ = nullptr;
  GtkWidget* path_info_label_ = nullptr;
  GtkWidget* seed_warning_banner_ = nullptr;
  GtkWidget* empty_state_box_ = nullptr;
  GtkWidget* scrolled_window_ = nullptr;

  struct PendingLog {
    LogLevel level;
    std::string text;
  };
  std::mutex queue_mutex_;
  std::deque<PendingLog> pending_queue_;
  std::deque<PendingLog> history_;
  std::string filter_text_;
  bool idle_scheduled_ = false;
  static constexpr size_t kMaxHistory = 5000;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_LOGS_TAB_H_
