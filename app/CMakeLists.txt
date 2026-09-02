cmake_minimum_required(VERSION 3.20)
project(castmirror_app LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)

# CLI Binary
add_executable(castmirror cli/main.cc)
target_link_libraries(castmirror PRIVATE castcore)
install(TARGETS castmirror RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NOT CASTMIRROR_BUILD_GUI)
  set(CASTMIRROR_TRAY_LINKED OFF PARENT_SCOPE)
  return()
endif()

pkg_check_modules(GTK4 REQUIRED gtk4>=4.12)
pkg_check_modules(LIBADWAITA REQUIRED libadwaita-1>=1.5)

set(CASTMIRROR_HAVE_TRAY_LIB OFF)
if(CASTMIRROR_ENABLE_TRAY)
  pkg_check_modules(AYATANA ayatana-appindicator-glib>=2.0)
  if(AYATANA_FOUND)
    set(CASTMIRROR_HAVE_TRAY_LIB ON)
  endif()
endif()

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(CASTMIRROR_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/gui/castmirror.gresource.xml)
set(CASTMIRROR_GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/castmirror-resources.c)
add_custom_command(
  OUTPUT ${CASTMIRROR_GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES}
          --target=${CASTMIRROR_GRESOURCE_C}
          --sourcedir=${CMAKE_SOURCE_DIR}
          --generate-source
          ${CASTMIRROR_GRESOURCE_XML}
  DEPENDS
    ${CASTMIRROR_GRESOURCE_XML}
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/castmirror.css
    ${CMAKE_SOURCE_DIR}/docs/assets/logo.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/icons/castmirror-signal-symbolic.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/icons/castmirror-live-symbolic.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/icons/castmirror-settings-symbolic.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/gui/icons/castmirror-logs-symbolic.svg
  VERBATIM
)

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
  gui/sparkline.cc
  ${CASTMIRROR_GRESOURCE_C}
)

add_executable(castmirror-gui ${GUI_SOURCES})
set_source_files_properties(${CASTMIRROR_GRESOURCE_C} PROPERTIES GENERATED TRUE)
target_include_directories(castmirror-gui PRIVATE
  ${GTK4_INCLUDE_DIRS}
  ${LIBADWAITA_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/gui
)
target_compile_options(castmirror-gui PRIVATE ${GTK4_CFLAGS_OTHER} ${LIBADWAITA_CFLAGS_OTHER})
target_compile_definitions(castmirror-gui PRIVATE
  CASTMIRROR_VERSION="${CastMirror_VERSION}"
)
target_link_libraries(castmirror-gui PRIVATE
  castcore
  ${GTK4_LIBRARIES}
  ${LIBADWAITA_LIBRARIES}
)

if(CASTMIRROR_HAVE_TRAY_LIB)
  target_compile_definitions(castmirror-gui PRIVATE CASTMIRROR_HAVE_TRAY=1)
  target_include_directories(castmirror-gui PRIVATE ${AYATANA_INCLUDE_DIRS})
  target_compile_options(castmirror-gui PRIVATE ${AYATANA_CFLAGS_OTHER})
  target_link_libraries(castmirror-gui PRIVATE ${AYATANA_LIBRARIES})
  set(CASTMIRROR_TRAY_LINKED ON PARENT_SCOPE)
else()
  set(CASTMIRROR_TRAY_LINKED OFF PARENT_SCOPE)
endif()

install(TARGETS castmirror-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES io.github.vindeckyy.CastMirror.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/docs/assets/logo.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME io.github.vindeckyy.CastMirror.svg)
install(FILES gui/icons/castmirror-signal-symbolic.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/symbolic/status
        RENAME io.github.vindeckyy.CastMirror-attention-symbolic.svg)
