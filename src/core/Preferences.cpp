#include "core/Preferences.h"

#include <KConfigGroup>

#include <algorithm>

namespace immichksync {

namespace {

constexpr const char *kGroup = "General";

// Key names are deliberately the same strings the macOS build uses in UserDefaults,
// so the two implementations stay legible side by side.
constexpr const char *kServerAddress = "serverAddress";
constexpr const char *kApiBaseUrl = "apiBaseURL";
constexpr const char *kAuthMode = "authMode";
constexpr const char *kAccountEmail = "accountEmail";
constexpr const char *kRootFolder = "rootFolder";
constexpr const char *kSyncInterval = "syncIntervalSeconds";
constexpr const char *kDeepScanInterval = "deepScanIntervalSeconds";
constexpr const char *kUploadConcurrency = "uploadConcurrency";
constexpr const char *kDownloadConcurrency = "downloadConcurrency";
constexpr const char *kSettleWindow = "writeSettleSeconds";
constexpr const char *kRemovalRatio = "removalRatioThreshold";
constexpr const char *kMinimumRemovals = "minimumRemovalsBeforeGating";
constexpr const char *kLogLevel = "logLevel";
constexpr const char *kIsPaused = "isPaused";

template<typename T>
T clamp(T value, T low, T high)
{
    return std::min(std::max(value, low), high);
}

} // namespace

Preferences::Preferences(QObject *parent)
    : Preferences(KSharedConfig::openConfig(QStringLiteral("immichksyncrc")), parent)
{
}

Preferences::Preferences(KSharedConfig::Ptr config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
{
    LogSink::instance().setMinimumLevel(logLevel());
}

KConfigGroup Preferences::group() const
{
    return m_config->group(QLatin1String(kGroup));
}

void Preferences::sync()
{
    m_config->sync();
    Q_EMIT changed();
}

QString Preferences::serverAddress() const
{
    return group().readEntry(kServerAddress, QString());
}

void Preferences::setServerAddress(const QString &value)
{
    group().writeEntry(kServerAddress, value);
    sync();
}

QUrl Preferences::apiBaseUrl() const
{
    const QString raw = group().readEntry(kApiBaseUrl, QString());
    return raw.isEmpty() ? QUrl() : QUrl(raw);
}

void Preferences::setApiBaseUrl(const QUrl &value)
{
    group().writeEntry(kApiBaseUrl, value.isEmpty() ? QString() : value.toString());
    sync();
}

ImmichAuthMode Preferences::authMode() const
{
    return authModeFromString(group().readEntry(kAuthMode, QStringLiteral("apiKey")));
}

void Preferences::setAuthMode(ImmichAuthMode value)
{
    group().writeEntry(kAuthMode, keyFor(value));
    sync();
}

QString Preferences::accountEmail() const
{
    return group().readEntry(kAccountEmail, QString());
}

void Preferences::setAccountEmail(const QString &value)
{
    group().writeEntry(kAccountEmail, value);
    sync();
}

QString Preferences::credentialScope() const
{
    const QString address = serverAddress().trimmed();
    if (!address.isEmpty()) {
        return address;
    }
    const QUrl base = apiBaseUrl();
    return base.isEmpty() ? QStringLiteral("unconfigured") : base.toString();
}

QString Preferences::rootFolder() const
{
    return group().readEntry(kRootFolder, QString());
}

void Preferences::setRootFolder(const QString &path)
{
    group().writeEntry(kRootFolder, path);
    sync();
}

int Preferences::syncIntervalSeconds() const
{
    return clamp(group().readEntry(kSyncInterval, 300), 60, 86400);
}

void Preferences::setSyncIntervalSeconds(int value)
{
    group().writeEntry(kSyncInterval, clamp(value, 60, 86400));
    sync();
}

int Preferences::deepScanIntervalSeconds() const
{
    return clamp(group().readEntry(kDeepScanInterval, 3600), 300, 604800);
}

void Preferences::setDeepScanIntervalSeconds(int value)
{
    group().writeEntry(kDeepScanInterval, clamp(value, 300, 604800));
    sync();
}

int Preferences::uploadConcurrency() const
{
    return clamp(group().readEntry(kUploadConcurrency, 4), 1, 16);
}

void Preferences::setUploadConcurrency(int value)
{
    group().writeEntry(kUploadConcurrency, clamp(value, 1, 16));
    sync();
}

int Preferences::downloadConcurrency() const
{
    return clamp(group().readEntry(kDownloadConcurrency, 4), 1, 16);
}

void Preferences::setDownloadConcurrency(int value)
{
    group().writeEntry(kDownloadConcurrency, clamp(value, 1, 16));
    sync();
}

int Preferences::settleWindowSeconds() const
{
    return clamp(group().readEntry(kSettleWindow, 5), 1, 120);
}

void Preferences::setSettleWindowSeconds(int value)
{
    group().writeEntry(kSettleWindow, clamp(value, 1, 120));
    sync();
}

double Preferences::removalRatioThreshold() const
{
    return clamp(group().readEntry(kRemovalRatio, 0.25), 0.01, 1.0);
}

void Preferences::setRemovalRatioThreshold(double value)
{
    group().writeEntry(kRemovalRatio, clamp(value, 0.01, 1.0));
    sync();
}

int Preferences::minimumRemovalsBeforeGating() const
{
    return clamp(group().readEntry(kMinimumRemovals, 10), 0, 10000);
}

void Preferences::setMinimumRemovalsBeforeGating(int value)
{
    group().writeEntry(kMinimumRemovals, clamp(value, 0, 10000));
    sync();
}

LogLevel Preferences::logLevel() const
{
    const int raw = clamp(group().readEntry(kLogLevel, static_cast<int>(LogLevel::Info)),
                          static_cast<int>(LogLevel::Debug),
                          static_cast<int>(LogLevel::Error));
    return static_cast<LogLevel>(raw);
}

void Preferences::setLogLevel(LogLevel value)
{
    group().writeEntry(kLogLevel, static_cast<int>(value));
    LogSink::instance().setMinimumLevel(value);
    sync();
}

bool Preferences::isPaused() const
{
    return group().readEntry(kIsPaused, false);
}

void Preferences::setPaused(bool value)
{
    group().writeEntry(kIsPaused, value);
    sync();
}

SyncSettings Preferences::snapshot() const
{
    SyncSettings settings;
    settings.apiBaseUrl = apiBaseUrl();
    settings.serverAddress = serverAddress();
    settings.authMode = authMode();
    settings.rootFolder = rootFolder();
    settings.syncIntervalSeconds = syncIntervalSeconds();
    settings.deepScanIntervalSeconds = deepScanIntervalSeconds();
    settings.uploadConcurrency = uploadConcurrency();
    settings.downloadConcurrency = downloadConcurrency();
    settings.settleWindowSeconds = settleWindowSeconds();
    settings.safetyGate.removalRatioThreshold = removalRatioThreshold();
    settings.safetyGate.minimumRemovalsBeforeGating = minimumRemovalsBeforeGating();
    return settings;
}

} // namespace immichksync
