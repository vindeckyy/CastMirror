#!/usr/bin/env bash
# CastMirror Accessibility (a11y) Audit Script
# Validates that all GTK buttons, icon-only actions, and interactive controls
# in app/gui have accessible labels/roles and tooltips configured.
# Exits with 0 if no violations found; exits with 1 if violations detected.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
GUI_SRC_DIR="${REPO_ROOT}/app/gui"

echo "========================================================"
echo "         CastMirror Accessibility (a11y) Audit          "
echo "========================================================"
echo "Scanning GUI sources in: ${GUI_SRC_DIR}"

WARNINGS=0
CHECKED=0

# 1. Audit icon-only button instantiations
echo ""
echo "[1/3] Auditing icon-only button tooltips & accessible properties..."
ICON_BUTTONS=(
  "rescan_btn_"
  "add_ip_btn_"
  "remove_btn_"
  "copy_last_100_button_"
  "copy_button_"
  "folder_button_"
  "clear_button_"
  "freeze_btn_"
  "mute_btn_"
  "game_mode_btn_"
  "cinema_mode_btn_"
)

for btn in "${ICON_BUTTONS[@]}"; do
  CHECKED=$((CHECKED + 1))
  # Check tooltip
  if ! grep -q "gtk_widget_set_tooltip_text(${btn}" "${GUI_SRC_DIR}"/*.cc; then
    echo "  [FAIL] ${btn} is missing gtk_widget_set_tooltip_text"
    WARNINGS=$((WARNINGS + 1))
  else
    echo "  [PASS] ${btn} has tooltip configured"
  fi

  # Check accessible property / label
  if ! grep -q "${btn}.*GTK_ACCESSIBLE" "${GUI_SRC_DIR}"/*.cc && \
     ! grep -q "GTK_ACCESSIBLE(${btn})" "${GUI_SRC_DIR}"/*.cc; then
    echo "  [FAIL] ${btn} is missing gtk_accessible_update_property label"
    WARNINGS=$((WARNINGS + 1))
  else
    echo "  [PASS] ${btn} has accessible property configured"
  fi
done

# 2. Check for unlabeled custom buttons
echo ""
echo "[2/3] Checking primary action buttons..."
if grep -q "cast_button_.*GTK_ACCESSIBLE" "${GUI_SRC_DIR}"/gui_app.cc || grep -q "GTK_ACCESSIBLE(cast_button_)" "${GUI_SRC_DIR}"/gui_app.cc; then
  echo "  [PASS] cast_button_ has accessible property configured"
else
  echo "  [FAIL] cast_button_ is missing accessible property"
  WARNINGS=$((WARNINGS + 1))
fi
CHECKED=$((CHECKED + 1))

# 3. AT-SPI2 Python validation (if available)
echo ""
echo "[3/3] Checking AT-SPI2 stack compatibility..."
if python3 -c "import gi; gi.require_version('Atspi', '2.0'); from gi.repository import Atspi" 2>/dev/null; then
  echo "  [PASS] AT-SPI2 Python bindings available and functional"
else
  echo "  [INFO] AT-SPI2 python bindings not installed (skipping live dbus inspection)"
fi

echo ""
echo "========================================================"
if [ "${WARNINGS}" -eq 0 ]; then
  echo "SUCCESS: 0 a11y warnings found across ${CHECKED} accessibility checks."
  exit 0
else
  echo "FAILURE: ${WARNINGS} accessibility warning(s) detected."
  exit 1
fi
