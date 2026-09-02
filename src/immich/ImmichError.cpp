#include "immich/ImmichError.h"

#include <QDateTime>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>

namespace immichksync {

ImmichError ImmichError::invalidServerUrl(const QString &value)
{
    ImmichError error;
    error.m_kind = Kind::InvalidServerUrl;
    error.m_detail = value;
    return error;
}

ImmichError ImmichError::transport(QNetworkReply::NetworkError code, const QString &detail)
{
    ImmichError error;
    error.m_kind = Kind::Transport;
    error.m_networkError = code;
    error.m_detail = detail;
    return error;
}

ImmichError ImmichError::http(int status,
                              const QString &endpoint,
                              const QString &serverMessage,
                              std::optional<Milliseconds> retryAfter)
{
    ImmichError error;
    error.m_kind = Kind::Http;
    error.m_status = status;
    error.m_endpoint = endpoint;
    error.m_detail = serverMessage;
    error.m_retryAfter = retryAfter;
    return error;
}

ImmichError ImmichError::decoding(const QString &endpoint, const QString &detail)
{
    ImmichError error;
    error.m_kind = Kind::Decoding;
    error.m_endpoint = endpoint;
    error.m_detail = detail;
    return error;
}

ImmichError ImmichError::notAnImmichServer(const QString &endpoint)
{
    ImmichError error;
    error.m_kind = Kind::NotAnImmichServer;
    error.m_endpoint = endpoint;
    return error;
}

ImmichError ImmichError::missingCredentials()
{
    ImmichError error;
    error.m_kind = Kind::MissingCredentials;
    return error;
}

ImmichError ImmichError::clientCertificateRequired()
{
    ImmichError error;
    error.m_kind = Kind::ClientCertificateRequired;
    return error;
}

ImmichError ImmichError::clientCertificateRejected()
{
    ImmichError error;
    error.m_kind = Kind::ClientCertificateRejected;
    return error;
}

ImmichError ImmichError::serverCertificateUntrusted(const QString &host, const QString &detail)
{
    ImmichError error;
    error.m_kind = Kind::ServerCertificateUntrusted;
    error.m_host = host;
    error.m_detail = detail;
    return error;
}

ImmichError ImmichError::local(const QString &message)
{
    ImmichError error;
    error.m_kind = Kind::Local;
    error.m_detail = message;
    return error;
}

ImmichError ImmichError::fromNetworkError(QNetworkReply::NetworkError code,
                                          const QString &detail,
                                          const QString &host)
{
    switch (code) {
    case QNetworkReply::SslHandshakeFailedError:
        // Qt collapses every handshake failure into this one code. It is reported as an
        // untrusted server certificate because that is overwhelmingly the cause, and
        // the message names both possibilities so the user knows where to look.
        return serverCertificateUntrusted(host, detail);
    default:
        return transport(code, detail);
    }
}

bool ImmichError::isRetryable() const
{
    switch (m_kind) {
    case Kind::Transport:
        switch (m_networkError) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::ServiceUnavailableError:
        case QNetworkReply::UnknownNetworkError:
            return true;
        default:
            return false;
        }
    case Kind::Http:
        // 408 Request Timeout, 429 Too Many Requests, and anything 5xx.
        return m_status == 408 || m_status == 429 || (m_status >= 500 && m_status <= 599);
    case Kind::None:
    case Kind::InvalidServerUrl:
    case Kind::Decoding:
    case Kind::NotAnImmichServer:
    case Kind::MissingCredentials:
    case Kind::ClientCertificateRequired:
    case Kind::ClientCertificateRejected:
    case Kind::ServerCertificateUntrusted:
    case Kind::Local:
        return false;
    }
    return false;
}

bool ImmichError::isAuthenticationFailure() const
{
    switch (m_kind) {
    case Kind::Http:
        return m_status == 401 || m_status == 403;
    case Kind::MissingCredentials:
        return true;
    // The engine uses this to stop and surface a settings problem rather than retry
    // forever, which is exactly right for a certificate that will never be accepted.
    // An untrusted *server* certificate is deliberately not here: it says nothing
    // about the account.
    case Kind::ClientCertificateRequired:
    case Kind::ClientCertificateRejected:
        return true;
    default:
        return false;
    }
}

QString ImmichError::message() const
{
    switch (m_kind) {
    case Kind::None:
        return {};
    case Kind::InvalidServerUrl:
        return QStringLiteral("“%1” is not a valid server address. Use something like "
                              "https://immich.example.com.")
            .arg(m_detail);
    case Kind::Transport:
        return QStringLiteral("Could not reach the server: %1").arg(m_detail);
    case Kind::Http:
        if (!m_detail.isEmpty()) {
            return QStringLiteral("%1 failed with HTTP %2: %3")
                .arg(m_endpoint)
                .arg(m_status)
                .arg(m_detail);
        }
        return QStringLiteral("%1 failed with HTTP %2.").arg(m_endpoint).arg(m_status);
    case Kind::Decoding:
        return QStringLiteral("Unexpected response from %1: %2").arg(m_endpoint, m_detail);
    case Kind::NotAnImmichServer:
        return QStringLiteral("%1 did not return an Immich API response. Check that the address "
                              "points at the Immich server and that nothing in front of it — a "
                              "reverse proxy or SSO portal — is answering instead.")
            .arg(m_endpoint);
    case Kind::MissingCredentials:
        return QStringLiteral("No credentials are configured. Open Settings ▸ Server to sign in.");
    case Kind::ClientCertificateRequired:
        return QStringLiteral("The server asked for a TLS client certificate and none is "
                              "installed. Import the .p12 you were issued in Settings ▸ Server.");
    case Kind::ClientCertificateRejected:
        return QStringLiteral("The server refused the TLS client certificate that is installed. "
                              "Check in Settings ▸ Server that it was issued by the certificate "
                              "authority the server trusts, and that it has not expired.");
    case Kind::ServerCertificateUntrusted:
        return QStringLiteral("The TLS connection to %1 could not be established: %2 If the "
                              "server's certificate was issued by a private certificate "
                              "authority, import that authority in Settings ▸ Server. If the "
                              "server asks for a client certificate, import that instead.")
            .arg(m_host.isEmpty() ? QStringLiteral("the server") : m_host, m_detail);
    case Kind::Local:
        return m_detail;
    }
    return {};
}

std::optional<Milliseconds> parseRetryAfter(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return std::nullopt;
    }

    bool isNumber = false;
    const double seconds = trimmed.toDouble(&isNumber);
    if (isNumber) {
        const double capped = std::clamp(seconds, 0.0, 300.0);
        return Milliseconds{static_cast<qint64>(capped * 1000)};
    }

    // The header may carry an HTTP-date instead of a delay.
    const QDateTime date =
        QLocale::c().toDateTime(trimmed, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!date.isValid()) {
        return std::nullopt;
    }
    QDateTime utc = date;
    utc.setTimeZone(QTimeZone::UTC);
    const qint64 deltaMs = QDateTime::currentDateTimeUtc().msecsTo(utc);
    return Milliseconds{std::clamp<qint64>(deltaMs, 0, 300000)};
}

} // namespace immichksync
