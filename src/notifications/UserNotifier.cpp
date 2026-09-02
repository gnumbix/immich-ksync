#include "notifications/UserNotifier.h"

#include "core/Logging.h"

#include <KNotification>

#include <QMutexLocker>

namespace immichksync {

UserNotifier::UserNotifier(QObject *parent)
    : QObject(parent)
{
}

QStringList UserNotifier::posted() const
{
    QMutexLocker locker(&m_mutex);
    return m_posted;
}

bool UserNotifier::claim(const QString &identifier)
{
    QMutexLocker locker(&m_mutex);
    if (m_alreadyPosted.contains(identifier)) {
        return false;
    }
    m_alreadyPosted.insert(identifier);
    return true;
}

void UserNotifier::clear(const QString &identifier)
{
    QMutexLocker locker(&m_mutex);
    m_alreadyPosted.remove(identifier);
}

void UserNotifier::postSafetyHold(const QString &albumName, int removals, int tracked)
{
    if (!claim(QStringLiteral("safety-hold.%1").arg(albumName))) {
        return;
    }
    post(QStringLiteral("safetyHold"),
         QStringLiteral("Sync paused for “%1”").arg(albumName),
         QStringLiteral("%1 of %2 synced items would be removed. Nothing was changed — review it "
                        "in Settings ▸ Albums.")
             .arg(removals)
             .arg(tracked));
}

void UserNotifier::postAuthenticationFailure()
{
    if (!claim(QStringLiteral("auth-failure"))) {
        return;
    }
    post(QStringLiteral("authenticationFailure"),
         QStringLiteral("ImmichKSync can’t sign in"),
         QStringLiteral("Check the server address and credentials in Settings ▸ Server."));
}

void UserNotifier::post(const QString &eventId, const QString &title, const QString &body)
{
    {
        QMutexLocker locker(&m_mutex);
        m_posted.append(QStringLiteral("%1: %2").arg(title, body));
    }
    log::app.notice(QStringLiteral("Notification — %1: %2").arg(title, body));

    if (m_dryRun) {
        return;
    }

    // CloseOnTimeout rather than Persistent: the settings window is where the decision
    // actually gets made, and a notification that will not go away is its own problem.
    auto *notification = new KNotification(eventId, KNotification::CloseOnTimeout, this);
    notification->setTitle(title);
    notification->setText(body);
    notification->setIconName(QStringLiteral("dialog-warning"));
    notification->sendEvent();
}

} // namespace immichksync
