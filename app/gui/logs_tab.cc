#include "logs_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "castcore/logger.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <deque>
#include <gio/gio.h>
#include <gtk/gtk.h>

namespace castcore::gui {

namespace {

std::string GetLogDirectory() {
  const char* home = std::getenv("HOME");
  std::string dir = (home ? std::string(home) : "/tmp") + "/.config/castmirror";
  return dir;
}

std::string ToLower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
  return out;
}

}  // namespace

LogsTab::LogsTab(GuiApp* app) : app_(app) {
  BuildUi();
  SeedInitialLogs();
}

LogsTab::~LogsTab() = default;

void LogsTab::BuildUi() {
  GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(main_vbox, "cm-page-content");
  root_widget_ = main_vbox;

  // 1. Toolbar Box
  GtkWidget* tb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(tb_box, "cm-log-toolbar");

  GtkWidget* level_lbl = gtk_label_new("Minimum level");
  gtk_widget_add_css_class(level_lbl, "cm-section-title");
  gtk_box_append(GTK_BOX(tb_box), level_lbl);

  const char* const levels[] = {"Debug", "Info", "Warn", "Error", nullptr};
  level_dropdown_ = gtk_drop_down_new_from_strings(levels);
  gtk_accessible_update_property(GTK_ACCESSIBLE(level_dropdown_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Minimum level",
                                 -1);

  LogLevel current_lvl = Logger::Instance().GetMinLevel();
  gtk_drop_down_set_selected(GTK_DROP_DOWN(level_dropdown_),
                             static_cast<guint>(current_lvl));

  g_signal_connect(level_dropdown_, "notify::selected",
                   G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer) {
                     guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
                     if (selected <= 3 && selected != GTK_INVALID_LIST_POSITION) {
                       Logger::Instance().SetMinLevel(static_cast<LogLevel>(selected));
                     }
                   }),
                   nullptr);
  gtk_box_append(GTK_BOX(tb_box), level_dropdown_);

  // Filter entry
  filter_entry_ = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(filter_entry_), "Filter logs…");
  gtk_widget_set_size_request(filter_entry_, 240, -1);
  gtk_widget_set_tooltip_text(filter_entry_, "Filter displayed log lines");
  gtk_accessible_update_property(GTK_ACCESSIBLE(filter_entry_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Filter logs",
                                 -1);
  g_signal_connect(filter_entry_, "search-changed",
                   G_CALLBACK(+[](GtkSearchEntry*, gpointer user_data) {
                     auto* self = static_cast<LogsTab*>(user_data);
                     self->OnFilterChanged();
                   }),
                   this);
  gtk_box_append(GTK_BOX(tb_box), filter_entry_);

  GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(tb_box), spacer);

  // Copy last 100 lines button (if sidecar present)
  copy_last_100_button_ = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_add_css_class(copy_last_100_button_, "flat");
  gtk_widget_set_size_request(copy_last_100_button_, 40, 40);
  gtk_widget_set_tooltip_text(copy_last_100_button_, "Copy last 100 lines");
  gtk_accessible_update_property(GTK_ACCESSIBLE(copy_last_100_button_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Copy last 100 lines",
                                 -1);
  g_signal_connect(copy_last_100_button_, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                     auto* self = static_cast<LogsTab*>(user_data);
                     self->OnCopyLast100Clicked();
                   }),
                   this);
  gtk_box_append(GTK_BOX(tb_box), copy_last_100_button_);

  // Copy All Button (40x40 icon-only flat button)
  copy_button_ = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_add_css_class(copy_button_, "flat");
  gtk_widget_set_size_request(copy_button_, 40, 40);
  gtk_widget_set_tooltip_text(copy_button_, "Copy all");
  gtk_accessible_update_property(GTK_ACCESSIBLE(copy_button_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Copy all",
                                 -1);
  g_signal_connect(copy_button_, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                     auto* self = static_cast<LogsTab*>(user_data);
                     self->OnCopyClicked();
                   }),
                   this);
  gtk_box_append(GTK_BOX(tb_box), copy_button_);

  // Open Log Folder Button (40x40 icon-only flat button)
  folder_button_ = gtk_button_new_from_icon_name("folder-open-symbolic");
  gtk_widget_add_css_class(folder_button_, "flat");
  gtk_widget_set_size_request(folder_button_, 40, 40);
  gtk_widget_set_tooltip_text(folder_button_, "Open log folder");
  gtk_accessible_update_property(GTK_ACCESSIBLE(folder_button_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Open log folder",
                                 -1);
  g_signal_connect(folder_button_, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer) {
                     std::string dir = GetLogDirectory();
                     std::string uri = "file://" + dir;
                     g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
                   }),
                   nullptr);
  gtk_box_append(GTK_BOX(tb_box), folder_button_);

  // Clear View Button (40x40 icon-only flat button)
  clear_button_ = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
  gtk_widget_add_css_class(clear_button_, "flat");
  gtk_widget_set_size_request(clear_button_, 40, 40);
  gtk_widget_set_tooltip_text(clear_button_, "Clear view");
  gtk_accessible_update_property(GTK_ACCESSIBLE(clear_button_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Clear view",
                                 -1);
  g_signal_connect(clear_button_, "clicked",
                   G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                     auto* self = static_cast<LogsTab*>(user_data);
                     self->OnClearClicked();
                   }),
                   this);
  gtk_box_append(GTK_BOX(tb_box), clear_button_);

  gtk_box_append(GTK_BOX(main_vbox), tb_box);

  // 2. Seed read warning banner (hidden by default)
  seed_warning_banner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(seed_warning_banner_, "cm-info-banner");
  gtk_widget_add_css_class(seed_warning_banner_, "is-warning");
  GtkWidget* warn_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_box_append(GTK_BOX(seed_warning_banner_), warn_icon);
  GtkWidget* warn_lbl = gtk_label_new("Existing logs could not be read. New messages will still appear here.");
  gtk_label_set_wrap(GTK_LABEL(warn_lbl), TRUE);
  gtk_widget_set_halign(warn_lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(seed_warning_banner_), warn_lbl);
  gtk_widget_set_visible(seed_warning_banner_, FALSE);
  gtk_box_append(GTK_BOX(main_vbox), seed_warning_banner_);

  // 3. Selectable wrapping path-help info label
  path_info_label_ = gtk_label_new(
      "Live view of process logs. Session log: ~/.config/castmirror/castmirror-session.log  •  History: ~/.config/castmirror/castmirror.log  •  JSON sidecar: ~/.config/castmirror/castmirror.ndjson (when verbose_json enabled)");
  gtk_widget_set_halign(path_info_label_, GTK_ALIGN_START);
  gtk_label_set_selectable(GTK_LABEL(path_info_label_), TRUE);
  gtk_label_set_wrap(GTK_LABEL(path_info_label_), TRUE);
  gtk_widget_add_css_class(path_info_label_, "cm-section-description");
  gtk_box_append(GTK_BOX(main_vbox), path_info_label_);

  // 4. Outlined full-height viewport with text view inside GtkOverlay
  scrolled_window_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window_),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_add_css_class(scrolled_window_, "cm-section-card");
  gtk_widget_set_hexpand(scrolled_window_, TRUE);
  gtk_widget_set_vexpand(scrolled_window_, TRUE);

  text_view_ = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view_), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view_), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view_), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_NONE);
  gtk_widget_add_css_class(text_view_, "cm-log-view");

  text_buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_));

  // Severity color tags matching palette tokens
  gtk_text_buffer_create_tag(text_buffer_, "tag_debug", "foreground", "#AEB8BF", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_info", "foreground", "#E3E8EB", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_warn", "foreground", "#F2C66D", nullptr);
  gtk_text_buffer_create_tag(text_buffer_, "tag_error", "foreground", "#FFB4AB", nullptr);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window_), text_view_);

  GtkWidget* overlay = gtk_overlay_new();
  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_vexpand(overlay, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled_window_);

  empty_state_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(empty_state_box_, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty_state_box_, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(empty_state_box_, "cm-empty-state");
  gtk_widget_set_can_target(empty_state_box_, FALSE);

  GtkWidget* empty_title = gtk_label_new("No log messages to show");
  gtk_widget_add_css_class(empty_title, "cm-section-title");
  gtk_box_append(GTK_BOX(empty_state_box_), empty_title);

  GtkWidget* empty_subtitle = gtk_label_new("New messages will appear here.");
  gtk_widget_add_css_class(empty_subtitle, "cm-section-description");
  gtk_box_append(GTK_BOX(empty_state_box_), empty_subtitle);

  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), empty_state_box_);

  gtk_box_append(GTK_BOX(main_vbox), overlay);

  UpdateBufferState();
  UpdateCopyLast100Sensitivity();
}

