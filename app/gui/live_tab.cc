#include "live_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "help_copy.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace castcore::gui {

namespace {

GtkWidget* MakePipeNode(const char* title, const char* default_sub, GtkWidget** out_sub) {
  GtkWidget* node = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_style_context_add_class(gtk_widget_get_style_context(node), "pipe-node");

  GtkWidget* tlbl = gtk_label_new(title);
  gtk_widget_set_halign(tlbl, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(tlbl), "pipe-node-title");
  gtk_box_pack_start(GTK_BOX(node), tlbl, FALSE, FALSE, 0);

  GtkWidget* slbl = gtk_label_new(default_sub);
  gtk_widget_set_halign(slbl, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(slbl), "pipe-node-sub");
  gtk_box_pack_start(GTK_BOX(node), slbl, FALSE, FALSE, 0);

  if (out_sub) {
    *out_sub = slbl;
  }
  return node;
}

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

}  // namespace

LiveTab::LiveTab(GuiApp* app) : app_(app) {
  BuildUi();
}

LiveTab::~LiveTab() = default;

void LiveTab::BuildUi() {
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  root_widget_ = scroller;

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_container_set_border_width(GTK_CONTAINER(content_box), 18);
  gtk_container_add(GTK_CONTAINER(scroller), content_box);

  // 1. Empty State Card
  empty_card_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_card_), "card-box");

  GtkWidget* empty_title = gtk_label_new(copy::kLiveEmptyTitle);
  gtk_widget_set_halign(empty_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_title), "setting-title");
  gtk_box_pack_start(GTK_BOX(empty_card_), empty_title, FALSE, FALSE, 0);

  GtkWidget* empty_body = gtk_label_new(copy::kLiveEmptyBody);
  gtk_widget_set_halign(empty_body, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(empty_body), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_body), "setting-help");
  gtk_box_pack_start(GTK_BOX(empty_card_), empty_body, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), empty_card_, FALSE, FALSE, 0);

  // 2. Active Session Live Container
  live_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_box_pack_start(GTK_BOX(content_box), live_container_, FALSE, FALSE, 0);

  // 2A. Pipeline Diagram Box
  GtkWidget* pipe_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(pipe_card), "pipe-container");

  GtkWidget* pipe_header = MakeSectionHeader("SESSION PIPELINE", "Live video flow from display capture to TV decoder");
  gtk_box_pack_start(GTK_BOX(pipe_card), pipe_header, FALSE, FALSE, 0);

  GtkWidget* pipe_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_hexpand(pipe_hbox, TRUE);

  pipe_screen_node_ = MakePipeNode("Screen", "Display 0", &pipe_screen_sub_);
  gtk_widget_set_hexpand(pipe_screen_node_, TRUE);
  gtk_box_pack_start(GTK_BOX(pipe_hbox), pipe_screen_node_, TRUE, TRUE, 0);

  auto make_arrow = []() -> GtkWidget* {
    GtkWidget* arr = gtk_label_new("→");
    gtk_style_context_add_class(gtk_widget_get_style_context(arr), "pipe-arrow");
    return arr;
  };

  gtk_box_pack_start(GTK_BOX(pipe_hbox), make_arrow(), FALSE, FALSE, 0);
  pipe_capture_node_ = MakePipeNode("Capture", "X11", &pipe_capture_sub_);
  gtk_widget_set_hexpand(pipe_capture_node_, TRUE);
  gtk_box_pack_start(GTK_BOX(pipe_hbox), pipe_capture_node_, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(pipe_hbox), make_arrow(), FALSE, FALSE, 0);
  pipe_encode_node_ = MakePipeNode("Encode", "h264_vaapi", &pipe_encode_sub_);
  gtk_widget_set_hexpand(pipe_encode_node_, TRUE);
  gtk_box_pack_start(GTK_BOX(pipe_hbox), pipe_encode_node_, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(pipe_hbox), make_arrow(), FALSE, FALSE, 0);
  pipe_network_node_ = MakePipeNode("Network", "8.0 Mbps", &pipe_network_sub_);
  gtk_widget_set_hexpand(pipe_network_node_, TRUE);
  gtk_box_pack_start(GTK_BOX(pipe_hbox), pipe_network_node_, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(pipe_hbox), make_arrow(), FALSE, FALSE, 0);
  pipe_tv_node_ = MakePipeNode("TV", "Living Room", &pipe_tv_sub_);
  gtk_widget_set_hexpand(pipe_tv_node_, TRUE);
  gtk_box_pack_start(GTK_BOX(pipe_hbox), pipe_tv_node_, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(pipe_card), pipe_hbox, FALSE, FALSE, 0);

  // Health Banner
  health_banner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(health_banner_), "health-banner");
  health_banner_lbl_ = gtk_label_new(copy::kHealthHealthy);
  gtk_widget_set_halign(health_banner_lbl_, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(health_banner_), health_banner_lbl_, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(pipe_card), health_banner_, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(live_container_), pipe_card, FALSE, FALSE, 0);

  // 2B. 3x3 Telemetry Grid
  GtkWidget* stats_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(stats_section), "card-box");
  GtkWidget* stats_header = MakeSectionHeader("LIVE TELEMETRY", "Real-time transmission counters and decoder metrics");
  gtk_box_pack_start(GTK_BOX(stats_section), stats_header, FALSE, FALSE, 0);

  GtkWidget* sgrid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(sgrid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(sgrid), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(sgrid), TRUE);

  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("FRAME RATE", copy::kStatFpsHelp, &val_fps_), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("VIDEO BITRATE", copy::kStatBitrateHelp, &val_bitrate_), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("ROUND TRIP (RTT)", copy::kStatRttHelp, &val_rtt_), 2, 0, 1, 1);

  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("PACKET LOSS", copy::kStatLossHelp, &val_loss_), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("TARGET DELAY", copy::kStatDelayHelp, &val_delay_), 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("OUTPUT SIZE", copy::kStatSizeHelp, &val_size_), 2, 1, 1, 1);

  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("ENCODER", copy::kStatEncoderHelp, &val_encoder_), 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("REPAIRS", copy::kStatRepairsHelp, &val_repairs_), 1, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), MakeStatCard("TOTAL SENT", copy::kStatSentHelp, &val_sent_), 2, 2, 1, 1);

  gtk_box_pack_start(GTK_BOX(stats_section), sgrid, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(live_container_), stats_section, FALSE, FALSE, 0);

  // 2C. Adaptive Quality Ladder
  GtkWidget* ladder_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(ladder_section), "card-box");
  GtkWidget* ladder_header = MakeSectionHeader("ADAPTIVE QUALITY LADDER", "Dynamic resolution and bitrate scaling stages");
  gtk_box_pack_start(GTK_BOX(ladder_section), ladder_header, FALSE, FALSE, 0);

  ladder_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_hexpand(ladder_box_, TRUE);

  const char* const kRungNames[] = {"4K60", "4K30", "1440p60", "1080p60", "1080p30", "720p60", "720p30", "540p30"};
  for (int i = 0; i < 8; ++i) {
    GtkWidget* rung = gtk_label_new(kRungNames[i]);
    gtk_widget_set_hexpand(rung, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(rung), "ladder-rung");
    gtk_box_pack_start(GTK_BOX(ladder_box_), rung, TRUE, TRUE, 0);
    ladder_rungs_.push_back(rung);
  }
  gtk_box_pack_start(GTK_BOX(ladder_section), ladder_box_, FALSE, FALSE, 0);

  ladder_caption_lbl_ = gtk_label_new(copy::kLadderCaption);
  gtk_widget_set_halign(ladder_caption_lbl_, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(ladder_caption_lbl_), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(ladder_caption_lbl_), "card-desc");
  gtk_box_pack_start(GTK_BOX(ladder_section), ladder_caption_lbl_, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(live_container_), ladder_section, FALSE, FALSE, 0);

  // 2D. Activity Timeline
  GtkWidget* timeline_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(timeline_section), "card-box");
  GtkWidget* timeline_header = MakeSectionHeader("SESSION ACTIVITY TIMELINE", "Recent connection lifecycle events");
  gtk_box_pack_start(GTK_BOX(timeline_section), timeline_header, FALSE, FALSE, 0);

  timeline_list_ = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(timeline_list_), GTK_SELECTION_NONE);
  gtk_box_pack_start(GTK_BOX(timeline_section), timeline_list_, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(live_container_), timeline_section, FALSE, FALSE, 0);

  // Initial state: empty card visible, live container hidden
  gtk_widget_set_visible(empty_card_, TRUE);
  gtk_widget_set_visible(live_container_, FALSE);
}

