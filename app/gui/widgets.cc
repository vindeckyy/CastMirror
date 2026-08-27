#include "widgets.h"

namespace castcore::gui {

GtkWidget* MakeSectionHeader(const char* title, const char* one_liner) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget* title_lbl = gtk_label_new(title);
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "card-title");
  gtk_box_pack_start(GTK_BOX(box), title_lbl, FALSE, FALSE, 0);

  if (one_liner && one_liner[0] != '\0') {
    GtkWidget* desc_lbl = gtk_label_new(one_liner);
    gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(desc_lbl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(desc_lbl), "card-desc");
    gtk_box_pack_start(GTK_BOX(box), desc_lbl, FALSE, FALSE, 0);
  }

  return box;
}

GtkWidget* MakeInfoButton(const char* help_text) {
  GtkWidget* btn = gtk_button_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_style_context_add_class(gtk_widget_get_style_context(btn), "btn-info");
  gtk_widget_set_tooltip_text(btn, "Click for detailed explanation");

  if (help_text && help_text[0] != '\0') {
    GtkWidget* popover = gtk_popover_new(btn);
    GtkWidget* pop_label = gtk_label_new(help_text);
    gtk_label_set_line_wrap(GTK_LABEL(pop_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(pop_label), 38);
    gtk_container_set_border_width(GTK_CONTAINER(popover), 12);
    gtk_container_add(GTK_CONTAINER(popover), pop_label);
    gtk_widget_show(pop_label);

    auto on_info_click = +[](GtkButton*, gpointer pop) {
      gtk_popover_popup(GTK_POPOVER(pop));
    };
    g_signal_connect(btn, "clicked", G_CALLBACK(on_info_click), popover);
  }

  return btn;
}

GtkWidget* MakeSettingRow(const char* title,
                          const char* one_liner,
                          const char* popover_text,
                          GtkWidget* control_widget) {
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(row), "setting-row");

  GtkWidget* left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(left_vbox, TRUE);

  GtkWidget* title_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* title_lbl = gtk_label_new(title);
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "setting-title");
  gtk_box_pack_start(GTK_BOX(title_hbox), title_lbl, FALSE, FALSE, 0);

  if (popover_text && popover_text[0] != '\0') {
    GtkWidget* info_btn = MakeInfoButton(popover_text);
    gtk_box_pack_start(GTK_BOX(title_hbox), info_btn, FALSE, FALSE, 0);
  }

  gtk_box_pack_start(GTK_BOX(left_vbox), title_hbox, FALSE, FALSE, 0);

  if (one_liner && one_liner[0] != '\0') {
    GtkWidget* desc_lbl = gtk_label_new(one_liner);
    gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(desc_lbl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(desc_lbl), "setting-help");
    gtk_box_pack_start(GTK_BOX(left_vbox), desc_lbl, FALSE, FALSE, 0);
  }

  gtk_box_pack_start(GTK_BOX(row), left_vbox, TRUE, TRUE, 0);

  if (control_widget) {
    gtk_widget_set_valign(control_widget, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(row), control_widget, FALSE, FALSE, 0);
  }

  return row;
}

GtkWidget* MakeStatCard(const char* title,
                        const char* popover_text,
                        GtkWidget** out_value_label) {
  GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_style_context_add_class(gtk_widget_get_style_context(card), "stat-card");

  GtkWidget* top_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget* tlbl = gtk_label_new(title);
  gtk_widget_set_halign(tlbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(tlbl), "stat-title");
  gtk_box_pack_start(GTK_BOX(top_hbox), tlbl, TRUE, TRUE, 0);

  if (popover_text && popover_text[0] != '\0') {
    GtkWidget* info_btn = MakeInfoButton(popover_text);
    gtk_box_pack_end(GTK_BOX(top_hbox), info_btn, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(card), top_hbox, FALSE, FALSE, 0);

  GtkWidget* val_lbl = gtk_label_new("—");
  gtk_widget_set_halign(val_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(val_lbl), "stat-value");
  gtk_box_pack_start(GTK_BOX(card), val_lbl, FALSE, FALSE, 0);

  if (out_value_label) {
    *out_value_label = val_lbl;
  }

  return card;
}

void UpdateStatusBadge(GtkWidget* badge, GtkWidget* label, SessionState state) {
  if (!badge || !label) return;
  GtkStyleContext* ctx = gtk_widget_get_style_context(badge);
  gtk_style_context_remove_class(ctx, "status-idle");
  gtk_style_context_remove_class(ctx, "status-connecting");
  gtk_style_context_remove_class(ctx, "status-live");
  gtk_style_context_remove_class(ctx, "status-failed");

  switch (state) {
    case SessionState::kStreaming:
      gtk_style_context_add_class(ctx, "status-live");
      gtk_label_set_text(GTK_LABEL(label), "● LIVE");
      break;
    case SessionState::kConnecting:
    case SessionState::kNegotiating:
      gtk_style_context_add_class(ctx, "status-connecting");
      gtk_label_set_text(GTK_LABEL(label), "◐ CONNECTING");
      break;
    case SessionState::kReconnecting:
      gtk_style_context_add_class(ctx, "status-connecting");
      gtk_label_set_text(GTK_LABEL(label), "◐ RECONNECTING");
      break;
    case SessionState::kStopping:
      gtk_style_context_add_class(ctx, "status-connecting");
      gtk_label_set_text(GTK_LABEL(label), "○ STOPPING");
      break;
    case SessionState::kFailed:
      gtk_style_context_add_class(ctx, "status-failed");
      gtk_label_set_text(GTK_LABEL(label), "✕ FAILED");
      break;
    case SessionState::kIdle:
    case SessionState::kReady:
    case SessionState::kDiscovering:
    default:
      gtk_style_context_add_class(ctx, "status-idle");
      gtk_label_set_text(GTK_LABEL(label), "○ READY");
      break;
  }
}

}  // namespace castcore::gui
