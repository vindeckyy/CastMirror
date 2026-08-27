#ifndef CASTMIRROR_GUI_LOGS_TAB_H_
#define CASTMIRROR_GUI_LOGS_TAB_H_

#include <gtk/gtk.h>
#include <string>
#include <deque>
#include <mutex>
#include "castcore/logger.h"

namespace castcore::gui {

class GuiApp;

class LogsTab {
 public:
  explicit LogsTab(GuiApp* app);
  ~LogsTab();

  GtkWidget* GetRootWidget() const { return root_widget_; }

  void OnLogMessage(LogLevel level, const std::string& formatted_line);
  void FlushPendingLogs();

 private:
  void BuildUi();
  void SeedInitialLogs();
  void AppendLogText(const std::string& text, LogLevel level);

  GuiApp* app_ = nullptr;
  GtkWidget* root_widget_ = nullptr;
  GtkWidget* text_view_ = nullptr;
  GtkTextBuffer* text_buffer_ = nullptr;
  GtkWidget* level_combo_ = nullptr;

  struct PendingLog {
    LogLevel level;
    std::string text;
  };
  std::mutex queue_mutex_;
  std::deque<PendingLog> pending_queue_;
  bool idle_scheduled_ = false;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_LOGS_TAB_H_