void LiveTab::UpdatePipelineDiagram(SessionState state, const StreamStats& stats) {
  auto set_node_class = [](GtkWidget* node, const char* cls) {
    GtkStyleContext* ctx = gtk_widget_get_style_context(node);
    gtk_style_context_remove_class(ctx, "pipe-node-active");
    gtk_style_context_remove_class(ctx, "pipe-node-warn");
    if (cls) {
      gtk_style_context_add_class(ctx, cls);
    }
  };

  // Update node labels
  if (!stats.display_name.empty()) {
    std::string s = stats.display_name + " (" + std::to_string(stats.current_resolution.width) + "x" + std::to_string(stats.current_resolution.height) + ")";
    gtk_label_set_text(GTK_LABEL(pipe_screen_sub_), s.c_str());
  }
  if (!stats.capture_backend.empty()) {
    gtk_label_set_text(GTK_LABEL(pipe_capture_sub_), stats.capture_backend.c_str());
  }
  if (!stats.encoder_name.empty()) {
    gtk_label_set_text(GTK_LABEL(pipe_encode_sub_), stats.encoder_name.c_str());
  }

  std::ostringstream ss_net;
  ss_net << std::fixed << std::setprecision(1) << (stats.bitrate_kbps / 1000.0) << " Mbps";
  if (stats.packet_loss_fraction > 0.001) {
    ss_net << " (" << std::fixed << std::setprecision(1) << (stats.packet_loss_fraction * 100.0) << "% loss)";
  }
  gtk_label_set_text(GTK_LABEL(pipe_network_sub_), ss_net.str().c_str());

  if (!stats.device_name.empty()) {
    gtk_label_set_text(GTK_LABEL(pipe_tv_sub_), stats.device_name.c_str());
  }

  // Update active highlighting
  if (state == SessionState::kStreaming) {
    set_node_class(pipe_screen_node_, "pipe-node-active");
    set_node_class(pipe_capture_node_, "pipe-node-active");
    set_node_class(pipe_encode_node_, "pipe-node-active");
    if (stats.packet_loss_fraction >= 0.05 || stats.round_trip_time_ms >= 80.0) {
      set_node_class(pipe_network_node_, "pipe-node-warn");
    } else {
      set_node_class(pipe_network_node_, "pipe-node-active");
    }
    set_node_class(pipe_tv_node_, "pipe-node-active");
  } else if (state == SessionState::kConnecting) {
    set_node_class(pipe_screen_node_, "pipe-node-active");
    set_node_class(pipe_capture_node_, "pipe-node-active");
    set_node_class(pipe_encode_node_, nullptr);
    set_node_class(pipe_network_node_, nullptr);
    set_node_class(pipe_tv_node_, nullptr);
  } else if (state == SessionState::kNegotiating) {
    set_node_class(pipe_screen_node_, "pipe-node-active");
    set_node_class(pipe_capture_node_, "pipe-node-active");
    set_node_class(pipe_encode_node_, "pipe-node-active");
    set_node_class(pipe_network_node_, nullptr);
    set_node_class(pipe_tv_node_, nullptr);
  } else if (state == SessionState::kReconnecting) {
    set_node_class(pipe_screen_node_, nullptr);
    set_node_class(pipe_capture_node_, nullptr);
    set_node_class(pipe_encode_node_, nullptr);
    set_node_class(pipe_network_node_, "pipe-node-warn");
    set_node_class(pipe_tv_node_, "pipe-node-warn");
  } else {
    set_node_class(pipe_screen_node_, nullptr);
    set_node_class(pipe_capture_node_, nullptr);
    set_node_class(pipe_encode_node_, nullptr);
    set_node_class(pipe_network_node_, nullptr);
    set_node_class(pipe_tv_node_, nullptr);
  }
}

