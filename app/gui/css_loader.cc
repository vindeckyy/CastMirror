#include "css_loader.h"
#include <gtk/gtk.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace castcore::gui {

namespace {

const char* kEmbeddedFallbackCss = R"(
window, .window-bg {
  background-color: #0e1116;
  color: #e8ecf1;
  font-family: Cantarell, "Ubuntu", "Noto Sans", sans-serif;
}
.header-box { background-color: #171b22; border-bottom: 1px solid #2c3440; padding: 14px 20px; }
.title-label { font-size: 20px; font-weight: 700; color: #ffffff; }
.subtitle-label { font-size: 12px; color: #8b93a1; }
.status-badge { padding: 4px 12px; border-radius: 999px; font-weight: 600; font-size: 12px; }
.status-idle { background-color: #1e242e; color: #8b93a1; }
.status-connecting { background-color: #3b2c12; color: #f5c542; }
.status-live { background-color: #103322; color: #3dd68c; }
.status-failed { background-color: #3b1414; color: #ff6b6b; }
notebook header { background-color: #171b22; border-bottom: 1px solid #2c3440; }
notebook tab { padding: 10px 18px; font-weight: 600; font-size: 13px; color: #8b93a1; }
notebook tab:checked { color: #4aa3f0; border-bottom: 2px solid #4aa3f0; }
.footer-box { background-color: #171b22; border-top: 1px solid #2c3440; padding: 14px 20px; }
button.btn-cast-start { background-image: none; background-color: #4aa3f0; color: #0e1116; font-weight: 700; border-radius: 10px; min-height: 46px; }
button.btn-cast-stop { background-image: none; background-color: #c23b3b; color: #ffffff; font-weight: 700; border-radius: 10px; min-height: 46px; }
.card-box { background-color: #171b22; border: 1px solid #2c3440; border-radius: 12px; padding: 16px; margin: 6px 0; }
.card-title { font-size: 11px; font-weight: 700; color: #4aa3f0; letter-spacing: 0.08em; }
.card-desc { font-size: 12px; color: #8b93a1; }
.stat-card { background-color: #171b22; border: 1px solid #2c3440; border-radius: 10px; padding: 12px 14px; }
.stat-title { font-size: 11px; font-weight: 600; color: #8b93a1; }
.stat-value { font-size: 18px; font-weight: 700; color: #4aa3f0; }
)";

bool TryLoadCssFile(GtkCssProvider* provider, const std::string& path) {
  if (path.empty()) return false;
  if (!std::filesystem::exists(path)) return false;
  GError* error = nullptr;
  gtk_css_provider_load_from_path(provider, path.c_str(), &error);
  if (error) {
    g_error_free(error);
    return false;
  }
  return true;
}

}  // namespace

void ApplyApplicationTheme() {
  GtkCssProvider* provider = gtk_css_provider_new();
  bool loaded = false;

  const char* env_css = std::getenv("CASTMIRROR_CSS");
  if (env_css && env_css[0] != '\0') {
    loaded = TryLoadCssFile(provider, env_css);
  }

#if defined(CASTMIRROR_SRC_CSS)
  if (!loaded) {
    loaded = TryLoadCssFile(provider, CASTMIRROR_SRC_CSS);
  }
#endif

#if defined(CASTMIRROR_DATADIR)
  if (!loaded) {
    std::string datadir_css = std::string(CASTMIRROR_DATADIR) + "/castmirror.css";
    loaded = TryLoadCssFile(provider, datadir_css);
  }
#endif

  if (!loaded) {
    gtk_css_provider_load_from_data(provider, kEmbeddedFallbackCss, -1, nullptr);
  }

  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

}  // namespace castcore::gui
