#pragma once

#include "app/SystemEvents.h"
#include "core/Preferences.h"
#include "credentials/SecretStore.h"
#include "credentials/TlsCertificateStore.h"
#include "immich/Transport.h"
#include "notifications/UserNotifier.h"
#include "storage/SyncStore.h"
#include "sync/SyncEngine.h"
#include "sync/SyncStatus.h"

#include <QObject>
#include <QThread>

#include <memory>

namespace immichksync {

/// Composition root. Owns the long-lived objects, wires them together, and exposes the
/// handful of commands the interface can issue.
///
/// Lives on the GUI thread; the engine lives on its own. Everything crossing that
/// boundary goes through queued signals, so no widget ever blocks on a network call and
/// no cycle ever touches a widget.
class AppEnvironment : public QObject {
    Q_OBJECT

public:
    /// `storePath` is a seam for the tests; production uses the default location.
    explicit AppEnvironment(QString storePath = QString(), QObject *parent = nullptr);
    ~AppEnvironment() override;

    Preferences *preferences() { return m_preferences.get(); }
    SyncStatusModel *status() { return m_status.get(); }
    SecretStore *secrets() { return m_secrets.get(); }
    TlsCertificateStore *certificates() { return m_certificates.get(); }
    SyncStore *store() { return m_store.get(); }

    /// Set when the local database could not be opened at all.
    QString fatalStartupError() const { return m_fatalStartupError; }

    bool isPaused() const;
    bool hasStoredApiKey() const { return m_hasStoredApiKey; }
    bool hasStoredSessionToken() const { return m_hasStoredSessionToken; }

    QList<AlbumRecord> albums() const { return m_albums; }
    QList<HeldRemoval> heldRemovals() const { return m_heldRemovals; }
    QList<HeldRemoval> heldRemovalsFor(const QString &albumId) const;

    /// Starts the engine and the system-event observers. Idempotent.
    void start();

    // MARK: - Commands

    void togglePause();
    void syncNow();
    void reconfigureEngine();

    struct ConnectionTestResult {
        std::optional<ServerProfile> profile;
        QString errorMessage;
        bool isSuccess() const { return profile.has_value(); }
    };

    /// Resolves the address, then validates the stored credentials against it.
    ConnectionTestResult testConnection();
    /// Returns an error message, or an empty string on success.
    QString saveApiKey(const QString &key);
    /// Exchanges an email and password for a session token. The password itself is
    /// never written anywhere.
    QString signIn(const QString &email, const QString &password);
    void signOut();

    void setRootFolder(const QString &path);

    // TLS
    TlsCertificateStore::ImportResult importClientCertificate(const QString &filePath,
                                                              const QString &passphrase);
    TlsCertificateStore::ImportResult importCertificateAuthority(const QString &filePath);
    void removeClientCertificate();
    void removeCertificateAuthority();

    // Albums
    void refreshAlbums();
    void setAlbumExcluded(bool excluded, const QString &albumId);
    void applyHeldRemovals(const QString &albumId);
    void restoreHeldRemovals(const QString &albumId);

    // Maintenance
    /// Forgets all reconciliation state. Files on disk and assets on the server are
    /// untouched; the next cycle rediscovers everything from the album markers.
    void resetLocalState();
    void openLogFolder();
    void openSyncFolder();

    bool launchesAtLogin() const;
    /// Returns an error message, or an empty string on success.
    QString setLaunchesAtLogin(bool enabled);

Q_SIGNALS:
    void albumsChanged();
    void credentialStateChanged();
    void certificateStateChanged();
    /// Asks the interface to bring the settings window forward.
    void settingsRequested();
    void quitRequested();

private Q_SLOTS:
    void handleAlbumsChanged();

private:
    void refreshCredentialState();
    /// Publishes the hosts the imported certificate authority may be used for.
    ///
    /// The transport runs on the engine thread and cannot read `Preferences`, so the
    /// host is pushed to it rather than pulled. Both the typed address and the
    /// discovered API base URL are included, because `.well-known/immich` is allowed to
    /// point the API at a different host from the one the user entered.
    void publishTrustedHosts();
    std::optional<ImmichCredentials> currentCredentials() const;
    /// Invokes a slot on the engine across the thread boundary.
    template<typename Function>
    void onEngineThread(Function &&function);

    std::unique_ptr<Preferences> m_preferences;
    std::unique_ptr<SyncStatusModel> m_status;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<TlsCertificateStore> m_certificates;
    std::unique_ptr<SyncStore> m_store;
    std::unique_ptr<NetworkTransport> m_transport;
    std::unique_ptr<UserNotifier> m_notifier;
    std::unique_ptr<SystemEventObserver> m_systemEvents;

    QThread *m_engineThread = nullptr;
    SyncEngine *m_engine = nullptr;

    QString m_fatalStartupError;
    bool m_started = false;
    bool m_hasStoredApiKey = false;
    bool m_hasStoredSessionToken = false;

    QList<AlbumRecord> m_albums;
    QList<HeldRemoval> m_heldRemovals;
};

} // namespace immichksync
