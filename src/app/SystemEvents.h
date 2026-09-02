#pragma once

#include <QObject>

namespace immichksync {

/// Nudges the sync engine when the machine's circumstances change.
///
/// A five-minute timer is fine for steady state but wrong at the two moments a user
/// notices most: the lid opening, and the network coming back. Both are cheap to
/// observe and turn a "why hasn't it synced yet" into an immediate cycle.
class SystemEventObserver : public QObject {
    Q_OBJECT

public:
    explicit SystemEventObserver(QObject *parent = nullptr);

    void start();
    void stop();

Q_SIGNALS:
    void wokeFromSleep();
    void networkBecameReachable();

private Q_SLOTS:
    void handlePrepareForSleep(bool aboutToSleep);
    void handleReachabilityChanged();

private:
    bool m_started = false;
    /// Only the offline → online edge is interesting; a reachable network that stays
    /// reachable says nothing new.
    bool m_wasOffline = false;
};

} // namespace immichksync
