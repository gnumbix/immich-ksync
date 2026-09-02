#pragma once

#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>

namespace immichksync {

/// Posts the small number of alerts that genuinely need the user's attention.
///
/// A background sync agent has exactly two things worth interrupting someone for: it
/// stopped because it needs a decision, or it stopped because it cannot continue.
/// Everything else belongs in the log.
class UserNotifier : public QObject {
    Q_OBJECT

public:
    explicit UserNotifier(QObject *parent = nullptr);

    /// Test seam: records what would have been posted instead of posting it.
    void setDryRun(bool dryRun) { m_dryRun = dryRun; }
    QStringList posted() const;

public Q_SLOTS:
    void postSafetyHold(const QString &albumName, int removals, int tracked);
    void postAuthenticationFailure();
    /// Lets a resolved condition notify again if it recurs.
    void clear(const QString &identifier);

private:
    /// Suppresses repeats, so a condition persisting across cycles is announced once
    /// rather than every five minutes.
    bool claim(const QString &identifier);
    void post(const QString &eventId, const QString &title, const QString &body);

    mutable QMutex m_mutex;
    QSet<QString> m_alreadyPosted;
    QStringList m_posted;
    bool m_dryRun = false;
};

} // namespace immichksync
