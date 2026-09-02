#include "app/SystemEvents.h"

#include "core/Logging.h"

#include <QDBusConnection>
#include <QNetworkInformation>

namespace immichksync {

namespace {

constexpr const char *kLogin1Service = "org.freedesktop.login1";
constexpr const char *kLogin1Path = "/org/freedesktop/login1";
constexpr const char *kLogin1Manager = "org.freedesktop.login1.Manager";

} // namespace

SystemEventObserver::SystemEventObserver(QObject *parent)
    : QObject(parent)
{
}

void SystemEventObserver::start()
{
    if (m_started) {
        return;
    }
    m_started = true;

    // logind emits PrepareForSleep(true) before suspending and (false) after resuming.
    // The false edge is the one worth reacting to.
    const bool connected =
        QDBusConnection::systemBus().connect(QLatin1String(kLogin1Service),
                                             QLatin1String(kLogin1Path),
                                             QLatin1String(kLogin1Manager),
                                             QStringLiteral("PrepareForSleep"),
                                             this,
                                             SLOT(handlePrepareForSleep(bool)));
    if (!connected) {
        log::app.info(QStringLiteral("Could not subscribe to logind sleep notifications; the "
                                     "interval timer will cover waking from sleep."));
    }

    if (QNetworkInformation::loadDefaultBackend() && QNetworkInformation::instance()) {
        auto *information = QNetworkInformation::instance();
        m_wasOffline = information->reachability() != QNetworkInformation::Reachability::Online;
        connect(information,
                &QNetworkInformation::reachabilityChanged,
                this,
                &SystemEventObserver::handleReachabilityChanged);
    } else {
        log::app.info(QStringLiteral("No network status backend is available; the interval timer "
                                     "will cover the network coming back."));
    }
}

void SystemEventObserver::stop()
{
    if (!m_started) {
        return;
    }
    QDBusConnection::systemBus().disconnect(QLatin1String(kLogin1Service),
                                            QLatin1String(kLogin1Path),
                                            QLatin1String(kLogin1Manager),
                                            QStringLiteral("PrepareForSleep"),
                                            this,
                                            SLOT(handlePrepareForSleep(bool)));
    if (QNetworkInformation::instance()) {
        disconnect(QNetworkInformation::instance(), nullptr, this, nullptr);
    }
    m_started = false;
}

void SystemEventObserver::handlePrepareForSleep(bool aboutToSleep)
{
    if (aboutToSleep) {
        return;
    }
    log::app.info(QStringLiteral("Woke from sleep; scheduling a sync"));
    Q_EMIT wokeFromSleep();
}

void SystemEventObserver::handleReachabilityChanged()
{
    auto *information = QNetworkInformation::instance();
    if (!information) {
        return;
    }
    const bool isOffline = information->reachability() != QNetworkInformation::Reachability::Online;
    const bool wasOffline = m_wasOffline;
    m_wasOffline = isOffline;

    if (!wasOffline || isOffline) {
        return;
    }
    log::app.info(QStringLiteral("Network reachable again; scheduling a sync"));
    Q_EMIT networkBecameReachable();
}

} // namespace immichksync
