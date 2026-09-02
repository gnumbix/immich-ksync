#pragma once

#include "immich/ImmichError.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QUrl>

#include <memory>

namespace immichksync {

class TlsCertificateStore;

/// One HTTP exchange, independent of how it is carried out.
struct HttpRequest {
    QString method = QStringLiteral("GET");
    QUrl url;
    QMap<QString, QString> headers;
    /// In-memory body. Mutually exclusive with `uploadFilePath`.
    QByteArray body;
    /// Streamed body, for uploads, so a multi-gigabyte video never enters memory.
    QString uploadFilePath;
    /// When set, the response body is streamed here instead of being buffered.
    QString downloadFilePath;
    /// Uploads must not follow redirects — a 307 would replay the body, and a 30x to
    /// an HTML login page would look like a successful upload. Matches the official
    /// CLI, which sets `redirect: 'error'`.
    bool followRedirects = true;
    /// Aborts if no data moves for this long. Not a cap on total duration: a slow
    /// large download is fine, a stalled one is not.
    int transferTimeoutSeconds = 60;

    QString label() const { return QStringLiteral("%1 %2").arg(method, url.path()); }
};

struct HttpResponse {
    int statusCode = 0;
    QByteArray body;
    QMap<QString, QString> headers;
    qint64 downloadedBytes = 0;
    /// Set when the exchange failed before a status was produced.
    ImmichError error;

    bool isSuccess() const { return error.isNull() && statusCode >= 200 && statusCode <= 299; }
    QString header(const QString &name) const;
    /// Filename advertised by `Content-Disposition`, when the server sent one.
    QString suggestedFilename() const;
};

/// The seam every request goes through.
///
/// An interface rather than a concrete class so the hermetic suite can assert on the
/// exact bytes the client puts on the wire without a server — the equivalent of the
/// macOS build's `StubURLProtocol`, but without having to replace a protocol stack.
class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpResponse send(const HttpRequest &request) = 0;
};

/// The live implementation, over `QNetworkAccessManager`.
///
/// Used synchronously from worker threads: a cycle is a sequence of dependent requests,
/// and expressing it as a callback chain would obscure the ordering that matters.
/// `QNetworkAccessManager` is thread-affine, so one is created per calling thread.
class NetworkTransport : public Transport {
public:
    explicit NetworkTransport(TlsCertificateStore *certificates = nullptr);
    ~NetworkTransport() override;

    HttpResponse send(const HttpRequest &request) override;

    /// Drops pooled connections so a certificate that was just imported or removed
    /// takes effect on the next request rather than whenever the current connection
    /// happens to idle out.
    void resetConnections();

    /// Aborts every in-flight request and makes later ones fail immediately.
    ///
    /// Called on shutdown. A cycle in the middle of a large download would otherwise
    /// keep the process alive long after its window closed — which is exactly what let
    /// the app survive its own Quit.
    void cancelAll();
    bool isCancelled() const;

    /// Sent so sessions and audit logs identify this client the way the mobile and web
    /// clients identify themselves.
    static QString userAgent();

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace immichksync
