#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace immichksync {

/// One `multipart/form-data` part backed by a file on disk.
struct MultipartFilePart {
    QString name;
    QString filename;
    QString path;

    QString contentType() const;
};

/// An assembled request body living in a temporary file, plus its exact length.
class MultipartBody {
public:
    MultipartBody() = default;
    ~MultipartBody();
    MultipartBody(MultipartBody &&other) noexcept;
    MultipartBody &operator=(MultipartBody &&other) noexcept;
    Q_DISABLE_COPY(MultipartBody)

    bool isValid() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }
    QString contentType() const { return m_contentType; }
    qint64 contentLength() const { return m_contentLength; }

    /// Deletes the temporary file. Also runs from the destructor.
    void discard();

    /// Assembles a body on disk so the transport can stream it with an exact
    /// `Content-Length`.
    ///
    /// Staging to disk rather than composing a stream is deliberate: the transport
    /// re-reads the file for redirects and retries and never holds the asset in
    /// memory, whereas a producer-driven stream's worst failure mode is a hung upload —
    /// unacceptable in an unattended background agent. The cost is transient disk use
    /// equal to the asset size, which is checked for up front.
    static MultipartBody write(const QList<QPair<QString, QString>> &fields,
                               const QList<MultipartFilePart> &files,
                               const QString &stagingDirectory,
                               QString *errorMessage);

private:
    QString m_path;
    QString m_contentType;
    qint64 m_contentLength = 0;
};

} // namespace immichksync
