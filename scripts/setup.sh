#!/usr/bin/env bash
# Copyright 2024-2026 magicint1337. Licensed under LGPL 3.0 (see COPYING).
#
# setup.sh - All-in-one VertexSDR setup: install dependencies, build, fetch frontend.
#
# Usage:
#   bash scripts/setup.sh              # CPU build (FFTW)
#   bash scripts/setup.sh --vulkan     # GPU build (VkFFT)
#   bash scripts/setup.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

USE_VULKAN=0

for arg in "$@"; do
  case "$arg" in
    --vulkan) USE_VULKAN=1 ;;
    --help|-h)
      echo "Usage: $0 [--vulkan]"
      echo "  --vulkan   Build with Vulkan GPU FFT support (requires Vulkan SDK)"
      exit 0
      ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

info()  { echo "[setup] $*"; }
die()   { echo "[setup] ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Check OS
# ---------------------------------------------------------------------------
info "Checking system..."
if ! command -v apt-get >/dev/null 2>&1; then
  die "This script requires apt-get (Debian/Ubuntu). Install dependencies manually."
fi

# ---------------------------------------------------------------------------
# 2. Install dependencies
# ---------------------------------------------------------------------------
info "Installing core build dependencies..."
sudo apt-get install -y \
  build-essential \
  libfftw3-dev \
  libpng-dev \
  libasound2-dev \
  libssl-dev \
  xxd \
  patch \
  curl

if [ "$USE_VULKAN" -eq 1 ]; then
  info "Installing Vulkan dependencies..."
  sudo apt-get install -y \
    libvulkan-dev \
    glslang-tools \
    spirv-tools
fi

# ---------------------------------------------------------------------------
# 3. Build VertexSDR
# ---------------------------------------------------------------------------
info "Building VertexSDR..."
cd "$REPO_ROOT"
if [ "$USE_VULKAN" -eq 1 ]; then
  make USE_VULKAN=1
else
  make
fi
info "Build complete: $REPO_ROOT/vertexsdr"

# ---------------------------------------------------------------------------
# 4. Fetch WebSDR frontend files
# ---------------------------------------------------------------------------
info "Fetching WebSDR frontend files..."
bash "$SCRIPT_DIR/fetch-frontend.sh"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
info ""
info "Setup complete."
info ""
info "Next steps:"
info "  1. Copy websdr.cfg.example to websdr.cfg and configure your SDR device"
info "  2. Run: ./vertexsdr"
info "  3. Open http://localhost:8901 in a browser"
info ""
if [ "$USE_VULKAN" -eq 1 ]; then
  info "GPU build: set 'fftbackend vkfft' in websdr.cfg to use Vulkan FFT."
fi
