#ifndef CASTMIRROR_GUI_WIDGETS_H_
#define CASTMIRROR_GUI_WIDGETS_H_

#include <gtk/gtk.h>
#include <string>
#include "castcore/types.h"

namespace castcore::gui {

// Creates an uppercase section title with an optional one-line description
GtkWidget* MakeSectionHeader(const char* title, const char* one_liner = nullptr);

// Creates a small (i) button that opens a wrapped GtkPopover with help text
GtkWidget* MakeInfoButton(const char* help_text);

// Creates a full-width settings row: left has title + description + (i) popover, right has control
GtkWidget* MakeSettingRow(const char* title,
                          const char* one_liner,
                          const char* popover_text,
                          GtkWidget* control_widget);

// Creates a telemetry stat card with title, large value label, and (i) popover
GtkWidget* MakeStatCard(const char* title,
                        const char* popover_text,
                        GtkWidget** out_value_label);

// Updates style classes for a status badge pill
void UpdateStatusBadge(GtkWidget* badge, GtkWidget* label, SessionState state);

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_WIDGETS_H_