void LogsTab::OnCopyClicked() {
  if (!text_buffer_) return;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(text_buffer_, &start, &end);
  char* text = gtk_text_buffer_get_text(text_buffer_, &start, &end, FALSE);
  if (text) {
    GdkClipboard* clip = gtk_widget_get_clipboard(copy_button_ ? copy_button_ : root_widget_);
    gdk_clipboard_set_text(clip, text);
    g_free(text);
    if (app_) {
      app_->ShowToast("Logs copied");
    }
  }
}

void LogsTab::OnClearClicked() {
  if (!text_buffer_) return;
  gtk_text_buffer_set_text(text_buffer_, "", 0);
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    history_.clear();
    pending_queue_.clear();
  }
  UpdateBufferState();
  UpdateCopyLast100Sensitivity();
  if (app_) {
    app_->ShowToast("Log view cleared");
  }
}

void LogsTab::OnFilterChanged() {
  if (!filter_entry_) return;
  const char* txt = gtk_editable_get_text(GTK_EDITABLE(filter_entry_));
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    filter_text_ = txt ? std::string(txt) : std::string();
  }
  ApplyFilter();
}

void LogsTab::ApplyFilter() {
  if (!text_buffer_) return;
  std::string filt;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    filt = filter_text_;
  }
  std::string filt_lower = ToLower(filt);

  // Snapshot history under lock
  std::deque<PendingLog> snapshot;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    snapshot = history_;
  }

  gtk_text_buffer_set_text(text_buffer_, "", 0);

  // Check if view was at bottom before refilling? For filter, scroll to end if matches.
  GtkTextIter end_iter;
  gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);

  for (const auto& item : snapshot) {
    if (!filt_lower.empty()) {
      std::string txt_lower = ToLower(item.text);
      if (txt_lower.find(filt_lower) == std::string::npos) {
        continue;
      }
    }
    const char* tag_name = "tag_info";
    if (item.level == LogLevel::kDebug) {
      tag_name = "tag_debug";
    } else if (item.level == LogLevel::kWarn) {
      tag_name = "tag_warn";
    } else if (item.level == LogLevel::kError || item.level == LogLevel::kFatal) {
      tag_name = "tag_error";
    }
    std::string text_with_nl = item.text + "\n";
    gtk_text_buffer_insert_with_tags_by_name(text_buffer_,
                                             &end_iter,
                                             text_with_nl.c_str(),
                                             -1,
                                             tag_name,
                                             nullptr);
  }

  UpdateBufferState();

  // Scroll to end if we inserted something
  gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);
  gtk_text_buffer_place_cursor(text_buffer_, &end_iter);
  if (text_view_) {
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &end_iter, 0.0, FALSE, 0.0, 0.0);
  }
}

