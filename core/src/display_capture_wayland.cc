#include "castcore/display_capture.h"
#include "castcore/config.h"
#include "castcore/logger.h"
#include "castcore/latency_hud.h"

#if defined(CASTCORE_HAVE_PIPEWIRE)

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <gio/gio.h>
#if defined(__has_include)
#if __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#elif __has_include(<drm/drm_fourcc.h>)
#include <drm/drm_fourcc.h>
#elif __has_include(<drm_fourcc.h>)
#include <drm_fourcc.h>
#endif
#endif

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#include <unistd.h>
#include <sys/mman.h>

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

// xdg-desktop-portal ScreenCast source type bitmask constants.
//   1 = MONITOR, 2 = WINDOW, 4 = VIRTUAL.
constexpr uint32_t kPortalSourceMonitor = 1;
constexpr uint32_t kPortalSourceWindow  = 2;

// Pure helper: map a CaptureSourceKind to the portal SelectSources "types" value.
uint32_t PortalSourceTypesFor(CaptureSourceKind kind) {
  return kind == CaptureSourceKind::kWindow ? kPortalSourceWindow : kPortalSourceMonitor;
}

// Pure helper: map a portal stream "source_type" u back to CaptureSourceKind.
CaptureSourceKind CaptureSourceKindFromPortalSourceType(uint32_t source_type) {
  return source_type == kPortalSourceWindow ? CaptureSourceKind::kWindow : CaptureSourceKind::kMonitor;
}

class PipeWirePortalCapture : public IDisplayCapture {
 public:
  PipeWirePortalCapture() = default;
  std::string BackendName() const override { return "PipeWire"; }
  ~PipeWirePortalCapture() override { Stop(); }

  bool Start(int display_id, int target_fps) override {
    return Start(CaptureSource{CaptureSourceKind::kMonitor, display_id, "Wayland"}, target_fps);
  }

