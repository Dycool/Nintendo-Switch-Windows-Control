#!/bin/bash
# Build Linux GUI app (requires SDL2, auto-downloads Dear ImGui)
# Install: sudo apt install libsdl2-dev  # Debian/Ubuntu

set -euo pipefail

IMGUI_VERSION="v1.91.0"
IMGUI_DIR="$(mktemp -d)"

# Download & extract Dear ImGui to a temporary directory
curl -sL "https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_VERSION}.zip" -o "${IMGUI_DIR}/imgui.zip"
unzip -q -o "${IMGUI_DIR}/imgui.zip" -d "${IMGUI_DIR}"
IMGUI_SRC="${IMGUI_DIR}/imgui-1.91.0"

g++ -O3 -std=c++17 ns-gui.cpp \
    "${IMGUI_SRC}/imgui.cpp" \
    "${IMGUI_SRC}/imgui_draw.cpp" \
    "${IMGUI_SRC}/imgui_tables.cpp" \
    "${IMGUI_SRC}/imgui_widgets.cpp" \
    "${IMGUI_SRC}/backends/imgui_impl_sdl2.cpp" \
    "${IMGUI_SRC}/backends/imgui_impl_sdlrenderer2.cpp" \
    -I"${IMGUI_SRC}" -I"${IMGUI_SRC}/backends" \
    -o ns-gui -lpthread -lSDL2

rm -rf "${IMGUI_DIR}"

echo "Built ns-gui"
