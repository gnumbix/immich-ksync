#include "app/AppEnvironment.h"

#include "app/AppInfo.h"
#include "core/Logging.h"
#include "filesystem/RootFolderAccess.h"
#include "immich/ServerDiscovery.h"

#include <KIO/OpenFileManagerWindowJob>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>

namespace immichksync {

AppEnvironment::AppEnvironment(QString storePath, QObject *parent)
    : QObject(parent)
    , m_preferences(std::make_unique<Preferences>())
    , m_status(std::make_unique<SyncStatusModel>())
    , m_secrets(std::make_unique<SecretStore>())
    , m_certificates(std::make_unique<TlsCertificateStore>(m_secrets.get()))
    , m_store(std::make_unique<SyncStore>())
    , m_transport(std::make_unique<NetworkTransport>(m_certificates.get()))
    , m_notifier(std::make_unique<UserNotifier>())
    , m_systemEvents(std::make_unique<SystemEventObserver>())
{
    const QString path = storePath.isEmpty() ? SyncStore::defaultPath() : storePath;
    QString error;
    if (!m_store->open(path, &error)) {
        m_fatalStartupError = error;
        log::app.error(QStringLiteral("Could not open the local database: %1").arg(error));
        m_status->setState(
            SyncState::failed(QStringLiteral("Local database unavailable: %1").arg(error)));
        return;
    }

    // The engine owns a thread of its own so a multi-gigabyte transfer can never make
    // the tray menu or the settings window stop responding.
    m_engineThread = new QThread(this);
    m_engineThread->setObjectName(QStringLiteral("immichksync-sync"));
    m_engine = new SyncEngine(m_store.get(), m_transport.get(), m_notifier.get());
    m_engine->moveToThread(m_engineThread);
    connect(m_engineThread, &QThread::finished, m_engine, &QObject::deleteLater);

    connect(m_engine, &SyncEngine::stateChanged, m_status.get(), &SyncStatusModel::setState);
    connect(m_engine, &SyncEngine::statisticsChanged, m_status.get(), &SyncStatusModel::setStatistics);
    connect(m_engine,
            &SyncEngine::safetyHoldsChanged,
            m_status.get(),
            &SyncStatusModel::setAlbumsOnSafetyHold);
    connect(m_engine,
            &SyncEngine::errorMessageChanged,
            m_status.get(),
            &SyncStatusModel::setLastErrorMessage);
    connect(m_engine,
            &SyncEngine::serverProfileChanged,
            m_status.get(),
            &SyncStatusModel::setServerProfile);
    connect(m_engine, &SyncEngine::cycleFinished, m_status.get(), &SyncStatusModel::setLastSummary);
    connect(m_engine, &SyncEngine::albumsChanged, this, &AppEnvironment::handleAlbumsChanged);

    connect(m_systemEvents.get(), &SystemEventObserver::wokeFromSleep, this, [this]() {
        onEngineThread([this]() { m_engine->trigger(SyncTrigger::WokeFromSleep); });
    });
    connect(m_systemEvents.get(), &SystemEventObserver::networkBecameReachable, this, [this]() {
        onEngineThread([this]() { m_engine->trigger(SyncTrigger::Scheduled); });
    });

    m_engineThread->start();
}

AppEnvironment::~AppEnvironment()
{
    if (!m_engineThread) {
        return;
    }

    // Cancel before asking politely. A queued stop() is not looked at until the cycle
    // occupying the thread finishes, and a BlockingQueuedConnection here would wait on
    // a thread that is busy by definition — which is how the app used to outlive its
    // own Quit, tray icon gone and process still running.
    if (m_transport) {
        m_transport->cancelAll();
    }
    if (m_engine) {
        m_engine->requestStop();
        QMetaObject::invokeMethod(m_engine, "stop", Qt::QueuedConnection);
    }

    m_engineThread->quit();
    if (!m_engineThread->wait(10000)) {
        // Every transfer has been aborted and every loop asked to end, so reaching here
        // means something is genuinely wedged. Exiting beats staying resident.
        log::app.warning(QStringLiteral("The sync thread did not stop in time; exiting anyway."));
        m_engineThread->terminate();
        m_engineThread->wait(1000);
    }
}

template<typename Function>
void AppEnvironment::onEngineThread(Function &&function)
{
    if (!m_engine) {
        return;
    }
    QMetaObject::invokeMethod(m_engine, std::forward<Function>(function), Qt::QueuedConnection);
}

// MARK: - Lifecycle

void AppEnvironment::start()
{
    if (m_started || !m_engine) {
        return;
    }
    m_started = true;
    log::app.info(QStringLiteral("Starting up (paused: %1)")
                      .arg(m_preferences->isPaused() ? QStringLiteral("yes") : QStringLiteral("no")));

    publishTrustedHosts();
    refreshCredentialState();
    m_systemEvents->start();

    const SyncSettings settings = m_preferences->snapshot();
    const auto credentials = currentCredentials();
    onEngineThread([this, settings, credentials]() {
        m_engine->applyConfiguration(settings, credentials);
    });

    if (m_preferences->isPaused()) {
        m_status->setState(SyncState::paused());
        log::app.notice(QStringLiteral("Synchronisation is paused"));
    } else if (settings.isConfigured() && credentials) {
        onEngineThread([this]() { m_engine->start(); });
    } else {
        // A background agent that silently does nothing is the hardest kind of bug to
        // diagnose, so it says out loud what is missing.
        QString reason;
        if (settings.apiBaseUrl.isEmpty()) {
            reason = QStringLiteral("Enter your Immich server address in Settings.");
        } else if (!credentials) {
            reason = QStringLiteral("Sign in to your Immich server in Settings.");
        } else {
            reason = QStringLiteral("Choose a sync folder in Settings.");
        }
        m_status->setState(SyncState::notConfigured(reason));
        log::app.notice(QStringLiteral("Not syncing yet — %1").arg(reason));
    }

    refreshAlbums();
}

bool AppEnvironment::isPaused() const
{
    return m_preferences->isPaused();
}

std::optional<ImmichCredentials> AppEnvironment::currentCredentials() const
{
    return m_secrets->credentials(m_preferences->credentialScope(), m_preferences->authMode());
}

void AppEnvironment::refreshCredentialState()
{
    const QString scope = m_preferences->credentialScope();
    const auto apiKey = m_secrets->read(scope, SecretStore::Slot::ApiKey);
    const auto token = m_secrets->read(scope, SecretStore::Slot::SessionToken);
    m_hasStoredApiKey = apiKey && !apiKey->isEmpty();
    m_hasStoredSessionToken = token && !token->isEmpty();
    Q_EMIT credentialStateChanged();
}

void AppEnvironment::publishTrustedHosts()
{
    QSet<QString> hosts;
    if (const auto origin = ServerDiscovery::normalizedOrigin(m_preferences->serverAddress())) {
        hosts.insert(origin->host());
    }
    const QUrl base = m_preferences->apiBaseUrl();
    if (!base.isEmpty()) {
        hosts.insert(base.host());
    }
    m_certificates->setTrustedHosts(hosts);
}

// MARK: - Commands

void AppEnvironment::togglePause()
{
    const bool nowPaused = !m_preferences->isPaused();
    m_preferences->setPaused(nowPaused);

    if (nowPaused) {
        onEngineThread([this]() { m_engine->stop(); });
        m_status->setState(SyncState::paused());
        log::app.notice(QStringLiteral("Synchronisation paused"));
    } else {
        log::app.notice(QStringLiteral("Synchronisation resumed"));
        reconfigureEngine();
        onEngineThread([this]() {
            m_engine->start();
            m_engine->trigger(SyncTrigger::Manual);
        });
    }
}

void AppEnvironment::syncNow()
{
    if (m_preferences->isPaused()) {
        return;
    }
    onEngineThread([this]() { m_engine->trigger(SyncTrigger::Manual); });
}

void AppEnvironment::reconfigureEngine()
{
    publishTrustedHosts();
    refreshCredentialState();

    const SyncSettings settings = m_preferences->snapshot();
    const auto credentials = currentCredentials();
    onEngineThread([this, settings, credentials]() {
        m_engine->applyConfiguration(settings, credentials);
    });

    if (!settings.isConfigured() || !credentials) {
        onEngineThread([this]() { m_engine->stop(); });
        m_status->setState(m_preferences->isPaused()
                               ? SyncState::paused()
                               : SyncState::notConfigured(
                                     QStringLiteral("Finish setup in Settings.")));
    } else if (!m_preferences->isPaused()) {
        onEngineThread([this]() { m_engine->start(); });
    }
    refreshAlbums();
}

AppEnvironment::ConnectionTestResult AppEnvironment::testConnection()
{
    ConnectionTestResult result;

    // Runs against whatever address the user has just typed, before `apiBaseUrl` is
    // set, so the anchor has to be published before the first request goes out.
    publishTrustedHosts();

    const auto credentials = currentCredentials();
    if (!credentials) {
        result.errorMessage = m_preferences->authMode() == ImmichAuthMode::ApiKey
            ? QStringLiteral("Enter an API key first.")
            : QStringLiteral("Sign in with your email and password first.");
        return result;
    }

    const auto baseUrl =
        ServerDiscovery::resolveApiBaseUrl(m_preferences->serverAddress(), m_transport.get());
    if (!baseUrl) {
        result.errorMessage =
            ImmichError::invalidServerUrl(m_preferences->serverAddress()).message();
        return result;
    }

    const auto profile = ServerDiscovery::probe(*baseUrl, *credentials, m_transport.get());
    if (!profile.succeeded()) {
        result.errorMessage = profile.error.message();
        return result;
    }

    m_preferences->setApiBaseUrl(*baseUrl);
    m_status->setServerProfile(*profile);
    reconfigureEngine();
    result.profile = *profile;
    return result;
}

QString AppEnvironment::saveApiKey(const QString &key)
{
    const QString trimmed = key.trimmed();
    const QString scope = m_preferences->credentialScope();

    if (trimmed.isEmpty()) {
        m_secrets->remove(scope, SecretStore::Slot::ApiKey);
    } else {
        QString error;
        if (!m_secrets->write(trimmed, scope, SecretStore::Slot::ApiKey, &error)) {
            return error;
        }
    }
    reconfigureEngine();
    return {};
}

QString AppEnvironment::signIn(const QString &email, const QString &password)
{
    publishTrustedHosts();

    const auto baseUrl =
        ServerDiscovery::resolveApiBaseUrl(m_preferences->serverAddress(), m_transport.get());
    if (!baseUrl) {
        return ImmichError::invalidServerUrl(m_preferences->serverAddress()).message();
    }

    ImmichClient client(*baseUrl, std::nullopt, m_transport.get());
    const auto response = client.login(email, password);
    if (!response.succeeded()) {
        return response.error.message();
    }

    QString error;
    if (!m_secrets->write(response->accessToken,
                          m_preferences->credentialScope(),
                          SecretStore::Slot::SessionToken,
                          &error)) {
        return error;
    }

    m_preferences->setApiBaseUrl(*baseUrl);
    m_preferences->setAccountEmail(response->userEmail);
    m_preferences->setAuthMode(ImmichAuthMode::Password);
    reconfigureEngine();
    log::credentials.notice(QStringLiteral("Signed in as %1").arg(response->userEmail));
    return {};
}

void AppEnvironment::signOut()
{
    m_secrets->removeAll(m_preferences->credentialScope());
    m_status->setServerProfile(std::nullopt);
    reconfigureEngine();
    log::credentials.notice(QStringLiteral("Signed out"));
}

void AppEnvironment::setRootFolder(const QString &path)
{
    m_preferences->setRootFolder(path);
    log::app.notice(QStringLiteral("Sync folder set to %1").arg(path));
    reconfigureEngine();
}

// MARK: - TLS

TlsCertificateStore::ImportResult
AppEnvironment::importClientCertificate(const QString &filePath, const QString &passphrase)
{
    TlsCertificateStore::ImportResult result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = CertificateImportError::unreadableFile(QFileInfo(filePath).fileName(),
                                                              file.errorString());
        return result;
    }

