#include "i3_integration.h"

#if !defined(_WIN32)

#include "castcore/logger.h"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace castcore {
namespace {

constexpr uint32_t kI3RunCommand = 0;
constexpr uint32_t kI3GetTree = 4;
constexpr uint32_t kMaxI3ReplyBytes = 16 * 1024 * 1024;

std::string GetI3SocketPath(Display* display) {
  const char* env_socket = std::getenv("I3SOCK");
  if (env_socket && env_socket[0] != '\0' && access(env_socket, F_OK) == 0) {
    return env_socket;
  }
  if (!display) return {};

  Atom socket_atom = XInternAtom(display, "I3_SOCKET_PATH", True);
  if (socket_atom == None) return {};

  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0;
  unsigned long bytes_after = 0;
  unsigned char* data = nullptr;
  const int status = XGetWindowProperty(
      display, DefaultRootWindow(display), socket_atom, 0, 1024, False,
      AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after,
      &data);

  std::string path;
  if (status == Success && data && actual_format == 8 && nitems > 0) {
    path.assign(reinterpret_cast<char*>(data), nitems);
  }
  if (data) XFree(data);
  return path;
}

bool WriteAll(int fd, const void* buffer, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  size_t sent = 0;
  while (sent < size) {
    const ssize_t count = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
    if (count <= 0) return false;
    sent += static_cast<size_t>(count);
  }
  return true;
}

bool ReadAll(int fd, void* buffer, size_t size) {
  auto* bytes = static_cast<uint8_t*>(buffer);
  size_t received = 0;
  while (received < size) {
    const ssize_t count = recv(fd, bytes + received, size - received, 0);
    if (count <= 0) return false;
    received += static_cast<size_t>(count);
  }
  return true;
}

bool SendI3Message(Display* display, uint32_t message_type,
                   const std::string& payload, std::string* reply) {
  const std::string socket_path = GetI3SocketPath(display);
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
      payload.size() > UINT32_MAX) {
    return false;
  }

  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  timeval timeout{};
  timeout.tv_sec = 1;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return false;
  }

  // i3 IPC header: six-byte magic, native-endian uint32 payload length, and
  // native-endian uint32 message type. Keep it byte-packed explicitly.
  uint8_t header[14]{};
  std::memcpy(header, "i3-ipc", 6);
  const uint32_t payload_length = static_cast<uint32_t>(payload.size());
  std::memcpy(header + 6, &payload_length, sizeof(payload_length));
  std::memcpy(header + 10, &message_type, sizeof(message_type));

  bool ok = WriteAll(fd, header, sizeof(header)) &&
            WriteAll(fd, payload.data(), payload.size());

  uint8_t response_header[14]{};
  if (ok) ok = ReadAll(fd, response_header, sizeof(response_header));

  uint32_t response_length = 0;
  uint32_t response_type = 0;
  if (ok && std::memcmp(response_header, "i3-ipc", 6) == 0) {
    std::memcpy(&response_length, response_header + 6, sizeof(response_length));
    std::memcpy(&response_type, response_header + 10, sizeof(response_type));
    ok = response_length <= kMaxI3ReplyBytes && response_type == message_type;
  } else {
    ok = false;
  }

  std::string response;
  if (ok) {
    response.resize(response_length);
    ok = ReadAll(fd, response.data(), response.size());
  }
  close(fd);

  if (ok && reply) *reply = std::move(response);
  return ok;
}

