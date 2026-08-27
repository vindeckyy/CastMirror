#include "castcore/display_capture.h"
#include "castcore/config.h"
#include "castcore/logger.h"

#if defined(CASTCORE_HAVE_PIPEWIRE)

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

namespace castcore {

namespace {

struct PortalResponseData {
  GMainLoop* loop = nullptr;
  guint response_code = 1;
  GVariant* results = nullptr;
};

void OnPortalSignal(GDBusConnection* conn,
                    const gchar* sender_name,
                    const gchar* object_path,
                    const gchar* interface_name,
                    const gchar* signal_name,
                    GVariant* parameters,
                    gpointer user_data) {
  (void)conn; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
  auto* data = static_cast<PortalResponseData*>(user_data);
  if (parameters) {
    g_variant_get(parameters, "(u@a{sv})", &data->response_code, &data->results);
    if (data->results) {
      g_variant_ref(data->results);
    }
  }
  if (data->loop && g_main_loop_is_running(data->loop)) {
    g_main_loop_quit(data->loop);
  }
}

bool CallPortalRequest(GDBusConnection* conn,
                       const char* method_name,
                       GVariant* parameters,
                       GVariant** out_results) {
  GError* err = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      conn,
      "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast",
      method_name,
      parameters,
      G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      10000,
      nullptr,
      &err);

  if (!reply) {
    LOG_ERROR << "Portal " << method_name << " failed: " << (err ? err->message : "unknown");
    if (err) g_error_free(err);
    return false;
  }

  const char* request_handle = nullptr;
  g_variant_get(reply, "(&o)", &request_handle);

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  PortalResponseData resp_data;
  resp_data.loop = loop;

  guint sub_id = g_dbus_connection_signal_subscribe(
      conn,
      "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request",
      "Response",
      request_handle,
      nullptr,
      G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
      OnPortalSignal,
      &resp_data,
      nullptr);

  // Timeout after 60 seconds (user interaction with screen-share dialog)
  GSource* timeout_source = g_timeout_source_new_seconds(60);
  g_source_set_callback(timeout_source, +[](gpointer data) -> gboolean {
    auto* d = static_cast<PortalResponseData*>(data);
    if (d->loop && g_main_loop_is_running(d->loop)) {
      g_main_loop_quit(d->loop);
    }
    return G_SOURCE_REMOVE;
  }, &resp_data, nullptr);
  g_source_attach(timeout_source, g_main_loop_get_context(loop));

  g_main_loop_run(loop);

  g_source_destroy(timeout_source);
  g_source_unref(timeout_source);
  g_dbus_connection_signal_unsubscribe(conn, sub_id);
  g_main_loop_unref(loop);
  g_variant_unref(reply);

  if (resp_data.response_code != 0 || !resp_data.results) {
    LOG_WARN << "Portal " << method_name << " response code: " << resp_data.response_code;
    if (resp_data.results) g_variant_unref(resp_data.results);
    return false;
  }

  *out_results = resp_data.results;
  return true;
}

} // namespace

class PipeWirePortalCapture : public IDisplayCapture {
 public:
  PipeWirePortalCapture() = default;
  std::string BackendName() const override { return "PipeWire"; }
  ~PipeWirePortalCapture() override { Stop(); }

  bool Start(int display_id, int target_fps) override {
    Stop();
    target_fps_.store(target_fps > 0 ? target_fps : 60);

    uint32_t node_id = 0;
    if (!RequestPortalSession(&node_id)) {
      LOG_ERROR << "Failed to acquire PipeWire stream node from xdg-desktop-portal";
      return false;
    }

    running_ = true;
    capture_thread_ = std::thread(&PipeWirePortalCapture::StreamLoop, this, node_id, target_fps_.load());
    return true;
  }

