#ifndef CASTMIRROR_GUI_WIDGETS_H_
#define CASTMIRROR_GUI_WIDGETS_H_

#include <gtk/gtk.h>
#include <string>
#include "castcore/types.h"

namespace castcore::gui {

// Clears every semantic state class (is-idle / is-progress / is-live /
// is-warning / is-error / is-selected / is-disabled) from the widget,
// then adds state_class when it is non-empty. Single owner of the
// "remove semantic classes, add one" pattern.
void SetSemanticClass(GtkWidget* widget, const char* state_class);

// Formats a bitrate as "<mbps> Mbps" with the given decimal precision.
std::string FormatMbps(double mbps, int precision = 1);

// Builds the standard 18px check-mark image used to mark the selected row,
// initially hidden. Callers append it to their row's trailing box.
GtkWidget* MakeSelectIcon();

GtkWidget* MakeSectionHeader(const char* title, const char* one_liner = nullptr);

GtkWidget* MakeInfoButton(const char* title, const char* help_text);

GtkWidget* MakeStatCard(const char* title, const char* icon_name, const char* help_text,
                        GtkWidget** out_value_label);

GtkWidget* MakeStatCardWithSparkline(const char* title, const char* icon_name,
                                     const char* help_text, GtkWidget** out_value_label,
                                     GtkWidget* sparkline_widget);

void UpdateStatusBadge(GtkWidget* pill, GtkWidget* dot, GtkWidget* label, SessionState state);

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_WIDGETS_H_
