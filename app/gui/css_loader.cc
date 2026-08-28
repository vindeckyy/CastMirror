#include "css_loader.h"

#include "castcore/logger.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace castcore::gui {

namespace {

constexpr const char* kResourceCss = "/io/github/vindeckyy/CastMirror/castmirror.css";
constexpr const char* kIconResourcePath = "/io/github/vindeckyy/CastMirror/icons";

struct CssLoadState {
  bool had_error = false;
};

void OnCssParsingError(GtkCssProvider* /*provider*/,
                       GtkCssSection* /*section*/,
                       GError* error,
                       gpointer user_data) {
  auto* state = static_cast<CssLoadState*>(user_data);
  if (state) {
    state->had_error = true;
  }
  if (error && error->message) {
    LOG_WARN << "[UI] CSS parsing error: " << error->message;
  }
}

bool LoadCssFromPath(GtkCssProvider* provider, const std::string& path, CssLoadState* state) {
  if (path.empty() || !std::filesystem::exists(path)) {
    return false;
  }
  state->had_error = false;
  gtk_css_provider_load_from_path(provider, path.c_str());
  return !state->had_error;
}

void LoadCssFromResource(GtkCssProvider* provider) {
  gtk_css_provider_load_from_resource(provider, kResourceCss);
}

}  // namespace

void ApplyApplicationTheme(GdkDisplay* display) {
  if (!display) {
    display = gdk_display_get_default();
  }
  if (!display) {
    LOG_WARN << "[UI] No GdkDisplay available to install application CSS";
    return;
  }

  GtkIconTheme* icon_theme = gtk_icon_theme_get_for_display(display);
  if (icon_theme) {
    gtk_icon_theme_add_resource_path(icon_theme, kIconResourcePath);
  }

  GtkCssProvider* provider = gtk_css_provider_new();
  CssLoadState state;
  const gulong parse_id =
      g_signal_connect(provider, "parsing-error", G_CALLBACK(OnCssParsingError), &state);

  bool loaded = false;
  const char* env_css = std::getenv("CASTMIRROR_CSS");
  if (env_css && env_css[0] != '\0') {
    loaded = LoadCssFromPath(provider, env_css, &state);
    if (!loaded) {
      LOG_WARN << "[UI] CASTMIRROR_CSS override failed; loading bundled stylesheet instead: "
               << env_css;
    }
  }

  if (!loaded) {
    state.had_error = false;
    LoadCssFromResource(provider);
  }

  g_signal_handler_disconnect(provider, parse_id);

  gtk_style_context_add_provider_for_display(
      display,
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

}  // namespace castcore::gui