  void Stop() override {
    if (!running_.exchange(false)) return;
    if (pw_loop_) {
      pw_main_loop_quit(pw_loop_);
    }
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    if (pw_stream_) {
      pw_stream_destroy(pw_stream_);
      pw_stream_ = nullptr;
    }
    if (pw_core_) {
      pw_core_disconnect(pw_core_);
      pw_core_ = nullptr;
    }
    if (pw_context_) {
      pw_context_destroy(pw_context_);
      pw_context_ = nullptr;
    }
    if (pw_loop_) {
      pw_main_loop_destroy(pw_loop_);
      pw_loop_ = nullptr;
    }
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  void SetTargetFps(int fps) override {
    if (fps > 0) {
      target_fps_.store(fps);
    }
  }

  bool SizeKnownBeforeStart() const override {
    return false;
  }

  std::vector<DisplayInfo> EnumerateDisplays() override {
    std::vector<DisplayInfo> list;
    DisplayInfo info;
    info.id = 0;
    info.name = "Wayland (pick in system dialog)";
    info.width = 1920;
    info.height = 1080;
    info.refresh_rate = 60;
    info.is_primary = true;
    list.push_back(info);
    return list;
  }

  void SetFrameCallback(FrameCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

 private:
  bool RequestPortalSession(uint32_t* out_node_id) {
    GError* err = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn) {
      LOG_ERROR << "Failed to connect to D-Bus session bus: " << (err ? err->message : "unknown");
      if (err) g_error_free(err);
      return false;
    }

    std::string token_str = "castmirror_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    // 1. CreateSession
    GVariantBuilder create_builder;
    g_variant_builder_init(&create_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&create_builder, "{sv}", "session_handle_token", g_variant_new_string(token_str.c_str()));
    g_variant_builder_add(&create_builder, "{sv}", "handle_token", g_variant_new_string((token_str + "_req").c_str()));

    GVariant* create_results = nullptr;
    if (!CallPortalRequest(conn, "CreateSession", g_variant_new("(@a{sv})", g_variant_builder_end(&create_builder)), &create_results)) {
      g_object_unref(conn);
      return false;
    }

    const char* session_handle = nullptr;
    g_variant_lookup(create_results, "session_handle", "&s", &session_handle);
    if (!session_handle) {
      LOG_ERROR << "No session_handle in CreateSession response";
      g_variant_unref(create_results);
      g_object_unref(conn);
      return false;
    }
    std::string session_path(session_handle);
    g_variant_unref(create_results);

    // 2. SelectSources
    GVariantBuilder select_builder;
    g_variant_builder_init(&select_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&select_builder, "{sv}", "types", g_variant_new_uint32(1)); // 1 = Monitor
    g_variant_builder_add(&select_builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&select_builder, "{sv}", "persist_mode", g_variant_new_uint32(2)); // 2 = Persist until revoked

    const auto& cfg = ConfigStore::Instance().Get();
    if (!cfg.portal_restore_token.empty()) {
      g_variant_builder_add(&select_builder, "{sv}", "restore_token", g_variant_new_string(cfg.portal_restore_token.c_str()));
    }

    GVariant* select_results = nullptr;
    if (!CallPortalRequest(conn, "SelectSources", g_variant_new("(o@a{sv})", session_path.c_str(), g_variant_builder_end(&select_builder)), &select_results)) {
      g_object_unref(conn);
      return false;
    }
    if (select_results) g_variant_unref(select_results);

    // 3. Start
    GVariantBuilder start_builder;
    g_variant_builder_init(&start_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&start_builder, "{sv}", "handle_token", g_variant_new_string((token_str + "_start").c_str()));

    GVariant* start_results = nullptr;
    if (!CallPortalRequest(conn, "Start", g_variant_new("(os@a{sv})", session_path.c_str(), "", g_variant_builder_end(&start_builder)), &start_results)) {
      g_object_unref(conn);
      return false;
    }

    GVariant* streams = nullptr;
    g_variant_lookup(start_results, "streams", "@a(ua{sv})", &streams);
    if (!streams) {
      LOG_ERROR << "No streams returned from ScreenCast.Start";
      g_variant_unref(start_results);
      g_object_unref(conn);
      return false;
    }

    GVariantIter iter;
    g_variant_iter_init(&iter, streams);
    uint32_t node_id = 0;
    GVariant* stream_props = nullptr;
    if (g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &stream_props)) {
      *out_node_id = node_id;
      if (stream_props) g_variant_unref(stream_props);
    }
    g_variant_unref(streams);

    const char* new_restore_token = nullptr;
    if (g_variant_lookup(start_results, "restore_token", "&s", &new_restore_token)) {
      if (new_restore_token && new_restore_token[0] != '\0') {
        ConfigStore::Instance().Mutable().portal_restore_token = new_restore_token;
        ConfigStore::Instance().Save();
      }
    }

    g_variant_unref(start_results);
    g_object_unref(conn);
    return *out_node_id != 0;
  }

