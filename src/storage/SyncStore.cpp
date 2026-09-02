#include "storage/SyncStore.h"

#include "core/Logging.h"
#include "storage/Migrations.h"

#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTimeZone>

namespace immichksync {

namespace {

/// Dates are stored as a Unix timestamp in seconds, in a REAL column — the same
/// representation the macOS build writes, so either implementation reads the other's
/// database without a conversion pass.
double toStorage(const QDateTime &value)
{
    return value.isValid() ? static_cast<double>(value.toMSecsSinceEpoch()) / 1000.0 : 0.0;
}

QDateTime fromStorage(double value)
{
    if (value == 0.0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(value * 1000.0), QTimeZone::UTC);
}

AlbumRecord decodeAlbum(const SqliteStatement &row)
{
    AlbumRecord album;
    album.albumId = row.columnText(0);
    album.albumName = row.columnText(1);
    album.folderName = row.columnText(2);
    album.hasRemoteUpdatedAt = !row.columnIsNull(3);
    album.remoteUpdatedAt = row.columnText(3);
    album.remoteAssetCount = row.columnInt(4);
    album.lastDeepScanAt = row.columnIsNull(5) ? QDateTime() : fromStorage(row.columnDouble(5));
    album.lastSyncedAt = row.columnIsNull(6) ? QDateTime() : fromStorage(row.columnDouble(6));
    album.isExcluded = row.columnBool(7);
    album.hasSafetyHold = row.columnBool(8);
    return album;
}

constexpr const char *kAlbumColumns =
    "album_id, album_name, folder_name, remote_updated_at, remote_asset_count, "
    "last_deep_scan_at, last_synced_at, is_excluded, safety_hold";

} // namespace

bool SyncStore::Statistics::operator==(const Statistics &other) const
{
    return albumCount == other.albumCount && syncedAssetCount == other.syncedAssetCount
        && syncedByteCount == other.syncedByteCount && heldRemovalCount == other.heldRemovalCount
        && failureCount == other.failureCount;
}

QString SyncStore::defaultPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(QStringLiteral("immichksync/sync-state.sqlite"));
}

bool SyncStore::open(const QString &path, QString *errorMessage)
{
    if (!m_database.open(path, errorMessage)) {
        return false;
    }
    if (!SchemaMigrations::apply(m_database, errorMessage)) {
        return false;
    }
    log::storage.info(QStringLiteral("Opened sync state at %1 (schema v%2)")
                          .arg(path)
                          .arg(m_database.schemaVersion()));
    return true;
}

// MARK: - Albums

QList<AlbumRecord> SyncStore::albums()
{
    QList<AlbumRecord> result;
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("SELECT %1 FROM album ORDER BY album_name COLLATE NOCASE")
            .arg(QLatin1String(kAlbumColumns)));
    if (!statement.isValid()) {
        return result;
    }
    while (statement.next()) {
        result.append(decodeAlbum(statement));
    }
    return result;
}

std::optional<AlbumRecord> SyncStore::album(const QString &albumId)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("SELECT %1 FROM album WHERE album_id = ?").arg(QLatin1String(kAlbumColumns)));
    if (!statement.isValid()) {
        return std::nullopt;
    }
    statement.bind(1, albumId);
    if (!statement.next()) {
        return std::nullopt;
    }
    return decodeAlbum(statement);
}

std::optional<AlbumRecord> SyncStore::albumByFolderName(const QString &folderName)
{
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("SELECT %1 FROM album WHERE folder_name = ?")
                               .arg(QLatin1String(kAlbumColumns)));
    if (!statement.isValid()) {
        return std::nullopt;
    }
    statement.bind(1, folderName);
    if (!statement.next()) {
        return std::nullopt;
    }
    return decodeAlbum(statement);
}

