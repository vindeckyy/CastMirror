#ifndef CASTMIRROR_GUI_I18N_H_
#define CASTMIRROR_GUI_I18N_H_

// ---------------------------------------------------------------------------
// CastMirror GTK GUI — single include point for translation.
//
// Include this header in every GUI translation unit that needs to mark a
// user-visible string.  The actual wrapping of literals in _() is a separate
// pass; this header exists so that pass has exactly one place to include and
// one place to change if the gettext plumbing moves.
//
// Build matrix (chosen by app/CMakeLists.txt):
//   * CASTMIRROR_HAVE_INTL defined   -> _() is gettext(), the GUI is localized
//                                       when a catalogue for the user's locale
//                                       is installed.
//   * CASTMIRROR_HAVE_INTL undefined -> _() and N_() are identity macros, so
//                                       the GUI still builds and behaves
//                                       exactly as before (English only).
//
// N_() is a no-op marker.  It evaluates to the string it is given (so a
// constant declared as `N_("...")` keeps its exact type and value) but tells
// xgettext to extract the literal.  po/POTFILES.in lists this header's
// consumers and the extractor is invoked with `--keyword=N_`.
//
// Typical use:
//   #include "i18n.h"
//   gtk_label_new(_("Where to cast"));       // translated immediately
//   static const char* kTitle = N_("Sound"); // marked now, _()d at the use site
// ---------------------------------------------------------------------------

#if defined(CASTMIRROR_HAVE_INTL)
#include <libintl.h>
#else
// No libintl: nothing to declare.  The macros below collapse to identity.
#endif

// Guarded so a TU that already pulled in <glib/gi18n.h> does not redefine them.
#ifndef _
#if defined(CASTMIRROR_HAVE_INTL)
#define _(String) gettext(String)
#else
#define _(String) (String)
#endif
#endif

#ifndef N_
#define N_(String) (String)
#endif

// gettext domain shared by the GUI (and, later, the CLI).  Kept in sync with
// CASTMIRROR_GETTEXT_DOMAIN in app/CMakeLists.txt and the .mo file names.
#ifndef CASTMIRROR_GETTEXT_DOMAIN
#define CASTMIRROR_GETTEXT_DOMAIN "castmirror"
#endif

// Install-time locale root.  app/CMakeLists.txt injects the real value from
// GNUInstallDirs; this fallback keeps a hand-rolled (non-CMake) build working.
#if defined(CASTMIRROR_HAVE_INTL) && !defined(CASTMIRROR_LOCALEDIR)
#define CASTMIRROR_LOCALEDIR "/usr/share/locale"
#endif

#endif  // CASTMIRROR_GUI_I18N_H_
