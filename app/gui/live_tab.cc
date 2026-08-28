#include "live_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "help_copy.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <ctime>

namespace castcore::gui {

static std::atomic<bool> g_debug_hold_warning{false};
static LiveTab* g_debug_live_tab = nullptr;
namespace {

std::string GetCurrentTimestampStr() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &in_time_t);
#else
  localtime_r(&in_time_t, &tm_buf);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm_buf, "%H:%M:%S");
  return ss.str();
}

GtkWidget* MakePipelineNode(const char* title,
                            const char* icon_name,
                            const char* default_sub,
                            GtkWidget** out_sub) {
  GtkWidget* node = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(node, "cm-pipeline-node");
  gtk_widget_add_css_class(node, "is-idle");
  gtk_widget_set_hexpand(node, TRUE);

  GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
  gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(node), icon);

  GtkWidget* tlbl = gtk_label_new(title);
  gtk_widget_set_halign(tlbl, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(tlbl, "cm-section-title");
  gtk_box_append(GTK_BOX(node), tlbl);

  GtkWidget* slbl = gtk_label_new(default_sub);
  gtk_widget_set_halign(slbl, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(slbl), TRUE);
  gtk_widget_add_css_class(slbl, "cm-section-description");
  gtk_box_append(GTK_BOX(node), slbl);

  if (out_sub) {
    *out_sub = slbl;
  }
  return node;
}

}  // namespace

LiveTab::LiveTab(GuiApp* app) : app_(app) {
  g_debug_live_tab = this;
  BuildUi();
}

LiveTab::~LiveTab() = default;

void LiveTab::OnGoToCastClicked() {
  if (app_) {
    app_->SwitchToPage("cast");
  }
}

void LiveTab::OnBackToCastClicked() {
  failure_visible_ = false;
  if (app_) {
    app_->SwitchToPage("cast");
  }
}