bool LogsTab::IsJsonSidecarPresent() const {
  std::string path = GetLogDirectory() + "/castmirror.ndjson";
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

void LogsTab::UpdateCopyLast100Sensitivity() {
  if (!copy_last_100_button_) return;
  bool has_sidecar = IsJsonSidecarPresent();
  bool has_buffer = false;
  if (text_buffer_) {
    has_buffer = gtk_text_buffer_get_char_count(text_buffer_) > 0;
  }
  // Button enabled if sidecar present (primary) or buffer has content
  gtk_widget_set_sensitive(copy_last_100_button_, has_sidecar || has_buffer);
  if (has_sidecar) {
    gtk_widget_set_tooltip_text(copy_last_100_button_, "Copy last 100 lines (JSON sidecar)");
  } else {
    gtk_widget_set_tooltip_text(copy_last_100_button_, "Copy last 100 lines (current view)");
  }
}

void LogsTab::OnCopyLast100Clicked() {
  std::string content;
  std::string json_path = GetLogDirectory() + "/castmirror.ndjson";
  std::error_code ec;
  if (std::filesystem::exists(json_path, ec)) {
    // Keep last 100 lines across .old + current file to honor 2x8MiB rotation cap
    std::deque<std::string> combined;
    std::string old_path = json_path + ".old";
    if (std::filesystem::exists(old_path, ec)) {
      std::ifstream old_file(old_path);
      std::string line;
      while (std::getline(old_file, line)) {
        combined.push_back(line);
        if (combined.size() > 100) combined.pop_front();
      }
    }
    {
      std::ifstream file(json_path);
      if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
          combined.push_back(line);
          if (combined.size() > 100) combined.pop_front();
        }
      }
    }
    for (auto &l : combined) content += l + "\n";
  }

  if (content.empty()) {
    // Fallback: last 100 lines from current text_buffer view (filtered or not)
    if (text_buffer_) {
      GtkTextIter start, end;
      gtk_text_buffer_get_bounds(text_buffer_, &start, &end);
      char* all = gtk_text_buffer_get_text(text_buffer_, &start, &end, FALSE);
      if (all) {
        std::string all_str(all);
        g_free(all);
        // Remove trailing newline split handling
        std::vector<std::string> lines;
        std::istringstream iss(all_str);
        std::string line;
        while (std::getline(iss, line)) {
          lines.push_back(line);
        }
        size_t start_idx = lines.size() > 100 ? lines.size() - 100 : 0;
        for (size_t i = start_idx; i < lines.size(); ++i) {
          content += lines[i] + "\n";
        }
      }
    }
  }

  if (!content.empty()) {
    GtkWidget* src = copy_last_100_button_ ? copy_last_100_button_ : (copy_button_ ? copy_button_ : root_widget_);
    GdkClipboard* clip = gtk_widget_get_clipboard(src);
    gdk_clipboard_set_text(clip, content.c_str());
    if (app_) {
      app_->ShowToast("Copied last 100 lines");
    }
  } else {
    if (app_) {
      app_->ShowToast("No log lines to copy");
    }
  }
}

