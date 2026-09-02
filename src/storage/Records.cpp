#include "storage/Records.h"

#include <algorithm>
#include <cmath>

namespace immichksync {

bool AssetBaseline::operator==(const AssetBaseline &other) const
{
    return albumId == other.albumId && checksum == other.checksum && assetId == other.assetId
        && originalFileName == other.originalFileName && relativePath == other.relativePath
        && size == other.size && syncedAt == other.syncedAt;
}

bool LocalFileFingerprint::matches(const LocalFileFingerprint &other) const
{
    return deviceId == other.deviceId && inode == other.inode && size == other.size
        && modifiedAtNanoseconds == other.modifiedAtNanoseconds;
}

QString HeldRemoval::id() const
{
    return QStringLiteral("%1/%2").arg(albumId, checksum.base64());
}

QString keyFor(HeldRemoval::Direction direction)
{
    return direction == HeldRemoval::Direction::RemoveFromAlbum
        ? QStringLiteral("removeFromAlbum")
        : QStringLiteral("trashLocalFile");
}

HeldRemoval::Direction directionFromString(const QString &raw)
{
    return raw == QLatin1String("trashLocalFile") ? HeldRemoval::Direction::TrashLocalFile
                                                  : HeldRemoval::Direction::RemoveFromAlbum;
}

namespace TransferBackoff {

QDateTime nextAttempt(int attempts, const QDateTime &now)
{
    const int capped = std::min(std::max(attempts, 1), 7);
    const double seconds = std::min(60.0 * std::pow(4.0, static_cast<double>(capped - 1)),
                                    6.0 * 60.0 * 60.0);
    return now.addMSecs(static_cast<qint64>(seconds * 1000.0));
}

QString downloadKey(const QString &albumId, const QString &assetId)
{
    return QStringLiteral("download:%1:%2").arg(albumId, assetId);
}

QString uploadKey(const QString &albumId, const QString &relativePath)
{
    return QStringLiteral("upload:%1:%2").arg(albumId, relativePath);
}

} // namespace TransferBackoff

} // namespace immichksync
