#ifndef CASTMIRROR_GUI_TRAY_H_
#define CASTMIRROR_GUI_TRAY_H_

#include <gtk/gtk.h>
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

  void CreateIndicator();
  void DestroyIndicator();
  void UpdateState(SessionState state, const std::string& device_name);

 private:
  GuiApp* app_ = nullptr;
  void* indicator_ = nullptr; // AppIndicator* if compiled
  GtkWidget* menu_ = nullptr;
  GtkWidget* item_show_ = nullptr;
  GtkWidget* item_cast_last_ = nullptr;
  GtkWidget* item_stop_ = nullptr;
  bool created_ = false;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_TRAY_H_
