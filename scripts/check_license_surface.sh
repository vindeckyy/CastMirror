#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NOTICE_FILE="$REPO_ROOT/NOTICE"
BUILD_DIR="$REPO_ROOT/build"

echo "=== CastMirror License Surface & GPL Hygiene Check ==="

if [[ ! -f "$NOTICE_FILE" ]]; then
  echo "ERROR: NOTICE file not found at $NOTICE_FILE"
  exit 1
fi

# Verify mandatory disclosures exist in NOTICE
REQUIRED_TERMS=("libx264" "FFmpeg" "OpenSSL" "protobuf" "Opus" "libadwaita" "GPL")
for term in "${REQUIRED_TERMS[@]}"; do
  if ! grep -qi "$term" "$NOTICE_FILE"; then
    echo "ERROR: Mandatory third-party component or disclosure '$term' missing from $NOTICE_FILE"
    exit 1
  fi
done

# If build directory contains binaries, check dynamic dependencies via ldd
BINARIES=(
  "$BUILD_DIR/app/castmirror"
  "$BUILD_DIR/app/castmirror-gui"
  "$BUILD_DIR/tests/castmirror_tests"
)

for bin in "${BINARIES[@]}"; do
  if [[ -f "$bin" ]]; then
    echo "Checking linkage surface of $bin..."
    DIRECT_DEPS=$(readelf -d "$bin" 2>/dev/null | grep NEEDED || true)
    TRANSITIVE_DEPS=$(ldd "$bin" 2>/dev/null || true)

    # Ensure forbidden GPLv3 libraries are never directly linked into CastMirror
    if echo "$DIRECT_DEPS" | grep -qi "x265"; then
      echo "  [ERROR] Prohibited direct dependency on libx265 linked into $bin!"
      exit 1
    fi

    # Check if transitive distro FFmpeg brings in libx264
    if echo "$TRANSITIVE_DEPS" | grep -q "libx264"; then
      echo "  [INFO] libx264 detected in runtime linkage surface."
      if ! grep -q "treating that binary" "$NOTICE_FILE"; then
        echo "  [ERROR] Binary has libx264 in linkage surface but NOTICE does not document GPL implications!"
        exit 1
      fi
    fi
  fi
done

echo "SUCCESS: License surface and GPL hygiene checks passed."
