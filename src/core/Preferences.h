#pragma once

#include "core/Logging.h"
#include "credentials/ImmichCredentials.h"
#include "sync/SafetyGate.h"

#include <KSharedConfig>

#include <QObject>
#include <QString>
#include <QUrl>

namespace immichksync {

/// Immutable snapshot of everything the sync engine needs, so the engine never
/// reaches back onto the GUI thread mid-cycle.
struct SyncSettings {
    QUrl apiBaseUrl;
    QString serverAddress;
    ImmichAuthMode authMode = ImmichAuthMode::ApiKey;
    /// Absolute path; empty when no folder has been chosen.
    QString rootFolder;
    int syncIntervalSeconds = 300;
    int deepScanIntervalSeconds = 3600;
    int uploadConcurrency = 4;
    int downloadConcurrency = 4;
    int settleWindowSeconds = 5;
    SafetyGate safetyGate;

    bool isConfigured() const { return !apiBaseUrl.isEmpty() && !rootFolder.isEmpty(); }
};

/// User-visible settings, persisted with KConfig in `$XDG_CONFIG_HOME/immichksyncrc`.
/// Never secrets — those live in the Secret Service.
class Preferences : public QObject {
    Q_OBJECT

public:
    explicit Preferences(QObject *parent = nullptr);
    /// Test seam: an isolated config file.
    Preferences(KSharedConfig::Ptr config, QObject *parent = nullptr);

    QString serverAddress() const;
    void setServerAddress(const QString &value);

    /// Cached result of `.well-known/immich` discovery.
    QUrl apiBaseUrl() const;
    void setApiBaseUrl(const QUrl &value);

    ImmichAuthMode authMode() const;
    void setAuthMode(ImmichAuthMode value);

    QString accountEmail() const;
    void setAccountEmail(const QString &value);

    /// Secret Service items are keyed on this, so changing servers cannot silently
    /// reuse the previous account's token.
    QString credentialScope() const;

    /// Absolute path to the sync root, or an empty string when none is set.
    QString rootFolder() const;
    void setRootFolder(const QString &path);

    int syncIntervalSeconds() const;
    void setSyncIntervalSeconds(int value);

    int deepScanIntervalSeconds() const;
    void setDeepScanIntervalSeconds(int value);

    int uploadConcurrency() const;
    void setUploadConcurrency(int value);

    int downloadConcurrency() const;
    void setDownloadConcurrency(int value);

    int settleWindowSeconds() const;
    void setSettleWindowSeconds(int value);

    double removalRatioThreshold() const;
    void setRemovalRatioThreshold(double value);

    int minimumRemovalsBeforeGating() const;
    void setMinimumRemovalsBeforeGating(int value);

    LogLevel logLevel() const;
    void setLogLevel(LogLevel value);

    bool isPaused() const;
    void setPaused(bool value);

    SyncSettings snapshot() const;

Q_SIGNALS:
    void changed();

private:
    KConfigGroup group() const;
    void sync();

    KSharedConfig::Ptr m_config;
};

} // namespace immichksync
