#include "first_run.h"
#include "gui_app.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include "castcore/config.h"
#include "help_copy.h"

namespace castcore::gui {

namespace {

struct FirstRunState {
  AdwDialog* dialog = nullptr;
  AdwCarousel* carousel = nullptr;
  GtkWidget* back_button = nullptr;
  GtkWidget* continue_button = nullptr;
  GtkWidget* done_button = nullptr;
  GtkWidget* step_label = nullptr;
  GuiApp* app = nullptr;
  bool completed = false;
  std::vector<GtkWidget*> pages;
};

void CompleteFirstRunOnce(FirstRunState* state) {
  if (!state || state->completed) {
    return;
  }
  state->completed = true;

  auto& cfg = ConfigStore::Instance().Mutable();
  if (!cfg.first_run_complete) {
    cfg.first_run_complete = true;
    ConfigStore::Instance().Save();
  }

  if (state->app) {
    state->app->PopModalActionBlock();
    state->app = nullptr;
  }

  if (state->dialog) {
    adw_dialog_force_close(state->dialog);
  }
}

void UpdateFirstRunNavigation(FirstRunState* state, guint index) {
  if (!state) {
    return;
  }

  char step_text[32];
  std::snprintf(step_text, sizeof(step_text), "Step %u of 3", index + 1);
  gtk_label_set_text(GTK_LABEL(state->step_label), step_text);
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->step_label),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, step_text,
                                 -1);

  gtk_widget_set_visible(state->back_button, index > 0);
  gtk_widget_set_visible(state->continue_button, index < 2);
  gtk_widget_set_visible(state->done_button, index >= 2);

  GtkWidget* default_btn = (index >= 2) ? state->done_button : state->continue_button;
  adw_dialog_set_default_widget(state->dialog, default_btn);
}