void LiveTab::BuildUi() {
  root_widget_ = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(root_widget_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(GTK_STACK(root_widget_), 180);

  // 1. Empty Page (AdwStatusPage)
  empty_page_ = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty_page_), "castmirror-signal-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty_page_), "No active cast");
  adw_status_page_set_description(
      ADW_STATUS_PAGE(empty_page_),
      "Start from the Cast page. Screen capture stays off until a session begins.");
  GtkWidget* go_cast_btn = gtk_button_new_with_label("Go to Cast");
  gtk_widget_add_css_class(go_cast_btn, "pill");
  gtk_widget_add_css_class(go_cast_btn, "suggested-action");
  gtk_widget_set_halign(go_cast_btn, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(go_cast_btn), "win.page");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(go_cast_btn),
                                         g_variant_new_string("cast"));
  adw_status_page_set_child(ADW_STATUS_PAGE(empty_page_), go_cast_btn);
  gtk_stack_add_named(GTK_STACK(root_widget_), empty_page_, "empty");

  // 2. Active Session Page (GtkScrolledWindow -> AdwClamp -> GtkBox)
  session_scroller_ = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(session_scroller_),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);

  GtkWidget* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 960);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_add_css_class(content_box, "cm-page-content");
  adw_clamp_set_child(ADW_CLAMP(clamp), content_box);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(session_scroller_), clamp);

  // 2A. Hero Card
  hero_card_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(hero_card_, "cm-section-card");
  gtk_widget_add_css_class(hero_card_, "cm-hero-row");
  gtk_widget_set_hexpand(hero_card_, TRUE);

  hero_icon_ = gtk_image_new_from_icon_name("castmirror-signal-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(hero_icon_), 56);
  gtk_widget_add_css_class(hero_icon_, "cm-hero-signal");
  gtk_box_append(GTK_BOX(hero_card_), hero_icon_);

  GtkWidget* hero_text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(hero_text_vbox, TRUE);
  gtk_widget_set_valign(hero_text_vbox, GTK_ALIGN_CENTER);

  hero_title_lbl_ = gtk_label_new("Starting cast…");
  gtk_widget_set_halign(hero_title_lbl_, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(hero_title_lbl_), TRUE);
  gtk_widget_add_css_class(hero_title_lbl_, "cm-section-title");

  hero_subtitle_lbl_ = gtk_label_new("Connecting to display…");
  gtk_widget_set_halign(hero_subtitle_lbl_, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(hero_subtitle_lbl_), TRUE);
  gtk_widget_add_css_class(hero_subtitle_lbl_, "cm-section-description");

  gtk_box_append(GTK_BOX(hero_text_vbox), hero_title_lbl_);
  gtk_box_append(GTK_BOX(hero_text_vbox), hero_subtitle_lbl_);
  gtk_box_append(GTK_BOX(hero_card_), hero_text_vbox);

  hero_status_pill_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(hero_status_pill_, "cm-status-pill");
  gtk_widget_set_valign(hero_status_pill_, GTK_ALIGN_CENTER);

  hero_status_dot_ = gtk_image_new();
  gtk_widget_add_css_class(hero_status_dot_, "cm-status-dot");

  hero_status_lbl_ = gtk_label_new("Connecting");
  gtk_box_append(GTK_BOX(hero_status_pill_), hero_status_dot_);
  gtk_box_append(GTK_BOX(hero_status_pill_), hero_status_lbl_);
  gtk_box_append(GTK_BOX(hero_card_), hero_status_pill_);

  gtk_box_append(GTK_BOX(content_box), hero_card_);

  // 2B. Session Pipeline Card
  pipe_card_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class(pipe_card_, "cm-section-card");

  GtkWidget* pipe_header =
      MakeSectionHeader("Session pipeline", "Live video flow from display capture to TV decoder");
  gtk_box_append(GTK_BOX(pipe_card_), pipe_header);

  GtkWidget* pipe_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pipe_scroller),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);

  GtkWidget* pipe_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(pipe_hbox, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pipe_scroller), pipe_hbox);

  auto make_chevron = []() -> GtkWidget* {
    GtkWidget* img = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(img, "dim-label");
    gtk_accessible_update_property(GTK_ACCESSIBLE(img), GTK_ACCESSIBLE_PROPERTY_LABEL, "separator", -1);
    return img;
  };

  pipe_screen_node_ = MakePipelineNode("Screen", "video-single-display-symbolic", "—", &pipe_screen_sub_);
  gtk_box_append(GTK_BOX(pipe_hbox), pipe_screen_node_);
  gtk_box_append(GTK_BOX(pipe_hbox), make_chevron());

  pipe_capture_node_ = MakePipelineNode("Capture", "camera-video-symbolic", "—", &pipe_capture_sub_);
  gtk_box_append(GTK_BOX(pipe_hbox), pipe_capture_node_);
  gtk_box_append(GTK_BOX(pipe_hbox), make_chevron());

  pipe_encode_node_ = MakePipelineNode("Encode", "applications-engineering-symbolic", "—", &pipe_encode_sub_);
  gtk_box_append(GTK_BOX(pipe_hbox), pipe_encode_node_);
  gtk_box_append(GTK_BOX(pipe_hbox), make_chevron());

  pipe_network_node_ = MakePipelineNode("Network", "network-transmit-receive-symbolic", "—", &pipe_network_sub_);
  gtk_box_append(GTK_BOX(pipe_hbox), pipe_network_node_);
  gtk_box_append(GTK_BOX(pipe_hbox), make_chevron());

  pipe_tv_node_ = MakePipelineNode("TV", "video-display-symbolic", "—", &pipe_tv_sub_);
  gtk_box_append(GTK_BOX(pipe_hbox), pipe_tv_node_);

  gtk_box_append(GTK_BOX(pipe_card_), pipe_scroller);
  gtk_box_append(GTK_BOX(content_box), pipe_card_);

  // 2C. Health Card
  health_card_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(health_card_, "cm-health-card");
  gtk_widget_add_css_class(health_card_, "is-idle");

  health_icon_ = gtk_image_new_from_icon_name("dialog-information-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(health_icon_), 20);
  gtk_box_append(GTK_BOX(health_card_), health_icon_);

  health_lbl_ = gtk_label_new("Waiting for stream data…");
  gtk_widget_set_halign(health_lbl_, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(health_lbl_), TRUE);
  gtk_widget_set_hexpand(health_lbl_, TRUE);
  gtk_box_append(GTK_BOX(health_card_), health_lbl_);

  gtk_box_append(GTK_BOX(content_box), health_card_);

  // 2D. 9 Stat Cards in FlowBox
  GtkWidget* stats_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class(stats_section, "cm-section-card");
  GtkWidget* stats_header =
      MakeSectionHeader("Live telemetry", "Real-time transmission counters and decoder metrics");
  gtk_box_append(GTK_BOX(stats_section), stats_header);

  GtkWidget* flow_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow_box), 2);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 3);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow_box), TRUE);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_box), 12);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_box), 12);

  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Frame rate", "media-playback-start-symbolic", copy::kStatFpsHelp, &val_fps_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Video bitrate", "network-transmit-symbolic", copy::kStatBitrateHelp, &val_bitrate_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Round-trip time", "alarm-symbolic", copy::kStatRttHelp, &val_rtt_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Packet loss", "network-error-symbolic", copy::kStatLossHelp, &val_loss_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Target delay", "preferences-system-time-symbolic", copy::kStatDelayHelp, &val_delay_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Output", "video-single-display-symbolic", copy::kStatSizeHelp, &val_size_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Encoder", "applications-engineering-symbolic", copy::kStatEncoderHelp, &val_encoder_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Repairs", "error-correct-symbolic", copy::kStatRepairsHelp, &val_repairs_));
  gtk_flow_box_append(GTK_FLOW_BOX(flow_box),
                      MakeStatCard("Total sent", "document-send-symbolic", copy::kStatSentHelp, &val_sent_));

  gtk_box_append(GTK_BOX(stats_section), flow_box);
  gtk_box_append(GTK_BOX(content_box), stats_section);

  // 2E. Adaptive Quality Ladder
  GtkWidget* ladder_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class(ladder_section, "cm-section-card");
  GtkWidget* ladder_header =
      MakeSectionHeader("Adaptive quality ladder", "Dynamic resolution and bitrate scaling stages");
  gtk_box_append(GTK_BOX(ladder_section), ladder_header);

  GtkWidget* ladder_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ladder_scroller),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_NEVER);

  ladder_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_hexpand(ladder_box_, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ladder_scroller), ladder_box_);
  gtk_box_append(GTK_BOX(ladder_section), ladder_scroller);

  const char* const kRungNames[] = {
      "4K60", "4K30", "1440p60", "1080p60", "1080p30", "720p60", "720p30", "540p30"};
  for (int i = 0; i < 8; ++i) {
    GtkWidget* chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(chip, "cm-ladder-chip");
    gtk_widget_set_hexpand(chip, TRUE);

    GtkWidget* marker = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(marker), 14);
    gtk_box_append(GTK_BOX(chip), marker);

    GtkWidget* lbl = gtk_label_new(kRungNames[i]);
    gtk_box_append(GTK_BOX(chip), lbl);

    ladder_rungs_.push_back(chip);
    gtk_box_append(GTK_BOX(ladder_box_), chip);
  }

  ladder_caption_lbl_ = gtk_label_new(copy::kLadderCaption);
  gtk_widget_set_halign(ladder_caption_lbl_, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(ladder_caption_lbl_), TRUE);
  gtk_widget_add_css_class(ladder_caption_lbl_, "cm-section-description");
  gtk_box_append(GTK_BOX(ladder_section), ladder_caption_lbl_);

  gtk_box_append(GTK_BOX(content_box), ladder_section);

  // 2F. Activity Timeline
  GtkWidget* timeline_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class(timeline_section, "cm-section-card");
  GtkWidget* timeline_header =
      MakeSectionHeader("Session activity timeline", "Recent connection lifecycle events");
  gtk_box_append(GTK_BOX(timeline_section), timeline_header);

  timeline_list_ = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(timeline_list_), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(timeline_list_, "boxed-list");
  gtk_box_append(GTK_BOX(timeline_section), timeline_list_);

  gtk_box_append(GTK_BOX(content_box), timeline_section);

  gtk_stack_add_named(GTK_STACK(root_widget_), session_scroller_, "session");

  // 3. Failed Page (AdwStatusPage)
  failed_page_ = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(failed_page_), "dialog-error-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(failed_page_), "Casting ended");
  adw_status_page_set_description(ADW_STATUS_PAGE(failed_page_),
                                  "An unexpected error occurred.");
  GtkWidget* back_cast_btn = gtk_button_new_with_label("Back to Cast");
  gtk_widget_add_css_class(back_cast_btn, "pill");
  gtk_widget_add_css_class(back_cast_btn, "suggested-action");
  gtk_widget_set_halign(back_cast_btn, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(back_cast_btn), "win.page");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(back_cast_btn),
                                         g_variant_new_string("cast"));
  g_signal_connect_swapped(back_cast_btn, "clicked", G_CALLBACK(+[](LiveTab* self) {
    self->failure_visible_ = false;
  }), this);
  adw_status_page_set_child(ADW_STATUS_PAGE(failed_page_), back_cast_btn);
  gtk_stack_add_named(GTK_STACK(root_widget_), failed_page_, "failed");

  // Initial state: empty page visible
  gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "empty");
}