    result = m_certificates->importClientCertificate(file.readAll(), passphrase);
    if (result.succeeded()) {
        log::credentials.notice(
            QStringLiteral("Imported the client certificate %1").arg(result.summary->commonName));
        m_transport->resetConnections();
        Q_EMIT certificateStateChanged();
        reconfigureEngine();
    }
    return result;
}

TlsCertificateStore::ImportResult AppEnvironment::importCertificateAuthority(const QString &filePath)
{
    TlsCertificateStore::ImportResult result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = CertificateImportError::unreadableFile(QFileInfo(filePath).fileName(),
                                                              file.errorString());
        return result;
    }

    result = m_certificates->importCertificateAuthority(file.readAll());
    if (result.succeeded()) {
        log::credentials.notice(
            QStringLiteral("Imported the certificate authority %1").arg(result.summary->commonName));
        m_transport->resetConnections();
        Q_EMIT certificateStateChanged();
        reconfigureEngine();
    }
    return result;
}

void AppEnvironment::removeClientCertificate()
{
    m_certificates->removeClientCertificate();
    m_transport->resetConnections();
    log::credentials.notice(QStringLiteral("Removed the client certificate"));
    Q_EMIT certificateStateChanged();
    reconfigureEngine();
}

void AppEnvironment::removeCertificateAuthority()
{
    m_certificates->removeCertificateAuthority();
    m_transport->resetConnections();
    log::credentials.notice(QStringLiteral("Removed the certificate authority"));
    Q_EMIT certificateStateChanged();
    reconfigureEngine();
}

