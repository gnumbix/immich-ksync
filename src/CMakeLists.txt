# Sources are listed explicitly rather than globbed, so adding a file is a visible
# change in the build and CMake re-runs when it happens.
target_sources(immichksync_core PRIVATE
    core/Logging.cpp
    core/Clock.cpp
    core/Checksum.cpp
    core/Retry.cpp
    core/TaskPool.cpp
    core/Preferences.cpp
    storage/SqliteDatabase.cpp
    storage/Migrations.cpp
    storage/Records.cpp
    storage/SyncStore.cpp
    filesystem/AlbumFolderLayout.cpp
    filesystem/FileIdentity.cpp
    filesystem/FileHasher.cpp
    filesystem/FileScanner.cpp
    filesystem/AtomicFileWriter.cpp
    filesystem/LocalTrash.cpp
    filesystem/FolderWatcher.cpp
    filesystem/RootFolderAccess.cpp
    immich/ImmichError.cpp
    immich/ImmichDtos.cpp
    immich/MediaTypeCatalog.cpp
    immich/Transport.cpp
    immich/MultipartBody.cpp
    immich/ImmichClient.cpp
    immich/ServerDiscovery.cpp
    credentials/ImmichCredentials.cpp
    credentials/CertificateSummary.cpp
    credentials/SecretStore.cpp
    credentials/TlsCertificateStore.cpp
    sync/SyncPlan.cpp
    sync/SyncPlanner.cpp
    sync/SafetyGate.cpp
    sync/SyncStatus.cpp
    sync/SyncExecutor.cpp
    sync/SyncEngine.cpp
    notifications/UserNotifier.cpp
)

# The presentation layer. Kept out of the core library so a headless test run never
# needs a display or a session bus.
add_executable(${IMMICHKSYNC_BINARY}
    app/main.cpp
    app/AppInfo.cpp
    app/AppEnvironment.cpp
    app/SystemEvents.cpp
    tray/TrayIcon.cpp
    settings/SettingsWindow.cpp
    settings/SettingsWidgets.cpp
    settings/ServerSettingsTab.cpp
    settings/FolderSettingsTab.cpp
    settings/AlbumsSettingsTab.cpp
    settings/AdvancedSettingsTab.cpp
    settings/LogTailWidget.cpp
)
target_link_libraries(${IMMICHKSYNC_BINARY} PRIVATE
    immichksync_core
    Qt6::Widgets
    KF6::StatusNotifierItem KF6::DBusAddons KF6::ConfigWidgets
    KF6::WindowSystem KF6::IconThemes KF6::KIOGui)

install(TARGETS ${IMMICHKSYNC_BINARY} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