void LiveTab::SetHealthState(const std::string& text, const char* state_class, const char* icon_name) {
  if (health_lbl_) {
    gtk_label_set_text(GTK_LABEL(health_lbl_), text.c_str());
  }
  if (health_icon_ && icon_name) {
    gtk_image_set_from_icon_name(GTK_IMAGE(health_icon_), icon_name);
  }
  if (health_card_) {
    gtk_widget_remove_css_class(health_card_, "is-live");
    gtk_widget_remove_css_class(health_card_, "is-warning");
    gtk_widget_remove_css_class(health_card_, "is-idle");
    if (state_class && state_class[0] != '\0') {
      gtk_widget_add_css_class(health_card_, state_class);
    } else {
      gtk_widget_add_css_class(health_card_, "is-idle");
    }
  }
}

void LiveTab::UpdatePipelineDiagram(SessionState state, const StreamStats& stats) {
  auto set_node_state = [](GtkWidget* node, const char* state_class) {
    if (!node) return;
    gtk_widget_remove_css_class(node, "is-live");
    gtk_widget_remove_css_class(node, "is-progress");
    gtk_widget_remove_css_class(node, "is-warning");
    gtk_widget_remove_css_class(node, "is-idle");
    if (state_class && state_class[0] != '\0') {
      gtk_widget_add_css_class(node, state_class);
    } else {
      gtk_widget_add_css_class(node, "is-idle");
    }
  };

  // Update node labels
  if (!stats.display_name.empty() && pipe_screen_sub_) {
    std::string s = stats.display_name;
    if (stats.current_resolution.width > 0 && stats.current_resolution.height > 0) {
      s += " (" + std::to_string(stats.current_resolution.width) + " × " +
           std::to_string(stats.current_resolution.height) + ")";
    }
    gtk_label_set_text(GTK_LABEL(pipe_screen_sub_), s.c_str());
  }
  if (!stats.capture_backend.empty() && pipe_capture_sub_) {
    gtk_label_set_text(GTK_LABEL(pipe_capture_sub_), stats.capture_backend.c_str());
  }
  if (!stats.encoder_name.empty() && pipe_encode_sub_) {
    gtk_label_set_text(GTK_LABEL(pipe_encode_sub_), stats.encoder_name.c_str());
  }

  if (pipe_network_sub_) {
    std::ostringstream ss_net;
    ss_net << std::fixed << std::setprecision(1) << (stats.bitrate_kbps / 1000.0) << " Mbps";
    if (stats.packet_loss_fraction > 0.001) {
      ss_net << " (" << std::fixed << std::setprecision(1)
             << (stats.packet_loss_fraction * 100.0) << "% loss)";
    }
    gtk_label_set_text(GTK_LABEL(pipe_network_sub_), ss_net.str().c_str());
  }

  if (!stats.device_name.empty() && pipe_tv_sub_) {
    gtk_label_set_text(GTK_LABEL(pipe_tv_sub_), stats.device_name.c_str());
  }

  // Update active highlighting
  if (state == SessionState::kStreaming) {
    set_node_state(pipe_screen_node_, "is-live");
    set_node_state(pipe_capture_node_, "is-live");
    set_node_state(pipe_encode_node_, "is-live");
    if (stats.packet_loss_fraction >= 0.05 || stats.round_trip_time_ms >= 80.0) {
      set_node_state(pipe_network_node_, "is-warning");
    } else {
      set_node_state(pipe_network_node_, "is-live");
    }
    set_node_state(pipe_tv_node_, "is-live");
  } else if (state == SessionState::kConnecting) {
    set_node_state(pipe_screen_node_, "is-progress");
    set_node_state(pipe_capture_node_, "is-progress");
    set_node_state(pipe_encode_node_, "is-idle");
    set_node_state(pipe_network_node_, "is-idle");
    set_node_state(pipe_tv_node_, "is-idle");
  } else if (state == SessionState::kNegotiating) {
    set_node_state(pipe_screen_node_, "is-progress");
    set_node_state(pipe_capture_node_, "is-progress");
    set_node_state(pipe_encode_node_, "is-progress");
    set_node_state(pipe_network_node_, "is-idle");
    set_node_state(pipe_tv_node_, "is-idle");
  } else if (state == SessionState::kReconnecting) {
    set_node_state(pipe_screen_node_, "is-idle");
    set_node_state(pipe_capture_node_, "is-idle");
    set_node_state(pipe_encode_node_, "is-idle");
    set_node_state(pipe_network_node_, "is-warning");
    set_node_state(pipe_tv_node_, "is-warning");
  } else {
    set_node_state(pipe_screen_node_, "is-idle");
    set_node_state(pipe_capture_node_, "is-idle");
    set_node_state(pipe_encode_node_, "is-idle");
    set_node_state(pipe_network_node_, "is-idle");
    set_node_state(pipe_tv_node_, "is-idle");
  }
}