void LiveTab::UpdateLadderVisualization(int active_rung, int total_rungs, bool enabled) {
  (void)total_rungs;
  for (size_t i = 0; i < ladder_rungs_.size(); ++i) {
    GtkStyleContext* ctx = gtk_widget_get_style_context(ladder_rungs_[i]);
    gtk_style_context_remove_class(ctx, "ladder-rung-active");
    gtk_style_context_remove_class(ctx, "ladder-rung-disabled");

    if (!enabled) {
      gtk_style_context_add_class(ctx, "ladder-rung-disabled");
    } else if (static_cast<int>(i) == active_rung) {
      gtk_style_context_add_class(ctx, "ladder-rung-active");
    }
  }

  gtk_label_set_text(GTK_LABEL(ladder_caption_lbl_),
                     enabled ? copy::kLadderCaption : copy::kLadderDisabledCaption);
}

void LiveTab::UpdateStats(const StreamStats& stats) {
  std::ostringstream ss_fps, ss_bitrate, ss_rtt, ss_loss, ss_delay, ss_size, ss_repairs, ss_sent;

  ss_fps << std::fixed << std::setprecision(1) << stats.current_fps << " FPS";
  ss_bitrate << std::fixed << std::setprecision(2) << (stats.bitrate_kbps / 1000.0) << " Mbps";
  ss_rtt << std::fixed << std::setprecision(0) << stats.round_trip_time_ms << " ms";
  ss_loss << std::fixed << std::setprecision(1) << (stats.packet_loss_fraction * 100.0) << " %";
  ss_delay << stats.target_delay_ms << " ms";
  ss_size << stats.current_resolution.width << "x" << stats.current_resolution.height << " @" << stats.current_framerate;

  std::string enc = stats.encoder_name.empty() ? "—" : stats.encoder_name;
  if (!stats.active_codec.empty() && !stats.encoder_name.empty()) {
    enc += " (" + stats.active_codec + ")";
  }

  ss_repairs << stats.nacks_received << " NACK  •  " << stats.pli_received << " PLI";
  ss_sent << stats.frames_sent << " frames  •  " << stats.packets_sent << " pkts";

  gtk_label_set_text(GTK_LABEL(val_fps_), ss_fps.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_bitrate_), ss_bitrate.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_rtt_), ss_rtt.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_loss_), ss_loss.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_delay_), ss_delay.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_size_), ss_size.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_encoder_), enc.c_str());
  gtk_label_set_text(GTK_LABEL(val_repairs_), ss_repairs.str().c_str());
  gtk_label_set_text(GTK_LABEL(val_sent_), ss_sent.str().c_str());

  UpdatePipelineDiagram(app_->GetCurrentState(), stats);
  UpdateLadderVisualization(stats.adaptive_rung_index, stats.adaptive_rung_count, stats.adaptive_enabled);

  // Health Hint banner update
  GtkStyleContext* hctx = gtk_widget_get_style_context(health_banner_);
  gtk_style_context_remove_class(hctx, "health-banner-warn");
  gtk_style_context_remove_class(hctx, "health-banner-error");

  if (!stats.health_hint.empty()) {
    gtk_label_set_text(GTK_LABEL(health_banner_lbl_), stats.health_hint.c_str());
    gtk_style_context_add_class(hctx, "health-banner-warn");

    if (stats.health_hint != last_health_hint_) {
      AppendActivityEvent(stats.health_hint);
      last_health_hint_ = stats.health_hint;
    }
  } else {
    gtk_label_set_text(GTK_LABEL(health_banner_lbl_), copy::kHealthHealthy);
    last_health_hint_.clear();
  }
}

