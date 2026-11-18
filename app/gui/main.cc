#include <adwaita.h>
#include "gui_app.h"
#include "css_loader.h"
#include "i18n.h"
#include "notify.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"

#include <memory>

#if defined(CASTMIRROR_HAVE_INTL)
#include <clocale>
#endif

#if !defined(_WIN32)
extern "C" int XInitThreads(void);
#endif

namespace {

struct ApplicationContext {
  std::unique_ptr<castcore::gui::GuiApp> gui;
};

void OnStartup(AdwApplication* app, gpointer) {
  auto& engine = castcore::CastEngine::Instance();
  engine.Initialize();
  engine.StartDiscovery();

  const std::string& theme = castcore::ConfigStore::Instance().Get().ui_theme;
  AdwStyleManager* style = adw_style_manager_get_default();
  if (theme == "light") {
    adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_LIGHT);
  } else if (theme == "dark") {
    adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  } else {
    adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  }

  GdkDisplay* display = gdk_display_get_default();
  castcore::gui::ApplyApplicationTheme(display);
  castcore::gui::NotificationManager::Initialize(G_APPLICATION(app));
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
  // Translation bootstrap.  Must run before any GTK object is constructed so
  // that GTK's own strings (and ours) resolve in the user's locale.  When the
  // build has no libintl (CASTMIRROR_HAVE_INTL undefined) this is a no-op and
  // the GUI stays English-only, exactly as before.
#if defined(CASTMIRROR_HAVE_INTL)
  setlocale(LC_ALL, "");
  bindtextdomain(CASTMIRROR_GETTEXT_DOMAIN, CASTMIRROR_LOCALEDIR);
  bind_textdomain_codeset(CASTMIRROR_GETTEXT_DOMAIN, "UTF-8");
  textdomain(CASTMIRROR_GETTEXT_DOMAIN);
#endif

#if !defined(_WIN32)
  XInitThreads();
#endif

  ApplicationContext ctx;
  AdwApplication* app =
      adw_application_new("io.github.vindeckyy.CastMirror", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "startup", G_CALLBACK(OnStartup), &ctx);
  g_signal_connect(app, "activate", G_CALLBACK(OnActivate), &ctx);
  g_signal_connect(app, "shutdown", G_CALLBACK(OnShutdown), &ctx);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  ctx.gui.reset();
  g_object_unref(app);
  return status;
}