void LiveTab::UpdateLadderVisualization(int active_rung, int total_rungs, bool enabled) {
  (void)total_rungs;
  for (size_t i = 0; i < ladder_rungs_.size(); ++i) {
    GtkWidget* rung = ladder_rungs_[i];
    gtk_widget_remove_css_class(rung, "is-selected");
    gtk_widget_remove_css_class(rung, "is-disabled");

    if (!enabled) {
      gtk_widget_add_css_class(rung, "is-disabled");
    } else if (static_cast<int>(i) == active_rung) {
      gtk_widget_add_css_class(rung, "is-selected");
    }
  }

  gtk_label_set_text(GTK_LABEL(ladder_caption_lbl_),
                     enabled ? copy::kLadderCaption : copy::kLadderDisabledCaption);
}

void LiveTab::UpdateStats(const StreamStats& stats) {
  StreamStats cur = stats;
  if (g_debug_hold_warning.load()) {
    cur.packet_loss_fraction = 0.06;
    cur.round_trip_time_ms = 90;
    cur.health_hint = "Test network warning";
    if (cur.bitrate_kbps == 0) cur.bitrate_kbps = 8000;
    if (cur.current_fps == 0) cur.current_fps = 58.0;
  }
  last_stats_ = cur;

  if (!cur.device_name.empty()) {
    last_device_name_ = cur.device_name;
    gtk_label_set_text(GTK_LABEL(hero_title_lbl_),
                       ("Casting to " + cur.device_name).c_str());
  }

  // Update Hero Subtitle
  std::ostringstream ss_hero;
  ss_hero << cur.current_resolution.width << " × " << cur.current_resolution.height
          << " · " << std::fixed << std::setprecision(1) << cur.current_fps << " FPS"
          << " · " << std::fixed << std::setprecision(1) << (cur.bitrate_kbps / 1000.0) << " Mbps";
  gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_), ss_hero.str().c_str());

  // 1. Frame rate
  std::ostringstream ss_fps;
  ss_fps << std::fixed << std::setprecision(1) << cur.current_fps << " FPS";
  gtk_label_set_text(GTK_LABEL(val_fps_), ss_fps.str().c_str());

  // 2. Video bitrate
  std::ostringstream ss_bitrate;
  ss_bitrate << std::fixed << std::setprecision(2) << (cur.bitrate_kbps / 1000.0) << " Mbps";
  gtk_label_set_text(GTK_LABEL(val_bitrate_), ss_bitrate.str().c_str());

  // 3. Round-trip time
  std::ostringstream ss_rtt;
  ss_rtt << std::fixed << std::setprecision(0) << cur.round_trip_time_ms << " ms";
  gtk_label_set_text(GTK_LABEL(val_rtt_), ss_rtt.str().c_str());

  // 4. Packet loss
  std::ostringstream ss_loss;
  ss_loss << std::fixed << std::setprecision(1) << (cur.packet_loss_fraction * 100.0) << "%";
  gtk_label_set_text(GTK_LABEL(val_loss_), ss_loss.str().c_str());

  // 5. Target delay
  std::ostringstream ss_delay;
  ss_delay << cur.target_delay_ms << " ms";
  gtk_label_set_text(GTK_LABEL(val_delay_), ss_delay.str().c_str());

  // 6. Output
  std::ostringstream ss_size;
  ss_size << cur.current_resolution.width << " × " << cur.current_resolution.height
          << " · " << cur.current_framerate << " fps";
  gtk_label_set_text(GTK_LABEL(val_size_), ss_size.str().c_str());

  // 7. Encoder
  std::string enc = cur.encoder_name.empty() ? "—" : cur.encoder_name;
  if (!cur.active_codec.empty() && !cur.encoder_name.empty()) {
    enc += " · " + cur.active_codec;
  }
  gtk_label_set_text(GTK_LABEL(val_encoder_), enc.c_str());

  // 8. Repairs
  std::ostringstream ss_repairs;
  ss_repairs << cur.nacks_received << " NACK · " << cur.pli_received << " PLI";
  gtk_label_set_text(GTK_LABEL(val_repairs_), ss_repairs.str().c_str());

  // 9. Total sent
  std::ostringstream ss_sent;
  ss_sent << cur.frames_sent << " frames · " << cur.packets_sent << " packets";
  gtk_label_set_text(GTK_LABEL(val_sent_), ss_sent.str().c_str());

  SessionState current_state = app_ ? app_->GetCurrentState() : SessionState::kStreaming;
  UpdatePipelineDiagram(current_state, stats);
  UpdateLadderVisualization(cur.adaptive_rung_index, cur.adaptive_rung_count, cur.adaptive_enabled);

  // Health Card
  if (current_state == SessionState::kStreaming) {
    if (!cur.health_hint.empty()) {
      SetHealthState(cur.health_hint, "is-warning", "dialog-warning-symbolic");
      if (cur.health_hint != last_health_hint_) {
        AppendActivityEvent(cur.health_hint);
        last_health_hint_ = cur.health_hint;
      }
    } else {
      SetHealthState(copy::kHealthHealthy, "is-live", "emblem-ok-symbolic");
      last_health_hint_.clear();
    }
  }
}