void LogsTab::UpdateBufferState() {
  if (!text_buffer_) return;
  gint char_count = gtk_text_buffer_get_char_count(text_buffer_);
  bool is_empty = (char_count == 0);

  if (copy_button_) {
    gtk_widget_set_sensitive(copy_button_, !is_empty);
  }
  if (clear_button_) {
    gtk_widget_set_sensitive(clear_button_, !is_empty);
  }
  if (empty_state_box_) {
    gtk_widget_set_visible(empty_state_box_, is_empty);
  }
  UpdateCopyLast100Sensitivity();
}

void LogsTab::SeedInitialLogs() {
  std::string dir = GetLogDirectory();
  std::string session_file = dir + "/castmirror-session.log";
  std::string main_file = dir + "/castmirror.log";
  std::string target_file = session_file;

  std::error_code ec;
  if (!std::filesystem::exists(session_file, ec)) {
    target_file = main_file;
  }

  if (std::filesystem::exists(target_file, ec)) {
    std::ifstream file(target_file, std::ios::binary);
    if (!file.is_open()) {
      if (seed_warning_banner_) {
        gtk_widget_set_visible(seed_warning_banner_, TRUE);
      }
      UpdateBufferState();
      return;
    }

    file.seekg(0, std::ios::end);
    std::streampos end_pos = file.tellg();
    if (end_pos < 0) {
      if (seed_warning_banner_) {
        gtk_widget_set_visible(seed_warning_banner_, TRUE);
      }
      UpdateBufferState();
      return;
    }

    size_t file_size = static_cast<size_t>(end_pos);
    size_t read_bytes = std::min(file_size, static_cast<size_t>(500 * 1024));
    file.seekg(file_size - read_bytes);

    std::string buffer(read_bytes, '\0');
    if (read_bytes > 0 && !file.read(&buffer[0], read_bytes)) {
      if (seed_warning_banner_) {
        gtk_widget_set_visible(seed_warning_banner_, TRUE);
      }
      UpdateBufferState();
      return;
    }

    // Truncate partial first line if sought into middle
    if (read_bytes < file_size) {
      size_t first_nl = buffer.find('\n');
      if (first_nl != std::string::npos) {
        buffer = buffer.substr(first_nl + 1);
      }
    }

    if (!buffer.empty()) {
      gtk_text_buffer_set_text(text_buffer_, buffer.c_str(), -1);

      // Populate history_ from seeded buffer for filtering
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::istringstream iss(buffer);
        std::string line;
        while (std::getline(iss, line)) {
          if (line.empty()) continue;
          // Determine level heuristically from tags if possible, default Info
          LogLevel lvl = LogLevel::kInfo;
          if (line.find("[DEBUG") != std::string::npos) lvl = LogLevel::kDebug;
          else if (line.find("[WARN") != std::string::npos) lvl = LogLevel::kWarn;
          else if (line.find("[ERROR") != std::string::npos) lvl = LogLevel::kError;
          history_.push_back({lvl, line});
          if (history_.size() > kMaxHistory) history_.pop_front();
        }
      }

      GtkTextIter end_iter;
      gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);
      gtk_text_buffer_place_cursor(text_buffer_, &end_iter);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &end_iter, 0.0, FALSE, 0.0, 0.0);
    }
  }

  UpdateBufferState();
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
    // Also push to history for filtering (preserve all, capped)
    for (auto &item : to_drain) {
      history_.push_back(item);
      if (history_.size() > kMaxHistory) history_.pop_front();
    }
  }

  if (to_drain.empty()) return;

  // Respect filter
  std::string filt;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    filt = filter_text_;
  }
  std::string filt_lower = ToLower(filt);

  // Check if view is currently at bottom before appending
  bool at_bottom = true;
  if (scrolled_window_) {
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window_));
    if (vadj) {
      double value = gtk_adjustment_get_value(vadj);
      double upper = gtk_adjustment_get_upper(vadj);
      double page_size = gtk_adjustment_get_page_size(vadj);
      at_bottom = (value + page_size >= upper - 10.0) || (upper <= page_size);
    }
  }

  GtkTextIter end_iter;
  gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);

  for (const auto& item : to_drain) {
    if (!filt_lower.empty()) {
      std::string txt_lower = ToLower(item.text);
      if (txt_lower.find(filt_lower) == std::string::npos) {
        continue;
      }
    }
    const char* tag_name = "tag_info";
    if (item.level == LogLevel::kDebug) {
      tag_name = "tag_debug";
    } else if (item.level == LogLevel::kWarn) {
      tag_name = "tag_warn";
    } else if (item.level == LogLevel::kError || item.level == LogLevel::kFatal) {
      tag_name = "tag_error";
    }

    std::string text_with_nl = item.text + "\n";
    gtk_text_buffer_insert_with_tags_by_name(text_buffer_,
                                             &end_iter,
                                             text_with_nl.c_str(),
                                             -1,
                                             tag_name,
                                             nullptr);
  }

  UpdateBufferState();

  if (at_bottom) {
    gtk_text_buffer_get_end_iter(text_buffer_, &end_iter);
    gtk_text_buffer_place_cursor(text_buffer_, &end_iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &end_iter, 0.0, FALSE, 0.0, 0.0);
  }
}

}  // namespace castcore::gui
