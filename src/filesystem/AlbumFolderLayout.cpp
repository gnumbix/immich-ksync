#include "filesystem/AlbumFolderLayout.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace immichksync {

bool AlbumMarker::operator==(const AlbumMarker &other) const
{
    return schema == other.schema && albumId == other.albumId && albumName == other.albumName
        && folderName == other.folderName;
}

namespace AlbumFolderLayout {

namespace {

/// Linux caps a single path component at 255 bytes (NAME_MAX), the same as APFS.
constexpr int kMaximumComponentBytes = 255;
constexpr const char *kFallbackAlbumName = "Untitled Album";

QSet<QString> lowercased(const QSet<QString> &values)
{
    QSet<QString> result;
    result.reserve(values.size());
    for (const QString &value : values) {
        result.insert(value.toLower());
    }
    return result;
}

/// Shared core of both sanitisers: the character rules are identical, only the
/// trailing-dot handling differs.
QString sanitizeCommon(const QString &input)
{
    QString name = input.normalized(QString::NormalizationForm_C);
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    // `:` is legal on Linux, and is stripped anyway. The macOS build must remove it
    // because Finder still renders it as `/`, and the two builds have to agree or the
    // same album would land in two differently named folders.
    name.replace(QLatin1Char(':'), QLatin1Char('-'));

    QString filtered;
    filtered.reserve(name.size());
    for (const QChar c : std::as_const(name)) {
        const char16_t value = c.unicode();
        if (value >= 0x20 && value != 0x7F) {
            filtered.append(c);
        }
    }
    filtered = filtered.trimmed();
    // A leading dot would create an invisible folder the scanner skips.
    while (filtered.startsWith(QLatin1Char('.'))) {
        filtered.remove(0, 1);
    }
    return filtered;
}

} // namespace

QSet<QString> reservedFolderNames()
{
    return {QString::fromLatin1(kTrashFolderName), QString::fromLatin1(kStagingFolderName)};
}

QString sanitize(const QString &albumName)
{
    QString name = sanitizeCommon(albumName);
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    name = truncate(name, kMaximumComponentBytes);
    return name.isEmpty() ? QString::fromLatin1(kFallbackAlbumName) : name;
}

QString sanitizeFilename(const QString &filename)
{
    QString name = truncate(sanitizeCommon(filename), kMaximumComponentBytes);
    return name.isEmpty() ? QStringLiteral("asset") : name;
}

QString folderName(const QString &albumName, const QString &albumId, const QSet<QString> &taken)
{
    const QSet<QString> takenLower = lowercased(taken);
    const QString base = sanitize(albumName);
    if (!takenLower.contains(base.toLower()) && !reservedFolderNames().contains(base)) {
        return base;
    }

    QString compactId = albumId;
    compactId.remove(QLatin1Char('-'));
    const QString suffix = QStringLiteral(" (%1)").arg(compactId.left(8));
    const QString trimmed = truncate(base, kMaximumComponentBytes - suffix.toUtf8().size());
    const QString candidate = trimmed + suffix;
    if (!takenLower.contains(candidate.toLower())) {
        return candidate;
    }

    // Both the name and the ID-suffixed name are taken: fall back to the full ID,
    // which is unique by construction.
    return truncate(base, kMaximumComponentBytes - albumId.toUtf8().size() - 3)
        + QStringLiteral(" (%1)").arg(albumId);
}

QString localFilename(const QString &originalFileName,
                      const Sha1Checksum &checksum,
                      const QSet<QString> &taken)
{
    const QSet<QString> takenLower = lowercased(taken);
    const QString sanitized = sanitizeFilename(originalFileName);
    if (!takenLower.contains(sanitized.toLower())) {
        return sanitized;
    }

    const QFileInfo info(sanitized);
    const QString extension = info.suffix();
    const QString stem = extension.isEmpty() ? sanitized
                                             : sanitized.left(sanitized.size() - extension.size() - 1);
    const QString suffix = QStringLiteral("~%1").arg(checksum.shortHex());
    const int budget = kMaximumComponentBytes - suffix.toUtf8().size()
        - (extension.isEmpty() ? 0 : extension.toUtf8().size() + 1);
    const QString trimmedStem = truncate(stem, std::max(budget, 1));
    return extension.isEmpty() ? trimmedStem + suffix
                               : QStringLiteral("%1%2.%3").arg(trimmedStem, suffix, extension);
}

QString truncate(const QString &value, int limitBytes)
{
    if (limitBytes <= 0) {
        return {};
    }
    if (value.toUtf8().size() <= limitBytes) {
        return value;
    }
    QString result = value;
    while (!result.isEmpty() && result.toUtf8().size() > limitBytes) {
        result.chop(1);
    }
    return result;
}

QString markerPath(const QString &folderPath)
{
    return QDir(folderPath).filePath(QString::fromLatin1(kMarkerFilename));
}

std::optional<AlbumMarker> readMarker(const QString &folderPath)
{
    const QString path = markerPath(folderPath);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        log::fileSystem.warning(
            QStringLiteral("Ignoring unreadable album marker at %1").arg(path));
        return std::nullopt;
    }

    const QJsonObject object = document.object();
    AlbumMarker marker;
    marker.schema = object.value(QStringLiteral("schema")).toInt(0);
    marker.albumId = object.value(QStringLiteral("albumID")).toString();
    marker.albumName = object.value(QStringLiteral("albumName")).toString();
    marker.folderName = object.value(QStringLiteral("folderName")).toString();

    if (marker.schema > AlbumMarker::kCurrentSchema || marker.albumId.isEmpty()) {
        log::fileSystem.warning(
            QStringLiteral("Album marker at %1 uses schema v%2; this build understands v%3.")
                .arg(path)
                .arg(marker.schema)
                .arg(AlbumMarker::kCurrentSchema));
        return std::nullopt;
    }
    return marker;
}

bool writeMarker(const AlbumMarker &marker, const QString &folderPath, QString *errorMessage)
{
    // Key names and the sorted, indented shape match the macOS build byte for byte, so
    // a marker written on either platform reads cleanly on the other.
    QJsonObject object;
    object.insert(QStringLiteral("albumID"), marker.albumId);
    object.insert(QStringLiteral("albumName"), marker.albumName);
    object.insert(QStringLiteral("folderName"), marker.folderName);
    object.insert(QStringLiteral("schema"), marker.schema);

    QSaveFile file(markerPath(folderPath));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

bool isInternalName(const QString &name)
{
    return name.startsWith(QLatin1Char('.')) || reservedFolderNames().contains(name);
}

} // namespace AlbumFolderLayout

} // namespace immichksync