void LiveTab::UpdateSessionState(SessionState state, const std::string& message) {
  if (!message.empty()) {
    AppendActivityEvent(message);
  }

  UpdateStatusBadge(hero_status_pill_, hero_status_dot_, hero_status_lbl_, state);

  if (state == SessionState::kFailed) {
    failure_visible_ = true;
    adw_status_page_set_description(
        ADW_STATUS_PAGE(failed_page_),
        message.empty() ? "Casting failed or disconnected unexpectedly." : message.c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "failed");
    current_ui_state_ = state;
    return;
  }

  if (state == SessionState::kConnecting) {
    failure_visible_ = false;
    if (current_ui_state_ == SessionState::kIdle ||
        current_ui_state_ == SessionState::kReady ||
        current_ui_state_ == SessionState::kFailed ||
        current_ui_state_ == SessionState::kDiscovering) {
      ResetSessionValues();
    }
    if (last_device_name_.empty()) {
      last_device_name_ = ConfigStore::Instance().Get().last_device_name;
    }
    gtk_label_set_text(GTK_LABEL(hero_title_lbl_),
                       last_device_name_.empty() ? "Starting cast…"
                                                 : ("Casting to " + last_device_name_).c_str());
    gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_),
                       message.empty() ? "Connecting to display…" : message.c_str());
    SetHealthState("Waiting for stream data…", "is-idle", "dialog-information-symbolic");
    UpdatePipelineDiagram(state, last_stats_);
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "session");
    current_ui_state_ = state;
    return;
  }

  if (state == SessionState::kNegotiating) {
    failure_visible_ = false;
    gtk_label_set_text(GTK_LABEL(hero_title_lbl_),
                       last_device_name_.empty() ? "Starting cast…"
                                                 : ("Casting to " + last_device_name_).c_str());
    gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_),
                       message.empty() ? "Negotiating stream parameters…" : message.c_str());
    SetHealthState("Waiting for stream data…", "is-idle", "dialog-information-symbolic");
    UpdatePipelineDiagram(state, last_stats_);
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "session");
    current_ui_state_ = state;
    return;
  }

  if (state == SessionState::kStreaming) {
    failure_visible_ = false;
    gtk_label_set_text(GTK_LABEL(hero_title_lbl_),
                       last_device_name_.empty() ? "Casting display"
                                                 : ("Casting to " + last_device_name_).c_str());
    if (last_stats_.bitrate_kbps == 0 && last_stats_.current_fps == 0) {
      gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_),
                         message.empty() ? "Streaming to display" : message.c_str());
    }
    if (!last_stats_.health_hint.empty()) {
      SetHealthState(last_stats_.health_hint, "is-warning", "dialog-warning-symbolic");
    } else {
      SetHealthState(copy::kHealthHealthy, "is-live", "emblem-ok-symbolic");
    }
    UpdatePipelineDiagram(state, last_stats_);
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "session");
    current_ui_state_ = state;
    return;
  }

  if (state == SessionState::kReconnecting) {
    failure_visible_ = false;
    gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_),
                       message.empty() ? "Reconnecting to display…" : message.c_str());
    SetHealthState(message.empty() ? "Reconnecting to display…" : message,
                   "is-warning",
                   "dialog-warning-symbolic");
    UpdatePipelineDiagram(state, last_stats_);
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "session");
    current_ui_state_ = state;
    return;
  }

  if (state == SessionState::kStopping) {
    failure_visible_ = false;
    gtk_label_set_text(GTK_LABEL(hero_subtitle_lbl_),
                       message.empty() ? "Stopping session…" : message.c_str());
    UpdatePipelineDiagram(state, last_stats_);
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "session");
    current_ui_state_ = state;
    return;
  }

  // Idle / Ready / Discovering
  if (failure_visible_) {
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "failed");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(root_widget_), "empty");
  }
  current_ui_state_ = state;
}