bool SyncStore::upsert(const AlbumRecord &album, QString *errorMessage)
{
    // Columns that later cycles refine are only overwritten when the caller actually
    // knows something: COALESCE keeps a recorded deep scan or sync time rather than
    // letting a partial upsert erase it.
    SqliteStatement statement = m_database.prepare(QStringLiteral(R"(
        INSERT INTO album (album_id, album_name, folder_name, remote_updated_at,
                           remote_asset_count, last_deep_scan_at, last_synced_at,
                           is_excluded, safety_hold)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (album_id) DO UPDATE SET
            album_name = excluded.album_name,
            folder_name = excluded.folder_name,
            remote_updated_at = COALESCE(excluded.remote_updated_at, album.remote_updated_at),
            remote_asset_count = excluded.remote_asset_count,
            last_deep_scan_at = COALESCE(excluded.last_deep_scan_at, album.last_deep_scan_at),
            last_synced_at = COALESCE(excluded.last_synced_at, album.last_synced_at),
            is_excluded = excluded.is_excluded,
            safety_hold = excluded.safety_hold
    )"),
                                                   errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, album.albumId);
    statement.bind(2, album.albumName);
    statement.bind(3, album.folderName);
    if (album.hasRemoteUpdatedAt) {
        statement.bind(4, album.remoteUpdatedAt);
    } else {
        statement.bindNull(4);
    }
    statement.bind(5, album.remoteAssetCount);
    if (album.lastDeepScanAt.isValid()) {
        statement.bind(6, toStorage(album.lastDeepScanAt));
    } else {
        statement.bindNull(6);
    }
    if (album.lastSyncedAt.isValid()) {
        statement.bind(7, toStorage(album.lastSyncedAt));
    } else {
        statement.bindNull(7);
    }
    statement.bind(8, album.isExcluded);
    statement.bind(9, album.hasSafetyHold);
    return statement.exec(errorMessage);
}