  bool Start(const CaptureSource& source, int target_fps) override {
    Stop();
    requested_source_ = source;
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

  // The xdg-desktop-portal ScreenCast interface advertises window capture
  // when the compositor supports it. We probe once and cache the result.
  bool SupportsWindowCapture() const override {
    if (window_support_probed_) return window_support_cached_;
    window_support_probed_ = true;
    window_support_cached_ = ProbePortalWindowSupport();
    return window_support_cached_;
  }

  // The portal does not expose a window list; the compositor's native picker
  // does the choosing. We return a single sentinel entry so the UI can show
  // "Pick in system dialog" instead of an empty list.
  std::vector<WindowInfo> EnumerateWindows() override {
    std::vector<WindowInfo> list;
    if (SupportsWindowCapture()) {
      WindowInfo w;
      w.id = 0;
      w.title = "Pick a window in the system dialog";
      w.app_class = "Wayland";
      w.visible = true;
      list.push_back(w);
    }
    return list;
  }

  CaptureSource ActiveSource() const override {
    return resolved_source_.has_value() ? *resolved_source_ : requested_source_;
  }

 private:
  // Probe whether the running portal/compositor advertises window source
  // type via org.freedesktop.portal.ScreenCast.GetAvailableSourceTypes.
  // Returns false if the call is unavailable (older portal) — in that case
  // we conservatively report no window support and the UI hides the toggle.
  static bool ProbePortalWindowSupport() {
    GError* err = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn) {
      if (err) g_error_free(err);
      return false;
    }
    GVariant* reply = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "GetAvailableSourceTypes",
        nullptr,
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        nullptr,
        &err);
    g_object_unref(conn);
    if (!reply) {
      if (err) g_error_free(err);
      return false;
    }
    uint32_t types = 0;
    g_variant_get(reply, "(u)", &types);
    g_variant_unref(reply);
    return (types & kPortalSourceWindow) != 0;
  }

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
    // types: 1 = Monitor, 2 = Window. The portal shows the compositor's
    // native picker for the requested kind; for Window the user chooses the
    // window in the system dialog (the requested_source_.id is ignored).
    const uint32_t source_types = PortalSourceTypesFor(requested_source_.kind);
    GVariantBuilder select_builder;
    g_variant_builder_init(&select_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&select_builder, "{sv}", "types", g_variant_new_uint32(source_types));
    g_variant_builder_add(&select_builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&select_builder, "{sv}", "persist_mode", g_variant_new_uint32(2)); // 2 = Persist until revoked

    const auto& cfg = ConfigStore::Instance().Get();
    // Only pass a restore_token when the requested kind matches the kind we
    // previously persisted a token for. A monitor token won't restore a
    // window source and vice-versa; mismatched tokens make the portal error.
    if (!cfg.portal_restore_token.empty() &&
        CaptureSourceKindFromString(cfg.last_source_kind) == requested_source_.kind) {
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
      // Resolve the actual source from stream props. The portal reports
      // source_type (u), size (ii), position (ii), and optionally title (s).
      if (stream_props) {
        CaptureSource resolved = requested_source_;
        uint32_t source_type = 0;
        if (g_variant_lookup(stream_props, "source_type", "u", &source_type)) {
          resolved.kind = CaptureSourceKindFromPortalSourceType(source_type);
        }
        gint32 sw = 0, sh = 0;
        if (g_variant_lookup(stream_props, "size", "(ii)", &sw, &sh)) {
          resolved.width = sw;
          resolved.height = sh;
        }
        gint32 px = 0, py = 0;
        if (g_variant_lookup(stream_props, "position", "(ii)", &px, &py)) {
          resolved.x = px;
          resolved.y = py;
        }
        const char* title = nullptr;
        if (g_variant_lookup(stream_props, "title", "&s", &title) && title && title[0]) {
          resolved.name = title;
        } else if (resolved.name.empty()) {
          resolved.name = resolved.IsWindow() ? "Wayland window" : "Wayland screen";
        }
        resolved.id = static_cast<int>(node_id);
        resolved_source_ = resolved;
        g_variant_unref(stream_props);
      }
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

    static const struct pw_stream_events stream_events = []() {
      struct pw_stream_events ev{};
      ev.version = PW_VERSION_STREAM_EVENTS;
      ev.param_changed = [](void* data, uint32_t id, const struct spa_pod* param) {
          auto* self = static_cast<PipeWirePortalCapture*>(data);
          if (id != SPA_PARAM_Format || !param) return;
          struct spa_video_info_raw info{};
          if (spa_format_video_raw_parse(param, &info) >= 0) {
            self->width_ = info.size.width;
            self->height_ = info.size.height;
            // Phase 1.2: negotiate DMA-BUF+NV12 for zero-copy; keep BGRx fallback.
            // When info.format is NV12 we will use DmaBuf path and avoid BGRA shadow copy.
            if (info.format == SPA_VIDEO_FORMAT_NV12) {
              self->negotiated_format_ = SPA_VIDEO_FORMAT_NV12;
              // NV12 has 1.5 bytes per pixel: Y stride + UV stride (same)
              self->stride_ = info.size.width; // Y stride, UV same
              self->is_dmabuf_negotiated_ = true;
              LOG_INFO << "PipeWire format negotiated: NV12 " << self->width_ << "x" << self->height_ << " (DMA-BUF zero-copy)";
            } else {
              self->negotiated_format_ = info.format;
              self->stride_ = info.size.width * 4;
              self->is_dmabuf_negotiated_ = false;
              LOG_INFO << "PipeWire format negotiated: BGRx " << self->width_ << "x" << self->height_ << " (SW fallback)";
            }
          }
        };
      ev.process = [](void* data) {
          auto* self = static_cast<PipeWirePortalCapture*>(data);
          if (!self->running_.load() || !self->pw_stream_) return;

          struct pw_buffer* b = pw_stream_dequeue_buffer(self->pw_stream_);
          if (!b || !b->buffer) return;

          struct spa_buffer* sbuf = b->buffer;
          if (self->width_ <= 0 || self->height_ <= 0) {
            pw_stream_queue_buffer(self->pw_stream_, b);
            return;
          }

          // Extract cursor plane if present (SPA_META_Cursor)
          bool has_cursor = false;
          int cursor_x = 0, cursor_y = 0, cursor_hotspot_x = 0, cursor_hotspot_y = 0;
          std::vector<uint8_t> cursor_bg;
          int cursor_w = 0, cursor_h = 0, cursor_stride = 0;
          if (sbuf->n_metas > 0) {
            struct spa_meta* m = spa_buffer_find_meta(sbuf, SPA_META_Cursor);
            if (m && m->size >= sizeof(struct spa_meta_cursor)) {
              auto* cur = static_cast<struct spa_meta_cursor*>(m->data);
              if (spa_meta_cursor_is_valid(cur)) {
                has_cursor = true;
                cursor_x = cur->position.x;
                cursor_y = cur->position.y;
                cursor_hotspot_x = cur->hotspot.x;
                cursor_hotspot_y = cur->hotspot.y;
                if (cur->bitmap_offset >= sizeof(struct spa_meta_cursor) && m->size >= cur->bitmap_offset + sizeof(struct spa_meta_bitmap)) {
                  auto* bm = reinterpret_cast<struct spa_meta_bitmap*>(reinterpret_cast<uint8_t*>(cur) + cur->bitmap_offset);
                  if (spa_meta_bitmap_is_valid(bm) && bm->offset >= sizeof(struct spa_meta_bitmap) && bm->size.width > 0 && bm->size.height > 0) {
                    cursor_w = bm->size.width;
                    cursor_h = bm->size.height;
                    cursor_stride = bm->stride;
                    uint8_t* bmp_data = reinterpret_cast<uint8_t*>(bm) + bm->offset;
                    size_t bmp_size = static_cast<size_t>(cursor_stride) * cursor_h;
                    // Validate size fits within meta
                    if (m->size >= cur->bitmap_offset + bm->offset + bmp_size) {
                      cursor_bg.assign(bmp_data, bmp_data + bmp_size);
                    }
                  }
                }
              }
            }
          }

          CapturedVideoFrame vf;
          vf.width = self->width_;
          vf.height = self->height_;
          vf.timestamp = std::chrono::steady_clock::now();
          vf.has_cursor = has_cursor;
          vf.cursor_x = cursor_x;
          vf.cursor_y = cursor_y;
          vf.cursor_hotspot_x = cursor_hotspot_x;
          vf.cursor_hotspot_y = cursor_hotspot_y;
          vf.cursor_data = std::move(cursor_bg);
          vf.cursor_width = cursor_w;
          vf.cursor_height = cursor_h;
          vf.cursor_stride = cursor_stride;

          bool handled = false;
          // Phase 1.2: DMA-BUF NV12 zero-copy path – avoid BGRA shadow copy entirely.
          // When portal negotiated DMA-BUF+NV12 and the buffer carries DmaBuf fds,
          // import the wl_buffer fd into AV_HWDEVICE_TYPE_VAAPI hw_frames_ctx_ via
          // the video_encoder's DRM PRIME mapping (0 extra GPU copies).
          if (self->negotiated_format_ == SPA_VIDEO_FORMAT_NV12 && sbuf->n_datas > 0) {
            bool is_dmabuf = false;
            int fd = -1;
            int stride = 0;
            int offset_y = 0;
            int offset_uv = 0;
            uint64_t modifier = DRM_FORMAT_MOD_INVALID;
            // PipeWire DmaBuf: each plane is a spa_data with type SPA_DATA_DmaBuf
            // For NV12 single-file case, datas[0] holds the fd, chunk stride covers Y and UV.
            // For multi-fd case, datas[0] Y, datas[1] UV with separate fds (we use first fd and offsets).
            if (sbuf->datas[0].type == SPA_DATA_DmaBuf) {
              is_dmabuf = true;
              fd = sbuf->datas[0].fd;
              if (sbuf->datas[0].chunk) {
                stride = sbuf->datas[0].chunk->stride;
                offset_y = sbuf->datas[0].chunk->offset;
              }
              if (sbuf->n_datas > 1 && sbuf->datas[1].type == SPA_DATA_DmaBuf) {
                // Multi-fd NV12: UV plane is separate fd – we keep single fd model
                // by using first fd and computing UV offset as height * stride.
                // If second fd differs, we cannot represent with single fd, fallback to MemPtr path.
                if (sbuf->datas[1].fd != fd) {
                  // Fallback: treat as SW copy (mmap both)
                  is_dmabuf = false;
                } else {
                  if (sbuf->datas[1].chunk) {
                    offset_uv = sbuf->datas[1].chunk->offset;
                    // stride should be same; use first
                  }
                }
                // For modifier, assume linear if not negotiated; compositor may not provide modifier
              } else {
                // Single fd, UV offset is Y_size
                if (stride > 0) {
                  offset_uv = stride * self->height_;
                  if (sbuf->datas[0].chunk) offset_uv += offset_y;
                }
              }
              // Modifier: if info provided modifier, use it; else linear
              // SPA not exposing modifier directly – keep INVALID sentinel
              if (is_dmabuf && fd >= 0 && stride > 0) {
                vf.is_dmabuf = true;
                vf.dmabuf_fd = fd; // note: PipeWire owns fd lifetime until queue, video_encoder will dup()
                vf.dmabuf_stride = stride > 0 ? stride : self->width_;
                vf.dmabuf_offset_y = offset_y;
                vf.dmabuf_offset_uv = offset_uv;
                vf.dmabuf_modifier = modifier;
                vf.dmabuf_format = DRM_FORMAT_NV12;
                vf.stride = stride;
                // No BGRA shadow copy – directly queue hw frame
                handled = true;
                FrameCallback cb;
                {
                  std::lock_guard<std::mutex> lock(self->mutex_);
                  cb = self->callback_;
                }
                if (cb) cb(vf);
              }
            }
          }

          if (!handled) {
            // SW fallback: MemPtr/MemFd/DmaBuf mmap BGRA shadow copy (existing path) – keep for compatibility
            // Also used when DmaBuf negotiation failed or compositor fell back to shm.
            uint8_t* src_data = nullptr;
            size_t src_size = 0;
            int src_stride = self->stride_ > 0 ? self->stride_ : self->width_ * 4;
            if (sbuf->datas[0].type == SPA_DATA_MemPtr && sbuf->datas[0].data) {
              src_data = static_cast<uint8_t*>(sbuf->datas[0].data);
              src_size = sbuf->datas[0].chunk ? sbuf->datas[0].chunk->size : static_cast<size_t>(src_stride) * self->height_;
              if (sbuf->datas[0].chunk) src_stride = sbuf->datas[0].chunk->stride;
            } else if (sbuf->datas[0].type == SPA_DATA_MemFd && sbuf->datas[0].fd >= 0 && sbuf->datas[0].chunk) {
              // MemFd: mmap the fd (SW fallback for shm)
              int fd = sbuf->datas[0].fd;
              size_t map_size = sbuf->datas[0].maxsize ? sbuf->datas[0].maxsize : sbuf->datas[0].chunk->size;
              src_stride = sbuf->datas[0].chunk->stride;
              void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
              if (mapped != MAP_FAILED) {
                src_data = static_cast<uint8_t*>(mapped) + sbuf->datas[0].chunk->offset;
                src_size = sbuf->datas[0].chunk->size;
                vf.data.resize(src_size);
                std::memcpy(vf.data.data(), src_data, src_size);
                vf.stride = src_stride;
                vf.width = self->width_;
                vf.height = self->height_;
                munmap(mapped, map_size);
                // Also handle cursor fallback already in vf
                if (ConfigStore::Instance().Get().latency_hud_enabled && !vf.data.empty()) {
                  LatencyHud::Render(vf);
                }
                FrameCallback cb;
                {
                  std::lock_guard<std::mutex> lock(self->mutex_);
                  cb = self->callback_;
                }
                if (cb) cb(vf);
                handled = true;
                pw_stream_queue_buffer(self->pw_stream_, b);
                return;
              }
            } else if (sbuf->datas[0].type == SPA_DATA_DmaBuf && sbuf->datas[0].fd >= 0 && sbuf->datas[0].chunk) {
              // DmaBuf mmap fallback if zero-copy import cannot be used
              int fd = sbuf->datas[0].fd;
              size_t map_size = sbuf->datas[0].maxsize ? sbuf->datas[0].maxsize : sbuf->datas[0].chunk->size;
              if (map_size == 0) map_size = static_cast<size_t>(src_stride) * self->height_;
              void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
              if (mapped != MAP_FAILED) {
                src_data = static_cast<uint8_t*>(mapped) + sbuf->datas[0].chunk->offset;
                src_size = sbuf->datas[0].chunk->size ? sbuf->datas[0].chunk->size : map_size;
                src_stride = sbuf->datas[0].chunk->stride ? sbuf->datas[0].chunk->stride : src_stride;
                vf.data.resize(src_size);
                std::memcpy(vf.data.data(), src_data, src_size);
                vf.stride = src_stride;
                vf.width = self->width_;
                vf.height = self->height_;
                munmap(mapped, map_size);
                if (ConfigStore::Instance().Get().latency_hud_enabled && !vf.data.empty()) {
                  LatencyHud::Render(vf);
                }
                FrameCallback cb;
                {
                  std::lock_guard<std::mutex> lock(self->mutex_);
                  cb = self->callback_;
                }
                if (cb) cb(vf);
                handled = true;
                pw_stream_queue_buffer(self->pw_stream_, b);
                return;
              }
            }
            if (!handled && src_data) {
              // Determine correct copy size
              size_t copy_size = static_cast<size_t>(src_stride) * self->height_;
              if (src_size > 0 && src_size < copy_size) copy_size = src_size;
              vf.data.resize(copy_size);
              std::memcpy(vf.data.data(), src_data, copy_size);
              vf.stride = src_stride;
              vf.width = self->width_;
              vf.height = self->height_;
              if (ConfigStore::Instance().Get().latency_hud_enabled && !vf.data.empty()) {
                LatencyHud::Render(vf);
              }
              FrameCallback cb;
              {
                std::lock_guard<std::mutex> lock(self->mutex_);
                cb = self->callback_;
              }
              if (cb) cb(vf);
            } else if (!handled) {
              // No valid data, skip
            }
          }

          pw_stream_queue_buffer(self->pw_stream_, b);
        };
      return ev;
    }();

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

    const struct spa_pod* params[2];
    // Phase 1.2: prefer DMA-BUF NV12 for zero-copy (0 extra GPU copies), fallback to BGRx shm
    struct spa_video_info_raw info_nv12{};
    info_nv12.format = SPA_VIDEO_FORMAT_NV12;
    info_nv12.framerate.num = target_fps > 0 ? target_fps : 60;
    info_nv12.framerate.denom = 1;
    info_nv12.size.width = 0; // let compositor choose
    info_nv12.size.height = 0;

    struct spa_video_info_raw info_bgrx{};
    info_bgrx.format = SPA_VIDEO_FORMAT_BGRx;
    info_bgrx.framerate.num = target_fps > 0 ? target_fps : 60;
    info_bgrx.framerate.denom = 1;

    params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info_nv12);
    params[1] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info_bgrx);

    if (pw_stream_connect(
            pw_stream_,
            PW_DIRECTION_INPUT,
            node_id,
            static_cast<enum pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT |
                PW_STREAM_FLAG_MAP_BUFFERS |
                PW_STREAM_FLAG_RT_PROCESS),
            params,
            2) < 0) {
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
  enum spa_video_format negotiated_format_ = SPA_VIDEO_FORMAT_UNKNOWN;
  bool is_dmabuf_negotiated_ = false;
  std::atomic<int> target_fps_{60};

  // Source tracking: requested_source_ is what the caller asked for;
  // resolved_source_ is what the portal actually gave us (filled from the
  // Start response stream props). ActiveSource() returns resolved when set.
  CaptureSource requested_source_{CaptureSourceKind::kMonitor, 0, "Wayland"};
  std::optional<CaptureSource> resolved_source_;
  // Cached window-support probe (mutable so the const override can lazily probe).
  mutable bool window_support_probed_ = false;
  mutable bool window_support_cached_ = false;
};

std::unique_ptr<IDisplayCapture> CreateWaylandPortalCapture() {
  return std::make_unique<PipeWirePortalCapture>();
}

} // namespace castcore
#endif
