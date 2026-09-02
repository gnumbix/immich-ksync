#pragma once

#include "core/Retry.h"

#include <QNetworkReply>
#include <QString>

#include <optional>

namespace immichksync {

/// Every way a request to Immich can fail, with the retry decision attached.
///
/// TLS failures are pulled out of the generic transport bucket because they are
/// configuration problems rather than weather. Retrying one four times with backoff
/// turns a one-line fix into a two-minute wait followed by a message that says nothing
/// useful about what to change.
class ImmichError {
public:
    enum class Kind {
        None,
        /// The configured server address is not a usable http(s) URL.
        InvalidServerUrl,
        /// Networking failed before any HTTP status was produced.
        Transport,
        /// The server answered with a non-2xx status.
        Http,
        /// A 2xx body did not match the expected schema.
        Decoding,
        /// Something answered, but it was not the Immich API — typically an SSO portal
        /// or a reverse proxy returning an HTML login page.
        NotAnImmichServer,
        /// No credentials are configured.
        MissingCredentials,
        /// The server asked for a TLS client certificate and none is installed.
        ClientCertificateRequired,
        /// The server refused the TLS client certificate that is installed.
        ClientCertificateRejected,
        /// The server's own TLS certificate could not be validated.
        ServerCertificateUntrusted,
        /// The local side could not produce the request (e.g. no disk space to stage).
        Local,
    };

    ImmichError() = default;

    static ImmichError invalidServerUrl(const QString &value);
    static ImmichError transport(QNetworkReply::NetworkError code, const QString &detail);
    static ImmichError http(int status,
                            const QString &endpoint,
                            const QString &serverMessage,
                            std::optional<Milliseconds> retryAfter);
    static ImmichError decoding(const QString &endpoint, const QString &detail);
    static ImmichError notAnImmichServer(const QString &endpoint);
    static ImmichError missingCredentials();
    static ImmichError clientCertificateRequired();
    static ImmichError clientCertificateRejected();
    static ImmichError serverCertificateUntrusted(const QString &host, const QString &detail);
    static ImmichError local(const QString &message);

    /// Maps a Qt network error onto the most specific case available.
    static ImmichError fromNetworkError(QNetworkReply::NetworkError code,
                                        const QString &detail,
                                        const QString &host);

    bool isNull() const { return m_kind == Kind::None; }
    Kind kind() const { return m_kind; }
    int httpStatus() const { return m_status; }

    bool isRetryable() const;
    std::optional<Milliseconds> retryAfter() const { return m_retryAfter; }

    /// True when the failure means "these credentials will never work", so the sync
    /// engine can stop and surface a settings problem instead of retrying forever.
    bool isAuthenticationFailure() const;

    QString message() const;

private:
    Kind m_kind = Kind::None;
    int m_status = 0;
    QNetworkReply::NetworkError m_networkError = QNetworkReply::NoError;
    QString m_endpoint;
    QString m_detail;
    QString m_host;
    std::optional<Milliseconds> m_retryAfter;
};

/// Parses `Retry-After`, which may be either a delay in seconds or an HTTP date.
/// Capped at five minutes so a hostile or confused header cannot stall a cycle.
std::optional<Milliseconds> parseRetryAfter(const QString &raw);

} // namespace immichksync
