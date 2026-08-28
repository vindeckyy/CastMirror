#include <adwaita.h>
#include "gui_app.h"
#include "css_loader.h"
#include "notify.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"

#include <memory>

#if !defined(_WIN32)
extern "C" int XInitThreads(void);
#endif

namespace {

struct ApplicationContext {
  std::unique_ptr<castcore::gui::GuiApp> gui;
};

void OnStartup(AdwApplication* app, gpointer) {
  AdwStyleManager* style = adw_style_manager_get_default();
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);

  GdkDisplay* display = gdk_display_get_default();
  castcore::gui::ApplyApplicationTheme(display);
  castcore::gui::NotificationManager::Initialize(G_APPLICATION(app));

  auto& engine = castcore::CastEngine::Instance();
  engine.Initialize();
  engine.StartDiscovery();
}

void OnActivate(AdwApplication* app, gpointer user_data) {
  auto* ctx = static_cast<ApplicationContext*>(user_data);
  if (!ctx->gui) {
    ctx->gui = std::make_unique<castcore::gui::GuiApp>(app);
  }
  ctx->gui->Present();
}

void OnShutdown(AdwApplication*, gpointer user_data) {
  auto* ctx = static_cast<ApplicationContext*>(user_data);
  if (ctx->gui) {
    ctx->gui->Shutdown();
  }
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
  XInitThreads();
#endif

  ApplicationContext ctx;
  AdwApplication* app = adw_application_new("io.github.vindeckyy.CastMirror",
                                            G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "startup", G_CALLBACK(OnStartup), &ctx);
  g_signal_connect(app, "activate", G_CALLBACK(OnActivate), &ctx);
  g_signal_connect(app, "shutdown", G_CALLBACK(OnShutdown), &ctx);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  ctx.gui.reset();
  g_object_unref(app);
  return status;
}