void LiveTab::AppendActivityEvent(const std::string& message) {
  if (message.empty()) return;

  std::string timestamp = GetCurrentTimestampStr();

  // Avoid consecutive duplicates
  if (!timeline_items_.empty() && timeline_items_.front() == message) {
    return;
  }
  timeline_items_.insert(timeline_items_.begin(), message);
  if (timeline_items_.size() > 20) {
    timeline_items_.pop_back();
  }

  // Create new row widget
  GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(row_box, "cm-timeline-row");

  GtkWidget* time_lbl = gtk_label_new(timestamp.c_str());
  gtk_widget_add_css_class(time_lbl, "dim-label");
  gtk_widget_add_css_class(time_lbl, "numeric");
  gtk_widget_set_valign(time_lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(row_box), time_lbl);

  GtkWidget* msg_lbl = gtk_label_new(message.c_str());
  gtk_widget_set_halign(msg_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(msg_lbl), TRUE);
  gtk_widget_set_hexpand(msg_lbl, TRUE);
  gtk_box_append(GTK_BOX(row_box), msg_lbl);

  // Prepend to list box
  gtk_list_box_prepend(GTK_LIST_BOX(timeline_list_), row_box);

  // Remove excess rows beyond 20
  GtkListBoxRow* excess_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(timeline_list_), 20);
  if (excess_row != nullptr) {
    gtk_list_box_remove(GTK_LIST_BOX(timeline_list_), GTK_WIDGET(excess_row));
  }
}

