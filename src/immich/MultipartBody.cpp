#include "immich/MultipartBody.h"

#include "core/Logging.h"
#include "filesystem/AtomicFileWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStorageInfo>
#include <QUuid>
#include <QtCore/qscopeguard.h>

namespace immichksync {

namespace {

constexpr qint64 kCopyBufferSize = 1 << 20; // 1 MiB
/// Keep a cushion so staging an upload can never fill the disk.
constexpr qint64 kDiskCushion = 256LL << 20;

/// RFC 7578 §5.1 quoting for `name` and `filename` parameters.
QString escape(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    escaped.replace(QLatin1String("\""), QLatin1String("\\\""));
    escaped.remove(QLatin1Char('\r'));
    escaped.remove(QLatin1Char('\n'));
    return escaped;
}

bool copyInto(QFile &destination, const QString &sourcePath, QString *errorMessage)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not read %1 for upload.")
                                .arg(QFileInfo(sourcePath).fileName());
        }
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kCopyBufferSize);
        if (chunk.isEmpty()) {
            break;
        }
        if (destination.write(chunk) != chunk.size()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not stage the upload: %1")
                                    .arg(destination.errorString());
            }
            return false;
        }
    }
    return true;
}

} // namespace

QString MultipartFilePart::contentType() const
{
    const QMimeType type = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    return type.isValid() ? type.name() : QStringLiteral("application/octet-stream");
}

MultipartBody::~MultipartBody()
{
    discard();
}

MultipartBody::MultipartBody(MultipartBody &&other) noexcept
    : m_path(std::move(other.m_path))
    , m_contentType(std::move(other.m_contentType))
    , m_contentLength(other.m_contentLength)
{
    other.m_path.clear();
    other.m_contentLength = 0;
}

MultipartBody &MultipartBody::operator=(MultipartBody &&other) noexcept
{
    if (this != &other) {
        discard();
        m_path = std::move(other.m_path);
        m_contentType = std::move(other.m_contentType);
        m_contentLength = other.m_contentLength;
        other.m_path.clear();
        other.m_contentLength = 0;
    }
    return *this;
}

void MultipartBody::discard()
{
    if (!m_path.isEmpty()) {
        QFile::remove(m_path);
        m_path.clear();
        m_contentLength = 0;
    }
}

MultipartBody MultipartBody::write(const QList<QPair<QString, QString>> &fields,
                                   const QList<MultipartFilePart> &files,
                                   const QString &stagingDirectory,
                                   QString *errorMessage)
{
    MultipartBody body;

    if (!AtomicFileWriter::ensureDirectory(stagingDirectory, /*markAsCache=*/true, errorMessage)) {
        return body;
    }

    qint64 payloadSize = 0;
    for (const MultipartFilePart &file : files) {
        const QFileInfo info(file.path);
        if (!info.exists()) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Could not read %1 for upload.").arg(info.fileName());
            }
            return body;
        }
        payloadSize += info.size();
    }

    if (payloadSize > 0) {
        const QStorageInfo storage(stagingDirectory);
        if (storage.isValid()) {
            const qint64 available = storage.bytesAvailable();
            if (available > 0 && available < payloadSize + kDiskCushion) {
                if (errorMessage) {
                    *errorMessage = QStringLiteral("Not enough free space to stage the upload: "
                                                   "need %1 bytes, %2 available.")
                                        .arg(payloadSize)
                                        .arg(available);
                }
                return body;
            }
        }
    }

    const QString boundary =
        QStringLiteral("----ImmichKSync-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    const QString path =
        QDir(stagingDirectory)
            .filePath(QStringLiteral("upload-%1.multipart")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not stage the upload: %1").arg(file.errorString());
        }
        return body;
    }

    bool succeeded = false;
    // Removed unless the whole body is written, so a half-formed multipart never
    // reaches the wire.
    const auto cleanup = qScopeGuard([&]() {
        file.close();
        if (!succeeded) {
            QFile::remove(path);
        }
    });

    for (const auto &field : fields) {
        const QString header = QStringLiteral("--%1\r\nContent-Disposition: form-data; "
                                              "name=\"%2\"\r\n\r\n")
                                   .arg(boundary, escape(field.first));
        file.write(header.toUtf8());
        file.write(field.second.toUtf8());
        file.write("\r\n");
    }

    for (const MultipartFilePart &part : files) {
        const QString header =
            QStringLiteral("--%1\r\nContent-Disposition: form-data; name=\"%2\"; "
                           "filename=\"%3\"\r\nContent-Type: %4\r\n\r\n")
                .arg(boundary, escape(part.name), escape(part.filename), part.contentType());
        file.write(header.toUtf8());
        if (!copyInto(file, part.path, errorMessage)) {
            return body;
        }
        file.write("\r\n");
    }

    file.write(QStringLiteral("--%1--\r\n").arg(boundary).toUtf8());
    if (file.error() != QFile::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not stage the upload: %1").arg(file.errorString());
        }
        return body;
    }
    file.flush();

    body.m_path = path;
    body.m_contentType = QStringLiteral("multipart/form-data; boundary=%1").arg(boundary);
    body.m_contentLength = file.size();
    succeeded = true;
    return body;
}

} // namespace immichksync
