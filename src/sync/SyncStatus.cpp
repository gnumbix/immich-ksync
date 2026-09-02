#include "sync/SyncStatus.h"

#include <QLocale>

namespace immichksync {

QString SyncState::Progress::verb() const
{
    switch (phase) {
    case Phase::Scanning: return QStringLiteral("Scanning");
    case Phase::Hashing: return QStringLiteral("Hashing");
    case Phase::Downloading: return QStringLiteral("Downloading");
    case Phase::Uploading: return QStringLiteral("Uploading");
    case Phase::UpdatingAlbums: return QStringLiteral("Updating albums");
    }
    return {};
}

QString SyncState::Progress::description() const
{
    if (total <= 0) {
        return QStringLiteral("%1…").arg(verb());
    }
    return QStringLiteral("%1 %2 of %3…").arg(verb()).arg(completed + 1).arg(total);
}

bool SyncState::Progress::operator==(const Progress &other) const
{
    return phase == other.phase && completed == other.completed && total == other.total
        && currentItem == other.currentItem;
}

SyncState SyncState::notConfigured(const QString &reason)
{
    return {Kind::NotConfigured, reason, {}};
}

SyncState SyncState::paused()
{
    return {Kind::Paused, {}, {}};
}

SyncState SyncState::idle()
{
    return {Kind::Idle, {}, {}};
}

SyncState SyncState::preparing()
{
    return {Kind::Preparing, {}, {}};
}

SyncState SyncState::working(const Progress &progress)
{
    return {Kind::Working, {}, progress};
}

SyncState SyncState::failed(const QString &message)
{
    return {Kind::Failed, message, {}};
}

QString SyncState::iconName() const
{
    // Breeze names, so the tray follows the user's icon theme rather than shipping art.
    switch (kind) {
    case Kind::NotConfigured: return QStringLiteral("dialog-information");
    case Kind::Paused: return QStringLiteral("media-playback-pause");
    case Kind::Idle: return QStringLiteral("cloud-upload");
    case Kind::Preparing:
    case Kind::Working: return QStringLiteral("view-refresh");
    case Kind::Failed: return QStringLiteral("dialog-error");
    }
    return QStringLiteral("cloud-upload");
}

bool SyncState::operator==(const SyncState &other) const
{
    return kind == other.kind && message == other.message && progress == other.progress;
}

bool SyncCycleSummary::didChangeAnything() const
{
    return uploaded + downloaded + removedFromAlbums + movedToTrash + albumsCreated > 0;
}

QString SyncCycleSummary::headline() const
{
    if (!didChangeAnything()) {
        return QStringLiteral("Up to date");
    }
    QStringList parts;
    if (downloaded > 0) {
        parts << QStringLiteral("%1 downloaded").arg(downloaded);
    }
    if (uploaded > 0) {
        parts << QStringLiteral("%1 uploaded").arg(uploaded);
    }
    if (removedFromAlbums > 0) {
        parts << QStringLiteral("%1 removed").arg(removedFromAlbums);
    }
    if (movedToTrash > 0) {
        parts << QStringLiteral("%1 trashed").arg(movedToTrash);
    }
    return parts.join(QStringLiteral(", "));
}

// MARK: - SyncStatusModel

SyncStatusModel::SyncStatusModel(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<SyncState>("immichksync::SyncState");
    qRegisterMetaType<SyncCycleSummary>("immichksync::SyncCycleSummary");
    qRegisterMetaType<SyncStore::Statistics>("immichksync::SyncStore::Statistics");
}

void SyncStatusModel::setState(const SyncState &state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT changed();
}

void SyncStatusModel::setLastSummary(const SyncCycleSummary &summary)
{
    m_lastSummary = summary;
    Q_EMIT changed();
}

void SyncStatusModel::setLastErrorMessage(const QString &message)
{
    if (m_lastErrorMessage == message) {
        return;
    }
    m_lastErrorMessage = message;
    Q_EMIT changed();
}

void SyncStatusModel::setStatistics(const SyncStore::Statistics &statistics)
{
    if (m_statistics == statistics) {
        return;
    }
    m_statistics = statistics;
    Q_EMIT changed();
}

void SyncStatusModel::setAlbumsOnSafetyHold(const QStringList &names)
{
    if (m_albumsOnSafetyHold == names) {
        return;
    }
    m_albumsOnSafetyHold = names;
    Q_EMIT changed();
}

void SyncStatusModel::setServerProfile(const std::optional<ServerProfile> &profile)
{
    m_serverProfile = profile;
    Q_EMIT changed();
}

QString SyncStatusModel::menuStatusLine() const
{
    switch (m_state.kind) {
    case SyncState::Kind::NotConfigured:
    case SyncState::Kind::Failed:
        return m_state.message;
    case SyncState::Kind::Paused:
        return QStringLiteral("Paused");
    case SyncState::Kind::Preparing:
        return QStringLiteral("Checking for changes…");
    case SyncState::Kind::Working:
        return m_state.progress.description();
    case SyncState::Kind::Idle:
        break;
    }

    if (!m_albumsOnSafetyHold.isEmpty()) {
        return m_albumsOnSafetyHold.size() == 1
            ? QStringLiteral("Review needed: %1").arg(m_albumsOnSafetyHold.first())
            : QStringLiteral("Review needed in %1 albums").arg(m_albumsOnSafetyHold.size());
    }
    if (m_statistics.albumCount == 0) {
        return QStringLiteral("No albums synced yet");
    }

    const QLocale locale;
    return QStringLiteral("%1 item%2 in %3 album%4")
        .arg(locale.toString(m_statistics.syncedAssetCount),
             m_statistics.syncedAssetCount == 1 ? QString() : QStringLiteral("s"),
             locale.toString(m_statistics.albumCount),
             m_statistics.albumCount == 1 ? QString() : QStringLiteral("s"));
}

bool SyncStatusModel::needsAttention() const
{
    switch (m_state.kind) {
    case SyncState::Kind::Failed:
    case SyncState::Kind::NotConfigured:
        return true;
    default:
        return !m_albumsOnSafetyHold.isEmpty();
    }
}

QString SyncStatusModel::trayIconName() const
{
    return needsAttention() ? QStringLiteral("dialog-warning") : m_state.iconName();
}

} // namespace immichksync