bool RunI3Command(Display* display, const std::string& command) {
  std::string reply;
  if (!SendI3Message(display, kI3RunCommand, command, &reply)) return false;
  try {
    const auto result = nlohmann::json::parse(reply);
    return result.is_array() && !result.empty() &&
           std::all_of(result.begin(), result.end(), [](const auto& entry) {
             return entry.value("success", false);
           });
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

std::string Criteria(Window window) {
  return "[id=\"" + std::to_string(static_cast<unsigned long>(window)) + "\"] ";
}

std::string EscapeI3String(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    if (ch == '\\' || ch == '"') escaped.push_back('\\');
    escaped.push_back(ch);
  }
  return escaped;
}

struct I3WindowLayout {
  bool found = false;
  bool floating = false;
  bool sticky = false;
  bool scratchpad = false;
  int fullscreen_mode = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  uint64_t focused_container_id = 0;
  std::string focused_workspace;
  std::string workspace;
};

bool FindFocusedContainer(const nlohmann::json& node,
                          const std::string& workspace,
                          uint64_t* container_id,
                          std::string* focused_workspace) {
  std::string node_workspace = workspace;
  if (node.value("type", std::string()) == "workspace") {
    node_workspace = node.value("name", std::string());
  }
  if (node.value("focused", false)) {
    const auto id_it = node.find("id");
    if (id_it != node.end() && id_it->is_number_integer()) {
      *container_id = id_it->get<uint64_t>();
      *focused_workspace = node_workspace;
      return true;
    }
  }
  for (const char* child_key : {"nodes", "floating_nodes"}) {
    const auto children_it = node.find(child_key);
    if (children_it == node.end() || !children_it->is_array()) continue;
    for (const auto& child : *children_it) {
      if (FindFocusedContainer(child, node_workspace, container_id,
                               focused_workspace)) {
        return true;
      }
    }
  }
  return false;
}

bool FindWindowLayout(const nlohmann::json& node, uint64_t target,
                      const std::string& workspace, I3WindowLayout* result) {
  std::string node_workspace = workspace;
  if (node.value("type", std::string()) == "workspace") {
    node_workspace = node.value("name", std::string());
  }

  const auto window_it = node.find("window");
  if (window_it != node.end() && window_it->is_number_integer() &&
      window_it->get<uint64_t>() == target) {
    result->found = true;
    result->workspace = node_workspace;
    const std::string floating = node.value("floating", std::string("auto_off"));
    result->floating = floating == "auto_on" || floating == "user_on";
    result->sticky = node.value("sticky", false);
    result->fullscreen_mode = node.value("fullscreen_mode", 0);
    result->scratchpad = node_workspace == "__i3_scratch" ||
                         node.value("scratchpad_state", std::string("none")) != "none";
    if (const auto rect_it = node.find("rect"); rect_it != node.end() && rect_it->is_object()) {
      result->x = rect_it->value("x", 0);
      result->y = rect_it->value("y", 0);
      result->width = rect_it->value("width", 0);
      result->height = rect_it->value("height", 0);
    }
    return true;
  }

  for (const char* child_key : {"nodes", "floating_nodes"}) {
    const auto children_it = node.find(child_key);
    if (children_it == node.end() || !children_it->is_array()) continue;
    for (const auto& child : *children_it) {
      if (FindWindowLayout(child, target, node_workspace, result)) return true;
    }
  }
  return false;
}

bool ReadWindowLayout(Display* display, Window window, I3WindowLayout* layout) {
  std::string reply;
  if (!SendI3Message(display, kI3GetTree, {}, &reply)) return false;
  try {
    const auto tree = nlohmann::json::parse(reply);
    FindFocusedContainer(tree, {}, &layout->focused_container_id,
                         &layout->focused_workspace);
    return FindWindowLayout(tree, static_cast<uint64_t>(window), {}, layout);
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

bool WaitUntilViewable(Display* display, Window window) {
  for (int attempt = 0; attempt < 60; ++attempt) {
    XSync(display, False);
    XWindowAttributes attrs{};
    if (XGetWindowAttributes(display, window, &attrs) &&
        attrs.map_state == IsViewable) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

struct OpacityState {
  Window window = None;
  bool valid = false;
  bool had_property = false;
  unsigned long value = 0;
};

OpacityState HideWindowWithOpacity(Display* display, Window window) {
  OpacityState state;
  state.window = window;
  if (!display || window == None) return state;

  const Atom opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char* data = nullptr;
  if (XGetWindowProperty(display, window, opacity, 0, 1, False, XA_CARDINAL,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) == Success) {
    state.valid = true;
    if (data && actual_format == 32 && nitems == 1) {
      state.had_property = true;
      state.value = *reinterpret_cast<unsigned long*>(data);
    }
    if (data) XFree(data);
  }

  const unsigned long transparent = 0;
  XChangeProperty(display, window, opacity, XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&transparent), 1);
  return state;
}

void RestoreOpacity(Display* display, const OpacityState& state) {
  if (!display || !state.valid || state.window == None) return;
  const Atom opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);
  if (state.had_property) {
    XChangeProperty(display, state.window, opacity, XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&state.value), 1);
  } else {
    XDeleteProperty(display, state.window, opacity);
  }
}

struct InputShapeState {
  Window window = None;
  bool valid = false;
  bool was_shaped = false;
  int ordering = Unsorted;
  std::vector<XRectangle> rectangles;
};

InputShapeState DisableWindowInput(Display* display, Window window) {
  InputShapeState state;
  state.window = window;
  if (!display || window == None) return state;

  int shape_event_base = 0;
  int shape_error_base = 0;
  if (!XShapeQueryExtension(display, &shape_event_base, &shape_error_base)) {
    return state;
  }

  Bool bounding_shaped = False;
  Bool input_shaped = False;
  int bounding_x = 0, bounding_y = 0;
  unsigned int bounding_width = 0, bounding_height = 0;
  int input_x = 0, input_y = 0;
  unsigned int input_width = 0, input_height = 0;
  if (!XShapeQueryExtents(display, window, &bounding_shaped, &bounding_x,
                          &bounding_y, &bounding_width, &bounding_height,
                          &input_shaped, &input_x, &input_y, &input_width,
                          &input_height)) {
    return state;
  }

  state.valid = true;
  state.was_shaped = input_shaped != False;
  if (state.was_shaped) {
    int count = 0;
    XRectangle* rectangles = XShapeGetRectangles(
        display, window, ShapeInput, &count, &state.ordering);
    if (rectangles && count > 0) {
      state.rectangles.assign(rectangles, rectangles + count);
    }
    if (rectangles) XFree(rectangles);
  }

  XShapeCombineRectangles(display, window, ShapeInput, 0, 0, nullptr, 0,
                          ShapeSet, Unsorted);
  return state;
}

void RestoreInputShape(Display* display, const InputShapeState& state) {
  if (!display || !state.valid || state.window == None) return;
  if (!state.was_shaped) {
    XShapeCombineMask(display, state.window, ShapeInput, 0, 0, None, ShapeSet);
    return;
  }
  XShapeCombineRectangles(
      display, state.window, ShapeInput, 0, 0,
      state.rectangles.empty() ? nullptr
                               : const_cast<XRectangle*>(state.rectangles.data()),
      static_cast<int>(state.rectangles.size()), ShapeSet, state.ordering);
}

Window GetFrameWindow(Display* display, Window client) {
  Window root = None;
  Window parent = None;
  Window* children = nullptr;
  unsigned int child_count = 0;
  if (!XQueryTree(display, client, &root, &parent, &children, &child_count)) {
    return None;
  }
  if (children) XFree(children);
  return parent != root ? parent : None;
}

}  // namespace

struct I3CapturePin::Impl {
  bool pinned = false;
  Window window = None;
  I3WindowLayout layout;
  std::vector<OpacityState> opacity_states;
  std::vector<InputShapeState> input_shape_states;
};

I3CapturePin::I3CapturePin() : impl_(std::make_unique<Impl>()) {}
I3CapturePin::~I3CapturePin() = default;

bool I3CapturePin::IsAvailable(Display* display) {
  return !GetI3SocketPath(display).empty();
}

bool I3CapturePin::Pin(Display* display, Window window) {
  if (!display || window == None || impl_->pinned || !IsAvailable(display)) {
    return false;
  }

  I3WindowLayout layout;
  if (!ReadWindowLayout(display, window, &layout) || !layout.found) {
    LOG_WARN << "i3 IPC could not locate selected X11 window "
             << static_cast<unsigned long>(window);
    return false;
  }

  impl_->window = window;
  impl_->layout = layout;

  std::string command = Criteria(window);
  if (layout.scratchpad) command += "scratchpad show, ";
  command += "focus, fullscreen disable, floating enable";
  if (layout.width > 0 && layout.height > 0) {
    command += ", resize set " + std::to_string(layout.width) + " px " +
               std::to_string(layout.height) + " px";
  }
  command += ", sticky enable";

  // From here on, always attempt restoration on failure: i3 can apply the
  // earlier commands in a comma chain even if a later command is rejected.
  impl_->pinned = true;
  if (!RunI3Command(display, command) || !WaitUntilViewable(display, window)) {
    LOG_WARN << "i3 could not pin selected window for background capture";
    Restore(display);
    return false;
  }

  XSync(display, False);
  const Window frame = GetFrameWindow(display, window);

  impl_->opacity_states.push_back(HideWindowWithOpacity(display, window));
  impl_->input_shape_states.push_back(DisableWindowInput(display, window));
  if (frame != None && frame != window) {
    impl_->opacity_states.push_back(HideWindowWithOpacity(display, frame));
    impl_->input_shape_states.push_back(DisableWindowInput(display, frame));
  }

  XSync(display, False);
  if (!layout.focused_workspace.empty()) {
    RunI3Command(display, "workspace --no-auto-back-and-forth \"" +
                              EscapeI3String(layout.focused_workspace) + "\"");
  }
  if (layout.focused_container_id != 0) {
    RunI3Command(display, "[con_id=\"" +
                              std::to_string(layout.focused_container_id) +
                              "\"] focus");
  }
  LOG_INFO << "Pinned i3 window for transparent background capture; original workspace='"
           << layout.workspace << "' floating=" << layout.floating
           << " sticky=" << layout.sticky;
  return true;
}

void I3CapturePin::Restore(Display* display) {
  if (!impl_->pinned || !display) return;

  for (auto it = impl_->input_shape_states.rbegin();
       it != impl_->input_shape_states.rend(); ++it) {
    RestoreInputShape(display, *it);
  }
  for (auto it = impl_->opacity_states.rbegin();
       it != impl_->opacity_states.rend(); ++it) {
    RestoreOpacity(display, *it);
  }
  XSync(display, False);

  const Window window = impl_->window;
  const I3WindowLayout layout = impl_->layout;
  std::string command = Criteria(window) + "sticky disable";

  if (layout.scratchpad) {
    command += ", move scratchpad";
  } else if (!layout.workspace.empty()) {
    command += ", move container to workspace \"" +
               EscapeI3String(layout.workspace) + "\"";
  }

  if (layout.floating) {
    command += ", floating enable";
    if (layout.width > 0 && layout.height > 0) {
      command += ", resize set " + std::to_string(layout.width) + " px " +
                 std::to_string(layout.height) + " px";
      command += ", move absolute position " + std::to_string(layout.x) +
                 " px " + std::to_string(layout.y) + " px";
    }
  } else {
    command += ", floating disable";
  }

  command += layout.sticky ? ", sticky enable" : ", sticky disable";
  if (layout.fullscreen_mode != 0) command += ", fullscreen enable";

  if (!RunI3Command(display, command)) {
    LOG_WARN << "Could not fully restore i3 layout for captured window";
  } else {
    LOG_INFO << "Restored captured window to i3 workspace '" << layout.workspace << "'";
  }

  impl_->pinned = false;
  impl_->window = None;
  impl_->layout = {};
  impl_->opacity_states.clear();
  impl_->input_shape_states.clear();
}

bool I3CapturePin::IsPinned() const {
  return impl_->pinned;
}

}  // namespace castcore

#endif  // !defined(_WIN32)