// MARK: - Albums

void AppEnvironment::handleAlbumsChanged()
{
    refreshAlbums();
}

void AppEnvironment::refreshAlbums()
{
    if (!m_store->isOpen()) {
        return;
    }
    m_albums = m_store->albums();
    m_heldRemovals = m_store->heldRemovals();
    m_status->setStatistics(m_store->statistics());
    Q_EMIT albumsChanged();
}

QList<HeldRemoval> AppEnvironment::heldRemovalsFor(const QString &albumId) const
{
    QList<HeldRemoval> matching;
    for (const HeldRemoval &removal : m_heldRemovals) {
        if (removal.albumId == albumId) {
            matching.append(removal);
        }
    }
    return matching;
}

void AppEnvironment::setAlbumExcluded(bool excluded, const QString &albumId)
{
    m_store->setExcluded(excluded, albumId);
    refreshAlbums();
    syncNow();
}

void AppEnvironment::applyHeldRemovals(const QString &albumId)
{
    onEngineThread([this, albumId]() { m_engine->applyHeldRemovals(albumId); });
}

void AppEnvironment::restoreHeldRemovals(const QString &albumId)
{
    onEngineThread([this, albumId]() { m_engine->restoreHeldRemovals(albumId); });
}

// MARK: - Maintenance

