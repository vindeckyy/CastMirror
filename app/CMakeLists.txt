cmake_minimum_required(VERSION 3.20)
project(castmirror_app CXX)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(AYATANA ayatana-appindicator3-0.1)
if(NOT AYATANA_FOUND)
  pkg_check_modules(AYATANA appindicator3-0.1)
endif()
pkg_check_modules(LIBNOTIFY libnotify)

# CLI Binary
add_executable(castmirror cli/main.cc)
target_link_libraries(castmirror PRIVATE castcore)

# Modern Desktop GUI Binary
set(GUI_SOURCES
  gui/main.cc
  gui/widgets.cc
  gui/css_loader.cc
  gui/gui_app.cc
  gui/cast_tab.cc
  gui/live_tab.cc
  gui/settings_tab.cc
  gui/logs_tab.cc
  gui/tray.cc
  gui/notify.cc
  gui/first_run.cc
)

add_executable(castmirror-gui ${GUI_SOURCES})
target_include_directories(castmirror-gui PRIVATE
  ${GTK3_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/gui
)
target_compile_options(castmirror-gui PRIVATE ${GTK3_CFLAGS_OTHER})

target_compile_definitions(castmirror-gui PRIVATE
  CASTMIRROR_DATADIR="${CMAKE_INSTALL_FULL_DATAROOTDIR}/castmirror"
  CASTMIRROR_SRC_CSS="${CMAKE_CURRENT_SOURCE_DIR}/gui/castmirror.css"
  CASTMIRROR_SRC_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(castmirror-gui PRIVATE castcore ${GTK3_LIBRARIES})

if(AYATANA_FOUND)
  target_compile_definitions(castmirror-gui PRIVATE CASTMIRROR_HAVE_TRAY=1)
  target_include_directories(castmirror-gui PRIVATE ${AYATANA_INCLUDE_DIRS})
  target_link_libraries(castmirror-gui PRIVATE ${AYATANA_LIBRARIES})
endif()

if(LIBNOTIFY_FOUND)
  target_compile_definitions(castmirror-gui PRIVATE CASTMIRROR_HAVE_NOTIFY=1)
  target_include_directories(castmirror-gui PRIVATE ${LIBNOTIFY_INCLUDE_DIRS})
  target_link_libraries(castmirror-gui PRIVATE ${LIBNOTIFY_LIBRARIES})
endif()

install(TARGETS castmirror castmirror-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES castmirror.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/docs/assets/logo.svg DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps RENAME castmirror.svg)
install(FILES gui/castmirror.css DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/castmirror)