  void StreamLoop(uint32_t node_id, int target_fps) {
    pw_init(nullptr, nullptr);
    pw_loop_ = pw_main_loop_new(nullptr);
    if (!pw_loop_) {
      LOG_ERROR << "Failed to create PipeWire main loop";
      return;
    }

    pw_context_ = pw_context_new(pw_main_loop_get_loop(pw_loop_), nullptr, 0);
    if (!pw_context_) {
      LOG_ERROR << "Failed to create PipeWire context";
      return;
    }

    pw_core_ = pw_context_connect(pw_context_, nullptr, 0);
    if (!pw_core_) {
      LOG_ERROR << "Failed to connect to PipeWire core";
      return;
    }

    static const struct pw_stream_events stream_events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = [](void* data, uint32_t id, const struct spa_pod* param) {
          auto* self = static_cast<PipeWirePortalCapture*>(data);
          if (id != SPA_PARAM_Format || !param) return;
          struct spa_video_info_raw info{};
          if (spa_format_video_raw_parse(param, &info) >= 0) {
            self->width_ = info.size.width;
            self->height_ = info.size.height;
            self->stride_ = info.size.width * 4;
            LOG_INFO << "PipeWire ScreenCast stream format negotiated: "
                     << self->width_ << "x" << self->height_;
          }
        },
        .process = [](void* data) {
          auto* self = static_cast<PipeWirePortalCapture*>(data);
          if (!self->running_.load() || !self->pw_stream_) return;

          struct pw_buffer* b = pw_stream_dequeue_buffer(self->pw_stream_);
          if (!b || !b->buffer) return;

          struct spa_buffer* sbuf = b->buffer;
          if (sbuf->datas[0].data && self->width_ > 0 && self->height_ > 0) {
            CapturedVideoFrame vf;
            vf.width = self->width_;
            vf.height = self->height_;
            vf.stride = self->stride_ > 0 ? self->stride_ : self->width_ * 4;
            vf.timestamp = std::chrono::steady_clock::now();
            vf.data.resize(static_cast<size_t>(vf.stride) * vf.height);
            std::memcpy(vf.data.data(), sbuf->datas[0].data, vf.data.size());

            FrameCallback cb;
            {
              std::lock_guard<std::mutex> lock(self->mutex_);
              cb = self->callback_;
            }
            if (cb) {
              cb(vf);
            }
          }
          pw_stream_queue_buffer(self->pw_stream_, b);
        },
    };

    pw_stream_ = pw_stream_new(
        pw_core_,
        "CastMirror Portal ScreenCast",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr));

    if (!pw_stream_) {
      LOG_ERROR << "Failed to create PipeWire stream";
      return;
    }

    pw_stream_add_listener(pw_stream_, &stream_listener_, &stream_events, this);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    const struct spa_pod* params[1];
    struct spa_video_info_raw info{};
    info.format = SPA_VIDEO_FORMAT_BGRx;
    info.framerate.num = target_fps > 0 ? target_fps : 60;
    info.framerate.denom = 1;

    params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(
            pw_stream_,
            PW_DIRECTION_INPUT,
            node_id,
            static_cast<enum pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS),
            params,
            1) < 0) {
      LOG_ERROR << "Failed to connect PipeWire stream to node " << node_id;
      return;
    }

    LOG_INFO << "Connected to PipeWire ScreenCast node " << node_id << " @ " << target_fps << "fps";
    pw_main_loop_run(pw_loop_);
  }
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  FrameCallback callback_;

  struct pw_main_loop* pw_loop_ = nullptr;
  struct pw_context* pw_context_ = nullptr;
  struct pw_core* pw_core_ = nullptr;
  struct pw_stream* pw_stream_ = nullptr;
  struct spa_hook stream_listener_{};

  int width_ = 0;
  int height_ = 0;
  int stride_ = 0;
  std::atomic<int> target_fps_{60};
};

std::unique_ptr<IDisplayCapture> CreateWaylandPortalCapture() {
  return std::make_unique<PipeWirePortalCapture>();
}

} // namespace castcore
#endif