GtkWidget* MakeCarouselPage(GtkWidget* icon_or_logo, const char* title, const char* body) {
  GtkWidget* scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_widget_set_hexpand(scrolled, TRUE);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start(box, 32);
  gtk_widget_set_margin_end(box, 32);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(box, GTK_ALIGN_START);

  if (icon_or_logo) {
    gtk_widget_set_halign(icon_or_logo, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(icon_or_logo, 8);
    gtk_box_append(GTK_BOX(box), icon_or_logo);
  }

  GtkWidget* title_lbl = gtk_label_new(title);
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(title_lbl), TRUE);
  gtk_label_set_justify(GTK_LABEL(title_lbl), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(title_lbl, "title-2");
  gtk_box_append(GTK_BOX(box), title_lbl);

  GtkWidget* body_lbl = gtk_label_new(body);
  gtk_widget_set_halign(body_lbl, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(body_lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(body_lbl), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(body_lbl), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(body_lbl), 50);
  gtk_widget_add_css_class(body_lbl, "body");
  gtk_widget_set_margin_top(body_lbl, 8);
  gtk_box_append(GTK_BOX(box), body_lbl);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
  return scrolled;
}

}  // namespace

void ShowFirstRunDialogIfNeeded(AdwApplicationWindow* parent) {
  if (ConfigStore::Instance().Get().first_run_complete) {
    return;
  }

  auto* state = new FirstRunState();
  state->app = static_cast<GuiApp*>(g_object_get_data(G_OBJECT(parent), "castmirror-gui-app"));
  if (state->app) {
    state->app->PushModalActionBlock();
  }

  AdwDialog* dialog = adw_dialog_new();
  state->dialog = dialog;
  adw_dialog_set_content_width(dialog, 600);
  adw_dialog_set_content_height(dialog, 480);
  adw_dialog_set_can_close(dialog, FALSE);
  adw_dialog_set_title(dialog, copy::kFirstRunWelcomeTitle);

  g_object_set_data_full(G_OBJECT(dialog), "first_run_state", state,
                         [](gpointer data) { delete static_cast<FirstRunState*>(data); });

  GtkWidget* toolbar_view = adw_toolbar_view_new();
  adw_dialog_set_child(dialog, toolbar_view);

  // Top header bar (close button retained)
  GtkWidget* header_bar = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

  // Center carousel
  GtkWidget* carousel = adw_carousel_new();
  state->carousel = ADW_CAROUSEL(carousel);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), carousel);

  // Page 1: Welcome
  GtkWidget* logo_img = gtk_image_new_from_resource("/io/github/vindeckyy/CastMirror/logo.svg");
  gtk_image_set_pixel_size(GTK_IMAGE(logo_img), 64);
  GtkWidget* page1 = MakeCarouselPage(logo_img, copy::kFirstRunWelcomeTitle, copy::kFirstRunWelcomeBody);
  adw_carousel_append(ADW_CAROUSEL(carousel), page1);
  state->pages.push_back(page1);

  // Page 2: Network connection
  GtkWidget* net_icon = gtk_image_new_from_icon_name("network-wireless-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(net_icon), 64);
  GtkWidget* page2 = MakeCarouselPage(net_icon, copy::kFirstRunNetworkTitle, copy::kFirstRunNetworkBody);
  adw_carousel_append(ADW_CAROUSEL(carousel), page2);
  state->pages.push_back(page2);

  // Page 3: Select screen
  GtkWidget* screen_icon = gtk_image_new_from_icon_name("video-display-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(screen_icon), 64);
  GtkWidget* page3 = MakeCarouselPage(screen_icon, copy::kFirstRunCaptureTitle, copy::kFirstRunCaptureBody);
  adw_carousel_append(ADW_CAROUSEL(carousel), page3);
  state->pages.push_back(page3);

  // Bottom navigation bar
  GtkWidget* bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bottom_bar, "toolbar");
  gtk_widget_set_margin_start(bottom_bar, 16);
  gtk_widget_set_margin_end(bottom_bar, 16);
  gtk_widget_set_margin_top(bottom_bar, 12);
  gtk_widget_set_margin_bottom(bottom_bar, 12);

  // Back button (flat, hidden on page 1)
  GtkWidget* back_button = gtk_button_new_with_label("Back");
  gtk_widget_add_css_class(back_button, "flat");
  gtk_widget_set_visible(back_button, FALSE);
  state->back_button = back_button;
  gtk_box_append(GTK_BOX(bottom_bar), back_button);

  // Center indicator cluster: Step label + Dots
  GtkWidget* center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(center_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(center_box, TRUE);

  GtkWidget* step_label = gtk_label_new("Step 1 of 3");
  gtk_widget_add_css_class(step_label, "dim-label");
  state->step_label = step_label;
  gtk_box_append(GTK_BOX(center_box), step_label);

  GtkWidget* dots = adw_carousel_indicator_dots_new();
  adw_carousel_indicator_dots_set_carousel(ADW_CAROUSEL_INDICATOR_DOTS(dots), ADW_CAROUSEL(carousel));
  gtk_box_append(GTK_BOX(center_box), dots);

  gtk_box_append(GTK_BOX(bottom_bar), center_box);

  // Action buttons
  GtkWidget* continue_button = gtk_button_new_with_label("Continue");
  gtk_widget_add_css_class(continue_button, "suggested-action");
  state->continue_button = continue_button;
  gtk_box_append(GTK_BOX(bottom_bar), continue_button);

  GtkWidget* done_button = gtk_button_new_with_label("Done");
  gtk_widget_add_css_class(done_button, "suggested-action");
  gtk_widget_set_visible(done_button, FALSE);
  state->done_button = done_button;
  gtk_box_append(GTK_BOX(bottom_bar), done_button);

  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), bottom_bar);

  // Initial navigation state
  UpdateFirstRunNavigation(state, 0);

  // Signals
  auto on_back_clicked = +[](GtkButton*, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    double pos = adw_carousel_get_position(s->carousel);
    int cur = static_cast<int>(std::round(pos));
    if (cur > 0 && cur - 1 < static_cast<int>(s->pages.size())) {
      adw_carousel_scroll_to(s->carousel, s->pages[cur - 1], TRUE);
    }
  };
  g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), state);

  auto on_continue_clicked = +[](GtkButton*, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    double pos = adw_carousel_get_position(s->carousel);
    int cur = static_cast<int>(std::round(pos));
    if (cur + 1 < static_cast<int>(s->pages.size())) {
      adw_carousel_scroll_to(s->carousel, s->pages[cur + 1], TRUE);
    }
  };
  g_signal_connect(continue_button, "clicked", G_CALLBACK(on_continue_clicked), state);

  auto on_done_clicked = +[](GtkButton*, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    CompleteFirstRunOnce(s);
  };
  g_signal_connect(done_button, "clicked", G_CALLBACK(on_done_clicked), state);

  auto on_page_changed = +[](AdwCarousel*, guint index, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    UpdateFirstRunNavigation(s, index);
  };
  g_signal_connect(carousel, "page-changed", G_CALLBACK(on_page_changed), state);

  auto on_close_attempt = +[](AdwDialog*, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    CompleteFirstRunOnce(s);
  };
  g_signal_connect(dialog, "close-attempt", G_CALLBACK(on_close_attempt), state);

  auto on_closed = +[](AdwDialog*, gpointer user_data) {
    auto* s = static_cast<FirstRunState*>(user_data);
    CompleteFirstRunOnce(s);
  };
  g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), state);

  adw_dialog_present(dialog, GTK_WIDGET(parent));
}

}  // namespace castcore::gui