bool SyncStore::deleteAlbum(const QString &albumId, QString *errorMessage)
{
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("DELETE FROM album WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, albumId);
    return statement.exec(errorMessage);
}

bool SyncStore::setExcluded(bool excluded, const QString &albumId, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("UPDATE album SET is_excluded = ? WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, excluded);
    statement.bind(2, albumId);
    return statement.exec(errorMessage);
}

bool SyncStore::setSafetyHold(bool held, const QString &albumId, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("UPDATE album SET safety_hold = ? WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, held);
    statement.bind(2, albumId);
    return statement.exec(errorMessage);
}

bool SyncStore::markSynced(const QString &albumId, const QDateTime &date, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("UPDATE album SET last_synced_at = ? WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, toStorage(date));
    statement.bind(2, albumId);
    return statement.exec(errorMessage);
}

QSet<QString> SyncStore::excludedAlbumIds()
{
    QSet<QString> result;
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("SELECT album_id FROM album WHERE is_excluded = 1"));
    if (!statement.isValid()) {
        return result;
    }
    while (statement.next()) {
        result.insert(statement.columnText(0));
    }
    return result;
}

// MARK: - Baselines

QHash<Sha1Checksum, AssetBaseline> SyncStore::baseline(const QString &albumId)
{
    QHash<Sha1Checksum, AssetBaseline> result;
    SqliteStatement statement = m_database.prepare(QStringLiteral(
        "SELECT album_id, checksum, asset_id, original_file_name, relative_path, size, synced_at "
        "FROM asset WHERE album_id = ?"));
    if (!statement.isValid()) {
        return result;
    }
    statement.bind(1, albumId);
    while (statement.next()) {
        const auto checksum = Sha1Checksum::fromBase64(statement.columnText(1));
        if (!checksum) {
            // A row whose checksum will not decode cannot be matched against anything;
            // skipping it makes the asset look new, which is recoverable. Using it
            // would not be.
            continue;
        }
        AssetBaseline baseline;
        baseline.albumId = statement.columnText(0);
        baseline.checksum = *checksum;
        baseline.assetId = statement.columnText(2);
        baseline.originalFileName = statement.columnText(3);
        baseline.relativePath = statement.columnText(4);
        baseline.size = statement.columnInt64(5);
        baseline.syncedAt = fromStorage(statement.columnDouble(6));
        result.insert(*checksum, baseline);
    }
    return result;
}

bool SyncStore::upsert(const AssetBaseline &baseline, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(QStringLiteral(R"(
        INSERT INTO asset (album_id, checksum, asset_id, original_file_name,
                           relative_path, size, synced_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (album_id, checksum) DO UPDATE SET
            asset_id = excluded.asset_id,
            original_file_name = excluded.original_file_name,
            relative_path = excluded.relative_path,
            size = excluded.size,
            synced_at = excluded.synced_at
    )"),
                                                   errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, baseline.albumId);
    statement.bind(2, baseline.checksum.base64());
    statement.bind(3, baseline.assetId);
    statement.bind(4, baseline.originalFileName);
    statement.bind(5, baseline.relativePath);
    statement.bind(6, baseline.size);
    statement.bind(7, toStorage(baseline.syncedAt));
    return statement.exec(errorMessage);
}

bool SyncStore::deleteBaseline(const QString &albumId,
                               const Sha1Checksum &checksum,
                               QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("DELETE FROM asset WHERE album_id = ? AND checksum = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, albumId);
    statement.bind(2, checksum.base64());
    return statement.exec(errorMessage);
}

bool SyncStore::deleteBaselines(const QString &albumId, QString *errorMessage)
{
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("DELETE FROM asset WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, albumId);
    return statement.exec(errorMessage);
}

// MARK: - Local file hash cache

QHash<QString, LocalFileFingerprint> SyncStore::fingerprints(const QString &folderName)
{
    QHash<QString, LocalFileFingerprint> result;
    SqliteStatement statement = m_database.prepare(QStringLiteral(
        "SELECT relative_path, device_id, inode, size, modified_at_nanos, checksum "
        "FROM local_file WHERE relative_path LIKE ? ESCAPE '\\'"));
    if (!statement.isValid()) {
        return result;
    }
    statement.bind(1, likePrefix(folderName + QLatin1Char('/')));
    while (statement.next()) {
        const auto checksum = Sha1Checksum::fromBase64(statement.columnText(5));
        if (!checksum) {
            continue;
        }
        LocalFileFingerprint fingerprint;
        fingerprint.relativePath = statement.columnText(0);
        fingerprint.deviceId = statement.columnInt64(1);
        fingerprint.inode = statement.columnInt64(2);
        fingerprint.size = statement.columnInt64(3);
        fingerprint.modifiedAtNanoseconds = statement.columnInt64(4);
        fingerprint.checksum = *checksum;
        result.insert(fingerprint.relativePath, fingerprint);
    }
    return result;
}

bool SyncStore::upsert(const LocalFileFingerprint &fingerprint,
                       const QDateTime &at,
                       QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(QStringLiteral(R"(
        INSERT INTO local_file (relative_path, device_id, inode, size,
                                modified_at_nanos, checksum, hashed_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT (relative_path) DO UPDATE SET
            device_id = excluded.device_id,
            inode = excluded.inode,
            size = excluded.size,
            modified_at_nanos = excluded.modified_at_nanos,
            checksum = excluded.checksum,
            hashed_at = excluded.hashed_at
    )"),
                                                   errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, fingerprint.relativePath);
    statement.bind(2, fingerprint.deviceId);
    statement.bind(3, fingerprint.inode);
    statement.bind(4, fingerprint.size);
    statement.bind(5, fingerprint.modifiedAtNanoseconds);
    statement.bind(6, fingerprint.checksum.base64());
    statement.bind(7, toStorage(at));
    return statement.exec(errorMessage);
}

bool SyncStore::deleteFingerprint(const QString &relativePath, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("DELETE FROM local_file WHERE relative_path = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, relativePath);
    return statement.exec(errorMessage);
}

bool SyncStore::pruneFingerprints(const QString &folderName,
                                  const QSet<QString> &livePaths,
                                  QString *errorMessage)
{
    QStringList stale;
    {
        SqliteStatement statement = m_database.prepare(
            QStringLiteral("SELECT relative_path FROM local_file WHERE relative_path LIKE ? "
                           "ESCAPE '\\'"),
            errorMessage);
        if (!statement.isValid()) {
            return false;
        }
        statement.bind(1, likePrefix(folderName + QLatin1Char('/')));
        while (statement.next()) {
            const QString path = statement.columnText(0);
            if (!livePaths.contains(path)) {
                stale.append(path);
            }
        }
    }
    if (stale.isEmpty()) {
        return true;
    }

    return m_database.transaction(
        [&]() {
            for (const QString &path : std::as_const(stale)) {
                if (!deleteFingerprint(path, errorMessage)) {
                    return false;
                }
            }
            return true;
        },
        errorMessage);
}

QString SyncStore::likePrefix(const QString &prefix)
{
    QString escaped = prefix;
    escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    escaped.replace(QLatin1String("%"), QLatin1String("\\%"));
    escaped.replace(QLatin1String("_"), QLatin1String("\\_"));
    return escaped + QLatin1Char('%');
}

// MARK: - Safety-held removals

QList<HeldRemoval> SyncStore::heldRemovals()
{
    QList<HeldRemoval> result;
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("SELECT album_id, checksum, direction, display_name, detected_at "
                       "FROM held_removal ORDER BY detected_at"));
    if (!statement.isValid()) {
        return result;
    }
    while (statement.next()) {
        const auto checksum = Sha1Checksum::fromBase64(statement.columnText(1));
        if (!checksum) {
            continue;
        }
        HeldRemoval removal;
        removal.albumId = statement.columnText(0);
        removal.checksum = *checksum;
        removal.direction = directionFromString(statement.columnText(2));
        removal.displayName = statement.columnText(3);
        removal.detectedAt = fromStorage(statement.columnDouble(4));
        result.append(removal);
    }
    return result;
}

bool SyncStore::replaceHeldRemovals(const QString &albumId,
                                    const QList<HeldRemoval> &removals,
                                    QString *errorMessage)
{
    return m_database.transaction(
        [&]() {
            if (!clearHeldRemovals(albumId, errorMessage)) {
                return false;
            }
            for (const HeldRemoval &removal : removals) {
                SqliteStatement statement = m_database.prepare(
                    QStringLiteral("INSERT INTO held_removal (album_id, checksum, direction, "
                                   "display_name, detected_at) VALUES (?, ?, ?, ?, ?)"),
                    errorMessage);
                if (!statement.isValid()) {
                    return false;
                }
                statement.bind(1, removal.albumId.isEmpty() ? albumId : removal.albumId);
                statement.bind(2, removal.checksum.base64());
                statement.bind(3, keyFor(removal.direction));
                statement.bind(4, removal.displayName);
                statement.bind(5, toStorage(removal.detectedAt));
                if (!statement.exec(errorMessage)) {
                    return false;
                }
            }
            return true;
        },
        errorMessage);
}

bool SyncStore::clearHeldRemovals(const QString &albumId, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("DELETE FROM held_removal WHERE album_id = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, albumId);
    return statement.exec(errorMessage);
}

QSet<QString> SyncStore::albumsWithHeldRemovals()
{
    QSet<QString> result;
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("SELECT DISTINCT album_id FROM held_removal"));
    if (!statement.isValid()) {
        return result;
    }
    while (statement.next()) {
        result.insert(statement.columnText(0));
    }
    return result;
}

// MARK: - Transfer back-off

QSet<QString> SyncStore::backedOffKeys(const QDateTime &now)
{
    QSet<QString> result;
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("SELECT key FROM transfer_failure WHERE next_attempt_at > ?"));
    if (!statement.isValid()) {
        return result;
    }
    statement.bind(1, toStorage(now));
    while (statement.next()) {
        result.insert(statement.columnText(0));
    }
    return result;
}

bool SyncStore::recordFailure(const QString &key,
                              const QString &error,
                              const QDateTime &now,
                              QString *errorMessage)
{
    int attempts = 0;
    {
        SqliteStatement statement = m_database.prepare(
            QStringLiteral("SELECT attempts FROM transfer_failure WHERE key = ?"), errorMessage);
        if (!statement.isValid()) {
            return false;
        }
        statement.bind(1, key);
        if (statement.next()) {
            attempts = statement.columnInt(0);
        }
    }
    ++attempts;

    SqliteStatement statement = m_database.prepare(QStringLiteral(R"(
        INSERT INTO transfer_failure (key, attempts, last_error, next_attempt_at)
        VALUES (?, ?, ?, ?)
        ON CONFLICT (key) DO UPDATE SET
            attempts = excluded.attempts,
            last_error = excluded.last_error,
            next_attempt_at = excluded.next_attempt_at
    )"),
                                                   errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, key);
    statement.bind(2, attempts);
    // Truncated: a server that returns a whole HTML page as its error would otherwise
    // put it in every row of this table.
    statement.bind(3, error.left(500));
    statement.bind(4, toStorage(TransferBackoff::nextAttempt(attempts, now)));
    return statement.exec(errorMessage);
}

bool SyncStore::clearFailure(const QString &key, QString *errorMessage)
{
    SqliteStatement statement = m_database.prepare(
        QStringLiteral("DELETE FROM transfer_failure WHERE key = ?"), errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, key);
    return statement.exec(errorMessage);
}

int SyncStore::failureCount()
{
    return static_cast<int>(
        m_database.scalarInt64(QStringLiteral("SELECT COUNT(*) FROM transfer_failure")));
}

// MARK: - Meta

std::optional<QString> SyncStore::metaValue(const QString &key)
{
    SqliteStatement statement =
        m_database.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    if (!statement.isValid()) {
        return std::nullopt;
    }
    statement.bind(1, key);
    if (!statement.next()) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

bool SyncStore::setMetaValue(const QString &key,
                             const std::optional<QString> &value,
                             QString *errorMessage)
{
    if (!value) {
        SqliteStatement statement =
            m_database.prepare(QStringLiteral("DELETE FROM meta WHERE key = ?"), errorMessage);
        if (!statement.isValid()) {
            return false;
        }
        statement.bind(1, key);
        return statement.exec(errorMessage);
    }

    SqliteStatement statement = m_database.prepare(
        QStringLiteral("INSERT INTO meta (key, value) VALUES (?, ?) "
                       "ON CONFLICT (key) DO UPDATE SET value = excluded.value"),
        errorMessage);
    if (!statement.isValid()) {
        return false;
    }
    statement.bind(1, key);
    statement.bind(2, *value);
    return statement.exec(errorMessage);
}

SyncStore::Statistics SyncStore::statistics()
{
    Statistics statistics;
    statistics.albumCount = static_cast<int>(
        m_database.scalarInt64(QStringLiteral("SELECT COUNT(*) FROM album WHERE is_excluded = 0")));
    statistics.syncedAssetCount =
        static_cast<int>(m_database.scalarInt64(QStringLiteral("SELECT COUNT(*) FROM asset")));
    statistics.syncedByteCount =
        m_database.scalarInt64(QStringLiteral("SELECT COALESCE(SUM(size), 0) FROM asset"));
    statistics.heldRemovalCount = static_cast<int>(
        m_database.scalarInt64(QStringLiteral("SELECT COUNT(*) FROM held_removal")));
    statistics.failureCount = static_cast<int>(
        m_database.scalarInt64(QStringLiteral("SELECT COUNT(*) FROM transfer_failure")));
    return statistics;
}

bool SyncStore::reset(QString *errorMessage)
{
    const bool ok = m_database.transaction(
        [&]() {
            for (const char *table :
                 {"held_removal", "asset", "local_file", "transfer_failure", "album", "meta"}) {
                if (!m_database.execute(QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)),
                                        errorMessage)) {
                    return false;
                }
            }
            return true;
        },
        errorMessage);
    if (!ok) {
        return false;
    }
    m_database.execute(QStringLiteral("VACUUM"));
    log::storage.notice(QStringLiteral("Local sync state reset"));
    return true;
}

void persisting(const QString &description, const std::function<bool(QString *)> &body)
{
    QString error;
    if (!body(&error)) {
        log::storage.error(QStringLiteral("Could not persist %1: %2").arg(description, error));
    }
}

} // namespace immichksync
