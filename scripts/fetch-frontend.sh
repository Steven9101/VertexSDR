#!/usr/bin/env bash
# Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).
#
# fetch-frontend.sh - Download WebSDR frontend files and apply VertexSDR patches.
#
# The WebSDR frontend files (websdr-base.js, websdr-waterfall.js, etc.) are
# copyright Pieter-Tjerk de Boer (PA3FWM) - all rights reserved.
# VertexSDR does NOT distribute these files. This script downloads them from
# community WebSDR operator repositories and applies VertexSDR-specific patches.
#
# Primary source: WeirdNewbie2/websdr (community WebSDR operator config share)
#   https://github.com/WeirdNewbie2/websdr
#
# Fallback sources:
#   https://github.com/reynico/raspberry-websdr
#   https://github.com/FarnhamSDR/websdr
#   https://github.com/ON5HB/websdr
#
# websdr-waterfall.js is always fetched from reynico/raspberry-websdr because
# that version retains PA3FWM's original copyright header (WeirdNewbie2's copy
# strips it). The rest of the content is identical.
#
# By running this script you acknowledge that the downloaded files remain under
# their original copyright and you must comply with the WebSDR license terms.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PUB2="$REPO_ROOT/pub2"
PATCHES="$REPO_ROOT/patches"

# ---------------------------------------------------------------------------
# Sources - ordered by preference
# ---------------------------------------------------------------------------
# Primary: WeirdNewbie2 (visual baseline for VertexSDR's frontend appearance)
PRIMARY_BASE="https://raw.githubusercontent.com/WeirdNewbie2/websdr/main/dist/pub2"

declare -A FALLBACK_BASES=(
  [reynico]="https://raw.githubusercontent.com/reynico/raspberry-websdr/master/dist11/pub2"
  [farnham]="https://raw.githubusercontent.com/FarnhamSDR/websdr/master/dist11/pub2"
  [on5hb]="https://raw.githubusercontent.com/ON5HB/websdr/master/dist11/pub2"
)

# websdr-waterfall.js: always fetch from reynico - it retains PA3FWM's
# copyright header that WeirdNewbie2's copy strips. Content is otherwise
# identical. Using the version with the copyright notice in-file is preferable.
WATERFALL_BASE="https://raw.githubusercontent.com/reynico/raspberry-websdr/master/dist11/pub2"

# Files downloaded from upstream (not shipped in VertexSDR repo)
PLAIN_FILES=(
  "carrier.png"
  "crossdomain.xml"
  "edgelower.png"
  "edgelowerbb.png"
  "edgeupper.png"
  "edgeupperbb.png"
  "scaleblack.png"
  "smeter1.png"
  "sysop.html"
  "websdr-1405020937.jar"
  "websdr-javasound.js"
  "websdr-javawaterfall.js"
)

# Files downloaded then patched with VertexSDR changes
PATCHED_FILES=(
  "websdr-base.js"
  "websdr-sound.js"
  "websdr-controls.html"
  "websdr-head.html"
  "index.html"
  "m.html"
  "mobile-controls.html"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
info()  { echo "[fetch-frontend] $*"; }
warn()  { echo "[fetch-frontend] WARNING: $*" >&2; }
die()   { echo "[fetch-frontend] ERROR: $*" >&2; exit 1; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1 - please install it."
}

download_file() {
  local url="$1"
  local dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$dest"
  elif command -v wget >/dev/null 2>&1; then
    wget -q "$url" -O "$dest"
  else
    die "Neither curl nor wget found. Install one of them and retry."
  fi
}

try_download() {
  local base="$1"
  local file="$2"
  local dest="$3"
  local url="$base/$file"
  if download_file "$url" "$dest" 2>/dev/null; then
    # Sanity check: file should be at least 100 bytes
    local sz
    sz=$(wc -c < "$dest" 2>/dev/null || echo 0)
    if [ "$sz" -lt 100 ]; then
      rm -f "$dest"
      return 1
    fi
    return 0
  fi
  return 1
}

fetch_file() {
  local file="$1"
  local base="${2:-$PRIMARY_BASE}"
  local dest="$PUB2/$file"

  # Try specified/primary source
  if try_download "$base" "$file" "$dest"; then
    info "  $file (${2:+$2 }primary)"
    return 0
  fi
  warn "$file: primary source failed, trying fallbacks..."

  # Try fallbacks
  for name in "${!FALLBACK_BASES[@]}"; do
    if try_download "${FALLBACK_BASES[$name]}" "$file" "$dest"; then
      info "  $file (fallback: $name)"
      return 0
    fi
  done

  die "Could not download $file from any source."
}

apply_patch() {
  local file="$1"
  local patchfile="$PATCHES/${file}.patch"
  local dest="$PUB2/$file"

  if [ ! -f "$patchfile" ] || [ ! -s "$patchfile" ]; then
    info "  no patch for $file - using upstream version as-is"
    return 0
  fi

  if patch -s -p0 "$dest" "$patchfile" 2>/dev/null; then
    info "  patch applied: $file"
  else
    warn "Patch for $file did not apply cleanly. Upstream version may have changed."
    warn "Keeping upstream version - VertexSDR-specific features may be missing."
  fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
require_cmd patch

info "Creating pub2/ directory..."
mkdir -p "$PUB2"

info "Downloading plain files (no VertexSDR modifications)..."
for f in "${PLAIN_FILES[@]}"; do
  fetch_file "$f"
done

info "Downloading websdr-waterfall.js (from reynico - retains copyright header)..."
fetch_file "websdr-waterfall.js" "$WATERFALL_BASE"

info "Downloading and patching VertexSDR-modified files..."
for f in "${PATCHED_FILES[@]}"; do
  fetch_file "$f"
  apply_patch "$f"
done

info ""
info "Done. Frontend files are ready in pub2/"
info ""
info "NOTE: The downloaded files are copyright Pieter-Tjerk de Boer (PA3FWM)."
info "      VertexSDR does not redistribute them. See docs/frontend-setup.md"
info "      for full legal details."