void LiveTab::ResetSessionValues() {
  timeline_items_.clear();
  last_health_hint_.clear();
  last_stats_ = StreamStats{};

  if (timeline_list_) {
    while (GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(timeline_list_), 0)) {
      gtk_list_box_remove(GTK_LIST_BOX(timeline_list_), GTK_WIDGET(row));
    }
  }

  if (val_fps_) gtk_label_set_text(GTK_LABEL(val_fps_), "—");
  if (val_bitrate_) gtk_label_set_text(GTK_LABEL(val_bitrate_), "—");
  if (val_rtt_) gtk_label_set_text(GTK_LABEL(val_rtt_), "—");
  if (val_loss_) gtk_label_set_text(GTK_LABEL(val_loss_), "—");
  if (val_delay_) gtk_label_set_text(GTK_LABEL(val_delay_), "—");
  if (val_size_) gtk_label_set_text(GTK_LABEL(val_size_), "—");
  if (val_encoder_) gtk_label_set_text(GTK_LABEL(val_encoder_), "—");
  if (val_repairs_) gtk_label_set_text(GTK_LABEL(val_repairs_), "—");
  if (val_sent_) gtk_label_set_text(GTK_LABEL(val_sent_), "—");

  if (pipe_screen_sub_) gtk_label_set_text(GTK_LABEL(pipe_screen_sub_), "—");
  if (pipe_capture_sub_) gtk_label_set_text(GTK_LABEL(pipe_capture_sub_), "—");
  if (pipe_encode_sub_) gtk_label_set_text(GTK_LABEL(pipe_encode_sub_), "—");
  if (pipe_network_sub_) gtk_label_set_text(GTK_LABEL(pipe_network_sub_), "—");
  if (pipe_tv_sub_) gtk_label_set_text(GTK_LABEL(pipe_tv_sub_), "—");

  SetHealthState("Waiting for stream data…", "is-idle", "dialog-information-symbolic");
  UpdateLadderVisualization(-1, 8, false);

  if (session_scroller_) {
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(session_scroller_));
    if (vadj) {
      gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
    }
  }
}
void DebugInjectHealthWarning() {
  FILE* f = fopen("/tmp/debug_inject.log", "a");
  if (f) { fprintf(f, "DebugInject called hold=%d\n", (int)g_debug_hold_warning.load()); fclose(f); }
  g_debug_hold_warning = true;
  if (f) { f=fopen("/tmp/debug_inject.log","a"); fprintf(f, "hold set true\n"); fclose(f); }
  g_timeout_add_seconds(5, +[](gpointer) -> gboolean { g_debug_hold_warning = false; FILE* ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"hold cleared\n"); fclose(ff);} return G_SOURCE_REMOVE; }, nullptr);
  if (g_debug_live_tab) {
    FILE* ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"using global live %p\n", g_debug_live_tab); fclose(ff);}
    // Direct warning injection on main thread without relying on UpdateStats hold
    g_main_context_invoke(nullptr, +[](gpointer data)->gboolean {
      LiveTab* live = static_cast<LiveTab*>(data);
      live->SetHealthState("Test network warning", "is-warning", "dialog-warning-symbolic");
      if (live->pipe_network_node_) {
        gtk_widget_remove_css_class(live->pipe_network_node_, "is-live");
        gtk_widget_remove_css_class(live->pipe_network_node_, "is-idle");
        gtk_widget_remove_css_class(live->pipe_network_node_, "is-progress");
        gtk_widget_add_css_class(live->pipe_network_node_, "is-warning");
        if (live->pipe_network_sub_) gtk_label_set_text(GTK_LABEL(live->pipe_network_sub_), "12.0 Mbps (6.0% loss)");
      }
      if (live->val_loss_) gtk_label_set_text(GTK_LABEL(live->val_loss_), "6.0%");
      if (live->val_rtt_) gtk_label_set_text(GTK_LABEL(live->val_rtt_), "90 ms");
      live->AppendActivityEvent("Test network warning");
      FILE* fff=fopen("/tmp/debug_inject.log","a"); if(fff){fprintf(fff,"direct warning applied\n"); fclose(fff);}
      return G_SOURCE_REMOVE;
    }, g_debug_live_tab);
    // Keep warning visible for 5s before next healthy sample clears it
    g_timeout_add_seconds(5, +[](gpointer data)->gboolean {
      LiveTab* live = static_cast<LiveTab*>(data);
      // Next healthy UpdateStats will clear, but also reset pipeline
      if (live->pipe_network_node_) {
        // Let next real stats restore; just clear hold flag is enough, but also force a healthy sample
        StreamStats st = live->last_stats_;
        st.health_hint.clear();
        st.packet_loss_fraction = 0.0;
        st.round_trip_time_ms = 24;
        live->UpdateStats(st);
      }
      FILE* fff=fopen("/tmp/debug_inject.log","a"); if(fff){fprintf(fff,"warning cleared\n"); fclose(fff);}
      g_debug_hold_warning.store(false);
      return G_SOURCE_REMOVE;
    }, g_debug_live_tab);
    FILE* ff2=fopen("/tmp/debug_inject.log","a"); if(ff2){fprintf(ff2,"scheduled warning 5s\n"); fclose(ff2);}
    return;
  }
  g_main_context_invoke(nullptr, +[](gpointer) -> gboolean {
    FILE* ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"invoke lambda start\n"); fclose(ff);}
    GApplication* gapp = g_application_get_default();
    if (!gapp) { ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"no gapp\n"); fclose(ff);} return G_SOURCE_REMOVE; }
    GtkWindow* win = nullptr;
    if (GTK_IS_APPLICATION(gapp)) {
      win = gtk_application_get_active_window(GTK_APPLICATION(gapp));
      if (!win) {
        GList* wins = gtk_application_get_windows(GTK_APPLICATION(gapp));
        if (wins && wins->data) win = GTK_WINDOW(wins->data);
      }
    }
    if (!win) { ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"no win\n"); fclose(ff);} return G_SOURCE_REMOVE; }
    auto* gui = static_cast<GuiApp*>(g_object_get_data(G_OBJECT(win), "castmirror-gui-app"));
    if (!gui) { ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"no gui\n"); fclose(ff);} return G_SOURCE_REMOVE; }
    LiveTab* live = gui->GetLiveTab();
    if (!live) { ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"no live\n"); fclose(ff);} return G_SOURCE_REMOVE; }
    StreamStats st = live->last_stats_;
    st.packet_loss_fraction = 0.06;
    st.round_trip_time_ms = 90;
    st.health_hint = "Test network warning";
    if (st.bitrate_kbps == 0) st.bitrate_kbps = 8000;
    if (st.current_fps == 0) st.current_fps = 58.0;
    if (st.current_resolution.width == 0) st.current_resolution = {1920, 1080};
    live->UpdateStats(st);
    ff=fopen("/tmp/debug_inject.log","a"); if(ff){fprintf(ff,"invoke UpdateStats done\n"); fclose(ff);}
    return G_SOURCE_REMOVE;
  }, nullptr);
  f=fopen("/tmp/debug_inject.log","a"); if(f){fprintf(f,"scheduled invoke\n"); fclose(f);}
}


}  // namespace castcore::gui
