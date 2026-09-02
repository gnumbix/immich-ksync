# ImmichKSync — a KDE 6 sync agent for Immich albums.
#
# There is no dependency-resolution step: every library used here ships with Qt 6,
# KDE Frameworks 6, or the C library. That is deliberate, and matches the macOS
# sibling of this project.

cmake_minimum_required(VERSION 3.23)

project(ImmichKSync VERSION 1.0.0 LANGUAGES CXX)

set(IMMICHKSYNC_APPID "com.gnumbix.immichksync")
set(IMMICHKSYNC_BINARY "immichksync")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM 6.0.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)
include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network DBus Concurrent Test)
find_package(SQLite3 REQUIRED)
find_package(KF6 6.0 REQUIRED COMPONENTS
    CoreAddons Config ConfigWidgets I18n Notifications
    StatusNotifierItem DBusAddons KIO WindowSystem IconThemes)


# Everything except the presentation layer lives in a static library, so the tests
# can link the real code rather than a copy of it.
add_library(immichksync_core STATIC)
target_include_directories(immichksync_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(immichksync_core PUBLIC KF6::KIOGui)
target_link_libraries(immichksync_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus Qt6::Concurrent
    KF6::CoreAddons KF6::ConfigCore KF6::I18n KF6::Notifications
    SQLite3::SQLite3)
target_compile_definitions(immichksync_core PUBLIC
    IMMICHKSYNC_VERSION="${PROJECT_VERSION}"
    IMMICHKSYNC_APPID="${IMMICHKSYNC_APPID}")

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

install(FILES data/${IMMICHKSYNC_APPID}.desktop
        DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES data/immichksync.notifyrc
        DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})
install(FILES data/${IMMICHKSYNC_APPID}.metainfo.xml
        DESTINATION ${KDE_INSTALL_METAINFODIR})
install(FILES data/icons/${IMMICHKSYNC_APPID}.svg
        DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)

# Rendered sizes as well as the SVG: the system tray on some setups asks for a fixed
# pixel size and will not rasterise an SVG itself.
foreach(size 16 22 24 32 48 64 128 256)
    install(FILES data/icons/${size}x${size}/${IMMICHKSYNC_APPID}.png
            DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/${size}x${size}/apps
            OPTIONAL)
endforeach()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
