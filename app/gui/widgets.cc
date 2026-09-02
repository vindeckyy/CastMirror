#include "widgets.h"

#include <string>

namespace castcore::gui {

namespace {

const char* kSemanticClasses[] = {
    "is-idle",
    "is-progress",
    "is-live",
    "is-warning",
    "is-error",
    "is-selected",
    "is-disabled",
};

void ClearSemanticClasses(GtkWidget* widget) {
  if (!widget) {
    return;
  }
  for (const char* cls : kSemanticClasses) {
    gtk_widget_remove_css_class(widget, cls);
  }
}

void MarkPresentation(GtkWidget* widget) {
  if (!widget) {
    return;
  }
  g_object_set(widget, "accessible-role", GTK_ACCESSIBLE_ROLE_PRESENTATION, nullptr);
}

void OnStatValueNotify(GObject* object, GParamSpec* /*pspec*/, gpointer user_data) {
  auto* card = GTK_WIDGET(user_data);
  const char* title = static_cast<const char*>(g_object_get_data(object, "cm-stat-title"));
  const char* value = gtk_label_get_text(GTK_LABEL(object));
  std::string accessible = std::string(title ? title : "Statistic") + ": " + (value ? value : "—");
  gtk_accessible_update_property(GTK_ACCESSIBLE(card),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, accessible.c_str(),
                                 -1);
}

}  // namespace

GtkWidget* MakeSectionHeader(const char* title, const char* one_liner) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  GtkWidget* title_lbl = gtk_label_new(title ? title : "");
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(title_lbl), TRUE);
  gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
  gtk_widget_add_css_class(title_lbl, "cm-section-title");
  gtk_box_append(GTK_BOX(box), title_lbl);

  if (one_liner && one_liner[0] != '\0') {
    GtkWidget* desc_lbl = gtk_label_new(one_liner);
    gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(desc_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.0f);
    gtk_widget_add_css_class(desc_lbl, "cm-section-description");
    gtk_box_append(GTK_BOX(box), desc_lbl);
  }

  return box;
}

GtkWidget* MakeInfoButton(const char* title, const char* help_text) {
  GtkWidget* btn = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn), "help-about-symbolic");
  gtk_widget_add_css_class(btn, "flat");
  gtk_widget_add_css_class(btn, "circular");
  gtk_widget_set_size_request(btn, 40, 40);

  std::string accessible = std::string("About ") + (title ? title : "this setting");
  gtk_widget_set_tooltip_text(btn, accessible.c_str());
  gtk_accessible_update_property(GTK_ACCESSIBLE(btn),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, accessible.c_str(),
                                 -1);

  if (help_text && help_text[0] != '\0') {
    GtkWidget* popover = gtk_popover_new();
    GtkWidget* label = gtk_label_new(help_text);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 42);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 12);
    gtk_popover_set_child(GTK_POPOVER(popover), label);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), popover);
  }

  return btn;
}

GtkWidget* MakeStatCard(const char* title,
                        const char* icon_name,
                        const char* help_text,
                        GtkWidget** out_value_label) {
  GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(card, "cm-stat-card");
  gtk_widget_set_hexpand(card, TRUE);
  gtk_widget_set_size_request(card, 220, -1);

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  if (icon_name && icon_name[0] != '\0') {
    GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    MarkPresentation(icon);
    gtk_box_append(GTK_BOX(header), icon);
  }

  GtkWidget* title_lbl = gtk_label_new(title ? title : "");
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_widget_set_hexpand(title_lbl, TRUE);
  gtk_label_set_wrap(GTK_LABEL(title_lbl), TRUE);
  gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
  gtk_widget_add_css_class(title_lbl, "cm-section-description");
  gtk_box_append(GTK_BOX(header), title_lbl);

  if (help_text && help_text[0] != '\0') {
    gtk_box_append(GTK_BOX(header), MakeInfoButton(title, help_text));
  }

  gtk_box_append(GTK_BOX(card), header);

  GtkWidget* value_lbl = gtk_label_new("—");
  gtk_widget_set_halign(value_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(value_lbl), TRUE);
  gtk_label_set_xalign(GTK_LABEL(value_lbl), 0.0f);
  gtk_box_append(GTK_BOX(card), value_lbl);

  g_object_set_data_full(G_OBJECT(value_lbl), "cm-stat-title",
                         g_strdup(title ? title : "Statistic"), g_free);
  g_signal_connect(value_lbl, "notify::label", G_CALLBACK(OnStatValueNotify), card);
  OnStatValueNotify(G_OBJECT(value_lbl), nullptr, card);

  if (out_value_label) {
    *out_value_label = value_lbl;
  }
  return card;
}

GtkWidget* MakeStatCardWithSparkline(const char* title,
                                     const char* icon_name,
                                     const char* help_text,
                                     GtkWidget** out_value_label,
                                     GtkWidget* sparkline_widget) {
  GtkWidget* card = MakeStatCard(title, icon_name, help_text, out_value_label);
  if (sparkline_widget) {
    gtk_box_append(GTK_BOX(card), sparkline_widget);
  }
  return card;
}

void UpdateStatusBadge(GtkWidget* pill, GtkWidget* dot, GtkWidget* label, SessionState state) {
  if (!pill || !label) {
    return;
  }

  ClearSemanticClasses(pill);
  if (dot) {
    ClearSemanticClasses(dot);
  }

  const char* text = "Ready";
  const char* cls = "is-idle";
  switch (state) {
    case SessionState::kStreaming:
      text = "Live";
      cls = "is-live";
      break;
    case SessionState::kConnecting:
      text = "Connecting";
      cls = "is-warning";
      break;
    case SessionState::kNegotiating:
      text = "Starting stream";
      cls = "is-warning";
      break;
    case SessionState::kReconnecting:
      text = "Reconnecting";
      cls = "is-warning";
      break;
    case SessionState::kStopping:
      text = "Stopping";
      cls = "is-warning";
      break;
    case SessionState::kFailed:
      text = "Failed";
      cls = "is-error";
      break;
    case SessionState::kIdle:
    case SessionState::kReady:
    case SessionState::kDiscovering:
    default:
      text = "Ready";
      cls = "is-idle";
      break;
  }

  gtk_widget_add_css_class(pill, cls);
  if (dot) {
    gtk_widget_add_css_class(dot, cls);
  }
  gtk_label_set_text(GTK_LABEL(label), text);
}

}  // namespace castcore::gui
