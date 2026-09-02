#ifndef CASTMIRROR_GUI_WIDGETS_H_
#define CASTMIRROR_GUI_WIDGETS_H_

#include <gtk/gtk.h>
#include "castcore/types.h"

namespace castcore::gui {

GtkWidget* MakeSectionHeader(const char* title, const char* one_liner = nullptr);

GtkWidget* MakeInfoButton(const char* title, const char* help_text);

GtkWidget* MakeStatCard(const char* title,
                        const char* icon_name,
                        const char* help_text,
                        GtkWidget** out_value_label);

GtkWidget* MakeStatCardWithSparkline(const char* title,
                                     const char* icon_name,
                                     const char* help_text,
                                     GtkWidget** out_value_label,
                                     GtkWidget* sparkline_widget);

void UpdateStatusBadge(GtkWidget* pill,
                       GtkWidget* dot,
                       GtkWidget* label,
                       SessionState state);

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_WIDGETS_H_
