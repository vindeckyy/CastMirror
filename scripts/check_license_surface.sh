#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NOTICE_FILE="$REPO_ROOT/NOTICE"
BUILD_DIR="${BUILD_DIR:-}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--build-dir DIR]

Checks NOTICE disclosures and inspects compiled CastMirror binaries for
prohibited direct GPLv3 (libx265) or transitive GPL (libx264) linkage.

Options:
  --build-dir DIR   Build directory containing app/ and tests/ binaries
                    (or set BUILD_DIR; defaults to auto-detecting build,
                    build-swarm, or build-asan).
  --help, -h        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --build-dir requires a value." >&2; exit 2; }
      BUILD_DIR="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    *)
      echo "ERROR: Unknown option: $1" >&2
      usage >&2
      exit 2 ;;
  esac
done

echo "=== CastMirror License Surface & GPL Hygiene Check ==="

if [[ ! -f "$NOTICE_FILE" ]]; then
  echo "ERROR: NOTICE file not found at $NOTICE_FILE" >&2
  exit 1
fi

# Verify mandatory disclosures exist in NOTICE
REQUIRED_TERMS=("libx264" "FFmpeg" "OpenSSL" "protobuf" "Opus" "libadwaita" "GPL")
for term in "${REQUIRED_TERMS[@]}"; do
  if ! grep -qi "$term" "$NOTICE_FILE"; then
    echo "ERROR: Mandatory third-party component or disclosure '$term' missing from $NOTICE_FILE" >&2
    exit 1
  fi
done

has_any_binary() {
  local dir="$1"
  [[ -f "$dir/app/castmirror" || -f "$dir/app/castmirror-gui" || -f "$dir/tests/castmirror_tests" ]]
}

if [[ -n "$BUILD_DIR" ]]; then
  if [[ ! -d "$BUILD_DIR" && -d "$REPO_ROOT/$BUILD_DIR" ]]; then
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
  fi
else
  for cand in "$REPO_ROOT/build" "$REPO_ROOT/build-swarm" "$REPO_ROOT/build-asan"; do
    if has_any_binary "$cand"; then
      BUILD_DIR="$cand"
      break
    fi
  done
  if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$REPO_ROOT/build"
  fi
fi

if ! command -v readelf >/dev/null 2>&1 || ! command -v ldd >/dev/null 2>&1; then
  echo "ERROR: Both 'readelf' and 'ldd' are required to inspect binary linkage surface." >&2
  exit 1
fi

BINARIES=(
  "$BUILD_DIR/app/castmirror"
  "$BUILD_DIR/app/castmirror-gui"
  "$BUILD_DIR/tests/castmirror_tests"
)

CHECKED_COUNT=0
for bin in "${BINARIES[@]}"; do
  if [[ -f "$bin" ]]; then
    CHECKED_COUNT=$((CHECKED_COUNT + 1))
    echo "Checking linkage surface of $bin..."
    DIRECT_DEPS=$(readelf -d "$bin" 2>/dev/null | grep NEEDED || true)
    TRANSITIVE_DEPS=$(ldd "$bin" 2>/dev/null || true)

    # Ensure forbidden GPLv3 libraries are never directly linked into CastMirror
    if echo "$DIRECT_DEPS" | grep -qi "x265"; then
      echo "  [ERROR] Prohibited direct dependency on libx265 linked into $bin!" >&2
      exit 1
    fi

    # Check if transitive distro FFmpeg brings in libx264
    if echo "$TRANSITIVE_DEPS" | grep -q "libx264"; then
      echo "  [INFO] libx264 detected in runtime linkage surface."
      if ! grep -q "treating that binary" "$NOTICE_FILE"; then
        echo "  [ERROR] Binary has libx264 in linkage surface but NOTICE does not document GPL implications!" >&2
        exit 1
      fi
    fi
  fi
done

if [[ "$CHECKED_COUNT" -eq 0 ]]; then
  echo "ERROR: No CastMirror binaries found under '$BUILD_DIR' (checked app/castmirror, app/castmirror-gui, tests/castmirror_tests)." >&2
  echo "       Pass --build-dir DIR or set BUILD_DIR to a built tree." >&2
  exit 1
fi

echo "SUCCESS: License surface and GPL hygiene checks passed ($CHECKED_COUNT binaries checked in $BUILD_DIR)."
