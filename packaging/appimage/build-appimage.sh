#!/usr/bin/env bash
# Builds an AppImage of ImmichKSync.
#
# NOT verified on the development machine: appimagetool is not installed there, so this
# recipe ships documented but unproven, and is expected to run in CI or on a machine
# where the tool has been downloaded. Everything up to the appimagetool invocation is
# ordinary CMake and does work locally.
#
# Needs `linuxdeploy` with its Qt plugin, and `appimagetool`:
#   https://github.com/linuxdeploy/linuxdeploy
#   https://github.com/AppImage/AppImageKit
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${ROOT}/build-appimage"
APPDIR="${BUILD}/AppDir"
APPID="com.gnumbix.immichksync"

command -v linuxdeploy >/dev/null || { echo "linuxdeploy is not on PATH" >&2; exit 1; }
command -v appimagetool >/dev/null || { echo "appimagetool is not on PATH" >&2; exit 1; }

rm -rf "${BUILD}"
cmake -B "${BUILD}" -S "${ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "${BUILD}"
DESTDIR="${APPDIR}" cmake --install "${BUILD}"

# KDE Frameworks are not part of any AppImage base, so they are bundled along with Qt.
# The result is large for a tray daemon — that is the trade for running anywhere.
linuxdeploy \
    --appdir "${APPDIR}" \
    --plugin qt \
    --desktop-file "${APPDIR}/usr/share/applications/${APPID}.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/${APPID}.svg"

appimagetool "${APPDIR}" "${ROOT}/dist/ImmichKSync-$(git -C "${ROOT}" describe --tags --always).AppImage"
echo "AppImage written to ${ROOT}/dist/"
