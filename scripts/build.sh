#!/usr/bin/env bash
# build.sh — Build AgentDesktop on macOS or Linux (single binary).
# Usage:
#   ./scripts/build.sh            # Release
#   ./scripts/build.sh Debug

set -e
CONFIG="${1:-Release}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo ""
echo "  ╔══════════════════════════════╗"
echo "  ║   AgentDesktop  Build        ║"
echo "  ╚══════════════════════════════╝"
echo "  Config : $CONFIG"
echo "  Root   : $ROOT"
echo ""

# ── Check cmake ───────────────────────────────────────────────────────────────
if ! command -v cmake &>/dev/null; then
    echo "ERROR: cmake not found."
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "  Install: brew install cmake"
    else
        echo "  Install: sudo apt install cmake  (or dnf/pacman equivalent)"
    fi
    exit 1
fi

# ── Linux: check for X11 + XTest dev packages ─────────────────────────────────
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    MISSING=()
    dpkg -s libx11-dev   &>/dev/null || MISSING+=("libx11-dev")
    dpkg -s libxtst-dev  &>/dev/null || MISSING+=("libxtst-dev")
    dpkg -s xvfb         &>/dev/null || MISSING+=("xvfb")
    if [ ${#MISSING[@]} -gt 0 ]; then
        echo "WARNING: Missing packages: ${MISSING[*]}"
        echo "  Install: sudo apt install ${MISSING[*]}"
        echo "  Continuing anyway…"
    fi
fi

# ── Configure ─────────────────────────────────────────────────────────────────
BUILD_DIR="$ROOT/build/$CONFIG"
echo "[1/3] Configuring → $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$CONFIG"

# ── Build ─────────────────────────────────────────────────────────────────────
echo ""
echo "[2/3] Building ($CONFIG)…"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

# ── Report ────────────────────────────────────────────────────────────────────
echo ""
echo "[3/3] Output:"
BIN="$ROOT/bin"
if [ -d "$BIN" ]; then
    ls -lh "$BIN"/AgentDesktop* 2>/dev/null || echo "  (no binary found — check build output)"
fi

echo ""
echo "  Done! Run:  $BIN/AgentDesktop"
echo ""
