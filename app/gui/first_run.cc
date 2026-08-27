#include "first_run.h"
#include "help_copy.h"
#include "castcore/config.h"

namespace castcore::gui {

namespace {

GtkWidget* MakeAssistantPage(const char* title, const char* body) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(box), 20);

  GtkWidget* title_lbl = gtk_label_new(title);
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "title-label");
  gtk_box_pack_start(GTK_BOX(box), title_lbl, FALSE, FALSE, 0);

  GtkWidget* body_lbl = gtk_label_new(body);
  gtk_widget_set_halign(body_lbl, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(body_lbl), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 50);
  gtk_style_context_add_class(gtk_widget_get_style_context(body_lbl), "setting-help");
  gtk_box_pack_start(GTK_BOX(box), body_lbl, TRUE, TRUE, 0);

  return box;
}

}  // namespace

void ShowFirstRunAssistantIfNeeded(GtkWindow* parent) {
  if (ConfigStore::Instance().Get().first_run_complete) {
    return;
  }

  GtkWidget* assistant = gtk_assistant_new();
  gtk_window_set_title(GTK_WINDOW(assistant), "Welcome to CastMirror");
  gtk_window_set_transient_for(GTK_WINDOW(assistant), parent);
  gtk_window_set_modal(GTK_WINDOW(assistant), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(assistant), 520, 360);

  // Page 1: Welcome
  GtkWidget* page1 = MakeAssistantPage(copy::kFirstRunWelcomeTitle, copy::kFirstRunWelcomeBody);
  gtk_assistant_append_page(GTK_ASSISTANT(assistant), page1);
  gtk_assistant_set_page_title(GTK_ASSISTANT(assistant), page1, "Welcome");
  gtk_assistant_set_page_type(GTK_ASSISTANT(assistant), page1, GTK_ASSISTANT_PAGE_INTRO);
  gtk_assistant_set_page_complete(GTK_ASSISTANT(assistant), page1, TRUE);

  // Page 2: Network
  GtkWidget* page2 = MakeAssistantPage(copy::kFirstRunNetworkTitle, copy::kFirstRunNetworkBody);
  gtk_assistant_append_page(GTK_ASSISTANT(assistant), page2);
  gtk_assistant_set_page_title(GTK_ASSISTANT(assistant), page2, "Network");
  gtk_assistant_set_page_type(GTK_ASSISTANT(assistant), page2, GTK_ASSISTANT_PAGE_CONTENT);
  gtk_assistant_set_page_complete(GTK_ASSISTANT(assistant), page2, TRUE);

  // Page 3: Screen Selection
  GtkWidget* page3 = MakeAssistantPage(copy::kFirstRunCaptureTitle, copy::kFirstRunCaptureBody);
  gtk_assistant_append_page(GTK_ASSISTANT(assistant), page3);
  gtk_assistant_set_page_title(GTK_ASSISTANT(assistant), page3, "Screen Selection");
  gtk_assistant_set_page_type(GTK_ASSISTANT(assistant), page3, GTK_ASSISTANT_PAGE_CONFIRM);
  gtk_assistant_set_page_complete(GTK_ASSISTANT(assistant), page3, TRUE);

  auto on_finish = +[](GtkAssistant* ast, gpointer) {
    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.first_run_complete = true;
    ConfigStore::Instance().Save();
    gtk_widget_destroy(GTK_WIDGET(ast));
  };

  g_signal_connect(assistant, "cancel", G_CALLBACK(on_finish), nullptr);
  g_signal_connect(assistant, "close", G_CALLBACK(on_finish), nullptr);
  g_signal_connect(assistant, "apply", G_CALLBACK(on_finish), nullptr);
  gtk_widget_show_all(assistant);
}

}  // namespace castcore::gui
