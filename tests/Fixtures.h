#pragma once

#include "core/Checksum.h"
#include "storage/Records.h"
#include "core/Clock.h"
#include "sync/SyncPlan.h"
#include "sync/SyncPlanner.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QTest>
#include <QTimeZone>

#include <optional>

namespace immichksync {
namespace Fixture {

/// A fixed instant, so nothing in the hermetic suites depends on the wall clock.
inline QDateTime referenceDate()
{
    return QDateTime::fromMSecsSinceEpoch(1'700'000'000'000, QTimeZone::UTC);
}

/// Deterministic, distinct checksums: `checksum(1)`, `checksum(2)`, …
inline Sha1Checksum checksum(int seed)
{
    const QString hex = QStringLiteral("%1").arg(seed, 40, 16, QLatin1Char('0'));
    return *Sha1Checksum::fromHex(hex);
}

inline QString defaultFilename(int seed)
{
    return QStringLiteral("IMG_%1.HEIC").arg(seed, 4, 10, QLatin1Char('0'));
}

inline RemoteAsset remoteAsset(int seed,
                               const QString &assetId = QString(),
                               const QString &filename = QString())
{
    RemoteAsset asset;
    asset.assetId = assetId.isEmpty() ? QStringLiteral("asset-%1").arg(seed) : assetId;
    asset.checksum = checksum(seed);
    asset.originalFileName = filename.isEmpty() ? defaultFilename(seed) : filename;
    asset.fileCreatedAt = referenceDate();
    asset.fileModifiedAt = referenceDate();
    return asset;
}

inline LocalAsset localAsset(int seed,
                             const QString &folder = QStringLiteral("Album"),
                             const QString &filename = QString())
{
    const QString name = filename.isEmpty() ? defaultFilename(seed) : filename;
    LocalAsset asset;
    asset.relativePath = QStringLiteral("%1/%2").arg(folder, name);
    asset.filename = name;
    asset.path = QStringLiteral("/tmp/%1/%2").arg(folder, name);
    asset.checksum = checksum(seed);
    asset.size = static_cast<qint64>(seed) * 1000;
    asset.createdAt = referenceDate();
    asset.modifiedAt = referenceDate();
    return asset;
}

inline AssetBaseline baseline(int seed,
                              const QString &albumId = QStringLiteral("album-1"),
                              const QString &folder = QStringLiteral("Album"),
                              const QString &filename = QString())
{
    const QString name = filename.isEmpty() ? defaultFilename(seed) : filename;
    AssetBaseline baseline;
    baseline.albumId = albumId;
    baseline.checksum = checksum(seed);
    baseline.assetId = QStringLiteral("asset-%1").arg(seed);
    baseline.originalFileName = name;
    baseline.relativePath = QStringLiteral("%1/%2").arg(folder, name);
    baseline.size = static_cast<qint64>(seed) * 1000;
    baseline.syncedAt = referenceDate();
    return baseline;
}

inline QHash<Sha1Checksum, AssetBaseline> baselineMap(const QList<int> &seeds,
                                                      const QString &albumId
                                                      = QStringLiteral("album-1"),
                                                      const QString &folder
                                                      = QStringLiteral("Album"))
{
    QHash<Sha1Checksum, AssetBaseline> map;
    for (const int seed : seeds) {
        map.insert(checksum(seed), baseline(seed, albumId, folder));
    }
    return map;
}

inline RemoteAlbumSummary remoteAlbum(const QString &id = QStringLiteral("album-1"),
                                      const QString &name = QStringLiteral("Album"),
                                      int assetCount = 0)
{
    RemoteAlbumSummary summary;
    summary.id = id;
    summary.name = name;
    summary.updatedAt = QStringLiteral("2024-01-01T00:00:00.000Z");
    summary.assetCount = assetCount;
    return summary;
}


inline AlbumMarker marker(const QString &albumName = QStringLiteral("Album"),
                          const QString &folderName = QStringLiteral("Album"),
                          const QString &albumId = QStringLiteral("album-1"))
{
    AlbumMarker marker;
    marker.albumId = albumId;
    marker.albumName = albumName;
    marker.folderName = folderName;
    return marker;
}

/// Named options for `input()`. C++ has no default arguments by keyword, and a
/// positional call with nine defaults would be unreadable at every call site.
struct InputOptions {
    QList<RemoteAsset> remote;
    QList<LocalAsset> local;
    QHash<Sha1Checksum, AssetBaseline> baseline;
    std::optional<AlbumMarker> marker = Fixture::marker();
    std::optional<RemoteAlbumSummary> album = Fixture::remoteAlbum();
    std::optional<QString> folderName = QStringLiteral("Album");
    bool suppressRemovals = false;
    QSet<QString> reservedFolderNames = {QStringLiteral("Album")};
    QSet<QString> occupiedFilenames;
    QStringList nestedFolderNames;
    std::optional<AlbumRecord> storedRecord;
    bool remoteUnchanged = false;
};

inline AlbumPlanInput input(InputOptions options = {})
{
    AlbumPlanInput input;
    input.remoteAlbum = options.album;
    input.folderName = options.folderName;
    input.marker = options.marker;
    input.storedRecord = options.storedRecord;
    input.remoteAssets = options.remoteUnchanged ? RemoteSnapshot::unchanged()
                                                 : RemoteSnapshot::enumerated(options.remote);
    input.localAssets = options.local;
    input.baseline = options.baseline;
    input.reservedFolderNames = options.reservedFolderNames;
    input.occupiedFilenames = options.occupiedFilenames;
    input.nestedFolderNames = options.nestedFolderNames;
    input.suppressRemovals = options.suppressRemovals;
    return input;
}

} // namespace Fixture

/// A clock that never moves, so deep-scan intervals and settle windows are decided by
/// the test rather than by how long the test took to run.
class FixedDateProvider : public DateProvider {
public:
    explicit FixedDateProvider(QDateTime value = Fixture::referenceDate())
        : m_value(std::move(value))
    {
    }
    QDateTime now() const override { return m_value; }
    void setNow(const QDateTime &value) { m_value = value; }

private:
    QDateTime m_value;
};

} // namespace immichksync

/// QTest's generic `toString` finds our own `toString(HeldRemoval::Direction)` through
/// ADL and then cannot convert the QString it returns. Explicit specialisations both
/// fix that and make a failure message name the value rather than print a pointer.
namespace QTest {

template<>
inline char *toString(const immichksync::HeldRemoval::Direction &value)
{
    return qstrdup(qUtf8Printable(immichksync::keyFor(value)));
}

template<>
inline char *toString(const immichksync::Sha1Checksum &value)
{
    return qstrdup(qUtf8Printable(value.base64()));
}

template<>
inline char *toString(const immichksync::AlbumStructureAction &value)
{
    static const char *kinds[] = {"createRemoteAlbum",
                                  "createLocalFolder",
                                  "renameLocalFolder",
                                  "renameRemoteAlbum",
                                  "trashLocalFolder"};
    return qstrdup(qUtf8Printable(QStringLiteral("%1(name=%2, from=%3, album=%4)")
                                     .arg(QLatin1String(kinds[static_cast<int>(value.kind)]),
                                          value.name,
                                          value.fromName,
                                          value.albumId)));
}

} // namespace QTest
