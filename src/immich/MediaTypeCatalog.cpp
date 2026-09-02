#include "immich/MediaTypeCatalog.h"

namespace immichksync {

namespace {

/// The server is inconsistent about the leading dot between versions, so both forms
/// are normalised to the dotted one the scanner compares against.
QSet<QString> normalize(const QStringList &values)
{
    QSet<QString> result;
    result.reserve(values.size());
    for (const QString &value : values) {
        const QString lower = value.toLower();
        result.insert(lower.startsWith(QLatin1Char('.')) ? lower : QLatin1Char('.') + lower);
    }
    return result;
}

} // namespace

MediaTypeCatalog MediaTypeCatalog::fromResponse(const ServerMediaTypesResponse &response)
{
    MediaTypeCatalog catalog;
    catalog.image = normalize(response.image);
    catalog.video = normalize(response.video);
    catalog.sidecar = normalize(response.sidecar);
    return catalog;
}

MediaTypeCatalog MediaTypeCatalog::fallback()
{
    MediaTypeCatalog catalog;
    catalog.image = {QStringLiteral(".jpg"),  QStringLiteral(".jpeg"), QStringLiteral(".png"),
                     QStringLiteral(".heic"), QStringLiteral(".heif"), QStringLiteral(".gif"),
                     QStringLiteral(".tif"),  QStringLiteral(".tiff"), QStringLiteral(".webp"),
                     QStringLiteral(".dng"),  QStringLiteral(".raf"),  QStringLiteral(".cr2"),
                     QStringLiteral(".cr3"),  QStringLiteral(".nef"),  QStringLiteral(".arw"),
                     QStringLiteral(".orf"),  QStringLiteral(".rw2"),  QStringLiteral(".avif"),
                     QStringLiteral(".bmp"),  QStringLiteral(".jxl")};
    catalog.video = {QStringLiteral(".mp4"), QStringLiteral(".mov"),  QStringLiteral(".m4v"),
                     QStringLiteral(".avi"), QStringLiteral(".mkv"),  QStringLiteral(".webm"),
                     QStringLiteral(".3gp"), QStringLiteral(".mpg"),  QStringLiteral(".mpeg"),
                     QStringLiteral(".wmv"), QStringLiteral(".flv"),  QStringLiteral(".insv")};
    catalog.sidecar = {QStringLiteral(".xmp")};
    return catalog;
}

bool MediaTypeCatalog::operator==(const MediaTypeCatalog &other) const
{
    return image == other.image && video == other.video && sidecar == other.sidecar;
}

} // namespace immichksync
