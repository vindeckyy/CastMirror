#include "logs_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "castcore/logger.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>

namespace castcore::gui {

namespace {

std::string GetLogDirectory() {
  const char* home = std::getenv("HOME");
  std::string dir = (home ? std::string(home) : "/tmp") + "/.config/castmirror";
  return dir;
}

}  // namespace

LogsTab::LogsTab(GuiApp* app) : app_(app) {
  BuildUi();
  SeedInitialLogs();
}

LogsTab::~LogsTab() = default;

void LogsTab::BuildUi() {
  GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 16);
  root_widget_ = main_vbox;

  // 1. Toolbar Box
  GtkWidget* tb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget* level_lbl = gtk_label_new("Minimum Level:");
  gtk_style_context_add_class(gtk_widget_get_style_context(level_lbl), "setting-title");
  gtk_box_pack_start(GTK_BOX(tb_box), level_lbl, FALSE, FALSE, 0);

  level_combo_ = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo_), "Debug");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo_), "Info");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo_), "Warn");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(level_combo_), "Error");

  LogLevel current_lvl = Logger::Instance().GetMinLevel();
  gtk_combo_box_set_active(GTK_COMBO_BOX(level_combo_), static_cast<int>(current_lvl));

  auto on_lvl = +[](GtkComboBox* combo, gpointer) {
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx <= 3) {
      Logger::Instance().SetMinLevel(static_cast<LogLevel>(idx));
    }
  };
  g_signal_connect(level_combo_, "changed", G_CALLBACK(on_lvl), nullptr);
  gtk_box_pack_start(GTK_BOX(tb_box), level_combo_, FALSE, FALSE, 0);

  // Copy All Button
  GtkWidget* copy_btn = gtk_button_new_with_label("📋 Copy all");
  gtk_widget_set_tooltip_text(copy_btn, "Copy entire log output to clipboard");
  auto on_copy = +[](GtkButton*, gpointer user_data) {
    auto* self = static_cast<LogsTab*>(user_data);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(self->text_buffer_, &start, &end);
    char* text = gtk_text_buffer_get_text(self->text_buffer_, &start, &end, FALSE);
    if (text) {
      GtkClipboard* clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
      gtk_clipboard_set_text(clip, text, -1);
      g_free(text);
    }
  };
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy), this);
  gtk_box_pack_end(GTK_BOX(tb_box), copy_btn, FALSE, FALSE, 0);

  // Open Folder Button
  GtkWidget* folder_btn = gtk_button_new_with_label("📁 Open log folder");
  gtk_widget_set_tooltip_text(folder_btn, "Open ~/.config/castmirror in file manager");
  auto on_folder = +[](GtkButton*, gpointer) {
    std::string dir = GetLogDirectory();
    std::string uri = "file://" + dir;
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
  };
  g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_folder), nullptr);
  gtk_box_pack_end(GTK_BOX(tb_box), folder_btn, FALSE, FALSE, 0);

  // Clear View Button
  GtkWidget* clear_btn = gtk_button_new_with_label("🗑 Clear view");
  gtk_widget_set_tooltip_text(clear_btn, "Clear current view buffer (does not delete log files)");
  auto on_clear = +[](GtkButton*, gpointer user_data) {
    auto* self = static_cast<LogsTab*>(user_data);
    gtk_text_buffer_set_text(self->text_buffer_, "", 0);
  };
  g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear), this);
  gtk_box_pack_end(GTK_BOX(tb_box), clear_btn, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(main_vbox), tb_box, FALSE, FALSE, 0);

  // Info label
  GtkWidget* path_info = gtk_label_new(
      "Live view of process logs. Session log: ~/.config/castmirror/castmirror-session.log  •  History: ~/.config/castmirror/castmirror.log");
  gtk_widget_set_halign(path_info, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(path_info), "card-desc");
  gtk_box_pack_start(GTK_BOX(main_vbox), path_info, FALSE, FALSE, 0);

  // 2. Monospace Scrolled Text View
  GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_style_context_add_class(gtk_widget_get_style_context(scrolled), "card-box");
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);

  text_view_ = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view_), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view_), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view_), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_NONE);
  gtk_style_context_add_class(gtk_widget_get_style_context(text_view_), "log-view");

  text_buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_));

  // Create tag colors
  gtk_text_buffer_create_tag(text_buffer_, "tag_debug", "foreground", "#8b93a1", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_info", "foreground", "#e8ecf1", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_warn", "foreground", "#f5c542", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_error", "foreground", "#ff6b6b", nullptr);

  gtk_container_add(GTK_CONTAINER(scrolled), text_view_);
  gtk_box_pack_start(GTK_BOX(main_vbox), scrolled, TRUE, TRUE, 0);
}

void LogsTab::SeedInitialLogs() {
  std::string dir = GetLogDirectory();
  std::string session_file = dir + "/castmirror-session.log";
  std::string main_file = dir + "/castmirror.log";
  std::string target_file = session_file;

  if (!std::filesystem::exists(session_file)) {
    target_file = main_file;
  }

  if (std::filesystem::exists(target_file)) {
    std::ifstream file(target_file, std::ios::binary);
    if (file.is_open()) {
      // Seek up to last 500 KB
      file.seekg(0, std::ios::end);
      size_t file_size = file.tellg();
      size_t read_bytes = std::min(file_size, static_cast<size_t>(500 * 1024));
      file.seekg(file_size - read_bytes);

      std::string buffer(read_bytes, '\0');
      file.read(&buffer[0], read_bytes);

      // Truncate partial first line if sought into middle
      if (read_bytes < file_size) {
        size_t first_nl = buffer.find('\n');
        if (first_nl != std::string::npos) {
          buffer = buffer.substr(first_nl + 1);
        }
      }

      gtk_text_buffer_set_text(text_buffer_, buffer.c_str(), -1);

      GtkTextIter end_iter;
      gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);
      gtk_text_buffer_place_cursor(text_buffer_, &end_iter);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &end_iter, 0.0, FALSE, 0.0, 0.0);
    }
  }
}

void LogsTab::OnLogMessage(LogLevel level, const std::string& formatted_line) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  pending_queue_.push_back({level, formatted_line});
  if (pending_queue_.size() > 2000) {
    pending_queue_.pop_front();
  }

  if (!idle_scheduled_) {
    idle_scheduled_ = true;
    g_idle_add(+[](gpointer user_data) -> gboolean {
      auto* self = static_cast<LogsTab*>(user_data);
      self->FlushPendingLogs();
      return G_SOURCE_REMOVE;
    }, this);
  }
}

void LogsTab::FlushPendingLogs() {
  std::deque<PendingLog> to_drain;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    to_drain.swap(pending_queue_);
    idle_scheduled_ = false;
  }

  if (to_drain.empty()) return;

  GtkTextIter end_iter;
  gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);

  for (const auto& item : to_drain) {
    const char* tag_name = "tag_info";
    if (item.level == LogLevel::kDebug) tag_name = "tag_debug";
    else if (item.level == LogLevel::kWarn) tag_name = "tag_warn";
    else if (item.level == LogLevel::kError || item.level == LogLevel::kFatal) tag_name = "tag_error";

    std::string text_with_nl = item.text + "\n";
    gtk_text_buffer_insert_with_tags_by_name(text_buffer_, &end_iter, text_with_nl.c_str(), -1, tag_name, nullptr);
  }

  gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);
  gtk_text_buffer_place_cursor(text_buffer_, &end_iter);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &end_iter, 0.0, FALSE, 0.0, 0.0);
}

}  // namespace castcore::gui
