#include "css_loader.h"

#include "castcore/logger.h"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace castcore::gui {

namespace {

constexpr const char* kResourceCss = "/io/github/vindeckyy/CastMirror/castmirror.css";
constexpr const char* kIconResourcePath = "/io/github/vindeckyy/CastMirror/icons";

constexpr const char* kLightThemeOverridesCss = R"(
@define-color cm_background #F5F7FA;
@define-color cm_surface #FFFFFF;
@define-color cm_surface_container #EDF1F5;
@define-color cm_surface_high #E3E9EF;
@define-color cm_surface_highest #D7E0E8;
@define-color cm_outline_variant #D0D9E2;
@define-color cm_control_outline #7D8C97;
@define-color cm_text #11181F;
@define-color cm_text_secondary #4A5965;
@define-color cm_brand #007799;
@define-color cm_primary #007799;
@define-color cm_primary_container #D4EEF6;
@define-color cm_on_primary #FFFFFF;
@define-color cm_healthy #157347;
@define-color cm_healthy_container #DDF3E7;
@define-color cm_warning #9A6700;
@define-color cm_warning_container #FFF3CD;
@define-color cm_error #B3261E;
@define-color cm_error_container #FDE8E6;
)";

struct LightThemeBinding {
  GdkDisplay* display = nullptr;
  GtkCssProvider* light_provider = nullptr;
  bool active = false;
};

void SyncLightThemeOverride(AdwStyleManager* sm, LightThemeBinding* binding) {
  if (!sm || !binding || !binding->display || !binding->light_provider) {
    return;
  }
  const bool want_light = !adw_style_manager_get_dark(sm);
  if (want_light && !binding->active) {
    gtk_style_context_add_provider_for_display(binding->display,
                                               GTK_STYLE_PROVIDER(binding->light_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    binding->active = true;
  } else if (!want_light && binding->active) {
    gtk_style_context_remove_provider_for_display(binding->display,
                                                  GTK_STYLE_PROVIDER(binding->light_provider));
    binding->active = false;
  }
}

struct CssLoadState {
  bool had_error = false;
};

void OnCssParsingError(GtkCssProvider* /*provider*/, GtkCssSection* /*section*/, GError* error,
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
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  AdwStyleManager* sm = adw_style_manager_get_default();
  if (sm) {
    GtkCssProvider* light_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(light_provider, kLightThemeOverridesCss);
    auto* binding = new LightThemeBinding{display, light_provider, false};
    SyncLightThemeOverride(sm, binding);
    g_signal_connect_data(
        sm,
        "notify::dark",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer user_data) {
          SyncLightThemeOverride(ADW_STYLE_MANAGER(obj),
                                 static_cast<LightThemeBinding*>(user_data));
        }),
        binding,
        +[](gpointer data, GClosure*) {
          auto* b = static_cast<LightThemeBinding*>(data);
          if (b->light_provider) {
            g_object_unref(b->light_provider);
          }
          delete b;
        },
        G_CONNECT_DEFAULT);
  }
}

}  // namespace castcore::gui
