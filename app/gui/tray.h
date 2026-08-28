#ifndef CASTMIRROR_GUI_TRAY_H_
#define CASTMIRROR_GUI_TRAY_H_

#include <gio/gio.h>
#include <string>
#include "castcore/types.h"

namespace castcore::gui {

class GuiApp;

class TrayManager {
 public:
  explicit TrayManager(GuiApp* app);
  ~TrayManager();

  bool IsAvailable() const;
  bool IsCreated() const { return created_; }
  bool IsConnected() const { return connected_; }
  bool CanHideToTray() const { return created_ && connected_; }

  void CreateIndicator();
  void DestroyIndicator();
  void UpdateState(SessionState state, const std::string& device_name);

 private:
  void OnConnectionChanged(bool connected);

  GuiApp* app_ = nullptr;
  void* indicator_ = nullptr;
  GMenu* menu_ = nullptr;
  GSimpleActionGroup* action_group_ = nullptr;
  GSimpleAction* action_show_ = nullptr;
  GSimpleAction* action_cast_last_ = nullptr;
  GSimpleAction* action_stop_ = nullptr;
  GSimpleAction* action_quit_ = nullptr;
  bool created_ = false;
  bool connected_ = false;
  std::string current_stop_label_;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_TRAY_H_