void LiveTab::UpdateSessionState(SessionState state, const std::string& message) {
  bool is_active = (state == SessionState::kStreaming || state == SessionState::kConnecting ||
                    state == SessionState::kNegotiating || state == SessionState::kReconnecting ||
                    state == SessionState::kStopping);

  gtk_widget_set_visible(empty_card_, !is_active);
  gtk_widget_set_visible(live_container_, is_active);

  if (!message.empty()) {
    AppendActivityEvent(message);
  }

  GtkStyleContext* hctx = gtk_widget_get_style_context(health_banner_);
  gtk_style_context_remove_class(hctx, "health-banner-warn");
  gtk_style_context_remove_class(hctx, "health-banner-error");

  if (state == SessionState::kFailed) {
    gtk_style_context_add_class(hctx, "health-banner-error");
    gtk_label_set_text(GTK_LABEL(health_banner_lbl_), message.empty() ? "Connection failed" : message.c_str());
  } else if (state == SessionState::kReconnecting) {
    gtk_style_context_add_class(hctx, "health-banner-warn");
    gtk_label_set_text(GTK_LABEL(health_banner_lbl_), message.empty() ? "Reconnecting..." : message.c_str());
  }
}

void LiveTab::AppendActivityEvent(const std::string& message) {
  if (message.empty()) return;

  std::string timestamp = GetCurrentTimestampStr();
  std::string full_line = timestamp + "  " + message;

  // Avoid consecutive duplicates
  if (!timeline_items_.empty() && timeline_items_.front() == full_line) {
    return;
  }

  timeline_items_.insert(timeline_items_.begin(), full_line);
  if (timeline_items_.size() > 20) {
    timeline_items_.pop_back();
  }

  // Re-populate list box (max 20 items)
  GList* children = gtk_container_get_children(GTK_CONTAINER(timeline_list_));
  for (GList* iter = children; iter != nullptr; iter = g_list_next(iter)) {
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  }
  g_list_free(children);

  for (const auto& item : timeline_items_) {
    GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(row_box), "timeline-row");

    size_t space_pos = item.find("  ");
    std::string time_part = (space_pos != std::string::npos) ? item.substr(0, space_pos) : "";
    std::string msg_part = (space_pos != std::string::npos) ? item.substr(space_pos + 2) : item;

    GtkWidget* time_lbl = gtk_label_new(time_part.c_str());
    gtk_style_context_add_class(gtk_widget_get_style_context(time_lbl), "timeline-time");
    gtk_box_pack_start(GTK_BOX(row_box), time_lbl, FALSE, FALSE, 0);

    GtkWidget* msg_lbl = gtk_label_new(msg_part.c_str());
    gtk_widget_set_halign(msg_lbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(msg_lbl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(msg_lbl), "timeline-msg");
    gtk_box_pack_start(GTK_BOX(row_box), msg_lbl, TRUE, TRUE, 0);

    gtk_list_box_insert(GTK_LIST_BOX(timeline_list_), row_box, -1);
  }
  gtk_widget_show_all(timeline_list_);
}

}  // namespace castcore::gui