void AppEnvironment::resetLocalState()
{
    onEngineThread([this]() { m_engine->stop(); });
    m_store->reset();
    refreshAlbums();
    log::app.notice(QStringLiteral("Local sync state reset by the user"));
    if (!m_preferences->isPaused()) {
        onEngineThread([this]() { m_engine->start(); });
    }
}

void AppEnvironment::openLogFolder()
{
    KIO::highlightInFileManager({QUrl::fromLocalFile(LogSink::filePath())});
}

void AppEnvironment::openSyncFolder()
{
    const QString root = m_preferences->rootFolder();
    if (root.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(root));
}

// MARK: - Launch at login

bool AppEnvironment::launchesAtLogin() const
{
    return QFileInfo::exists(AppInfo::autostartFilePath());
}

QString AppEnvironment::setLaunchesAtLogin(bool enabled)
{
    const QString path = AppInfo::autostartFilePath();

    if (!enabled) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            return QStringLiteral("Could not remove %1").arg(path);
        }
        return {};
    }

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return QStringLiteral("Could not create %1").arg(QFileInfo(path).absolutePath());
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return file.errorString();
    }
    // Written rather than symlinked to the installed entry: the installed one is
    // visible in the application menu, and this one must not be.
    const QString contents = QStringLiteral("[Desktop Entry]\n"
                                            "Type=Application\n"
                                            "Name=%1\n"
                                            "Comment=Keep a folder and your Immich albums in sync\n"
                                            "Exec=%2\n"
                                            "Icon=%3\n"
                                            "Terminal=false\n"
                                            "NoDisplay=true\n"
                                            "X-GNOME-Autostart-enabled=true\n")
                                 .arg(AppInfo::displayName(),
                                      QCoreApplication::applicationFilePath(),
                                      AppInfo::applicationId());
    file.write(contents.toUtf8());
    file.close();
    return {};
}

} // namespace immichksync
