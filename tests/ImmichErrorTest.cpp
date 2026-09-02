#include "immich/ImmichError.h"

#include <QDateTime>
#include <QLocale>
#include <QTest>

using namespace immichksync;

/// What is worth retrying, and what is a configuration problem the user has to fix.
/// Getting this wrong either hammers a struggling server or hides a one-line mistake
/// behind two minutes of backoff.
class ImmichErrorTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void retriesTransientHttpStatuses()
    {
        for (const int status : {408, 429, 500, 502, 503, 504}) {
            const ImmichError error =
                ImmichError::http(status, QStringLiteral("GET /x"), QString(), std::nullopt);
            QVERIFY2(error.isRetryable(), qUtf8Printable(QStringLiteral("status %1").arg(status)));
        }
    }

    void doesNotRetryClientErrors()
    {
        for (const int status : {400, 401, 403, 404, 409, 422}) {
            const ImmichError error =
                ImmichError::http(status, QStringLiteral("GET /x"), QString(), std::nullopt);
            QVERIFY2(!error.isRetryable(), qUtf8Printable(QStringLiteral("status %1").arg(status)));
        }
    }

    void treatsUnauthorisedAsAnAuthenticationFailure()
    {
        QVERIFY(ImmichError::http(401, QStringLiteral("GET /x"), QString(), std::nullopt)
                    .isAuthenticationFailure());
        QVERIFY(ImmichError::http(403, QStringLiteral("GET /x"), QString(), std::nullopt)
                    .isAuthenticationFailure());
        QVERIFY(!ImmichError::http(500, QStringLiteral("GET /x"), QString(), std::nullopt)
                     .isAuthenticationFailure());
        QVERIFY(ImmichError::missingCredentials().isAuthenticationFailure());
    }

    /// A certificate the server will never accept is a settings problem, so the engine
    /// must stop rather than retry forever.
    void treatsClientCertificateProblemsAsAuthenticationFailures()
    {
        QVERIFY(ImmichError::clientCertificateRequired().isAuthenticationFailure());
        QVERIFY(ImmichError::clientCertificateRejected().isAuthenticationFailure());
        QVERIFY(!ImmichError::clientCertificateRequired().isRetryable());
        QVERIFY(!ImmichError::clientCertificateRejected().isRetryable());
    }

    /// An untrusted *server* certificate says nothing about the account, so it is
    /// deliberately not an authentication failure.
    void serverCertificateProblemsAreNotAuthenticationFailures()
    {
        const ImmichError error =
            ImmichError::serverCertificateUntrusted(QStringLiteral("immich.example.com"),
                                                    QStringLiteral("self signed"));
        QVERIFY(!error.isAuthenticationFailure());
        QVERIFY(!error.isRetryable());
    }

    void retriesTransientNetworkErrors()
    {
        for (const auto code : {QNetworkReply::ConnectionRefusedError,
                                QNetworkReply::TimeoutError,
                                QNetworkReply::HostNotFoundError,
                                QNetworkReply::TemporaryNetworkFailureError}) {
            QVERIFY(ImmichError::transport(code, QStringLiteral("x")).isRetryable());
        }
    }

    void doesNotRetryPermanentNetworkErrors()
    {
        for (const auto code : {QNetworkReply::ContentAccessDenied,
                                QNetworkReply::ContentNotFoundError,
                                QNetworkReply::ProtocolInvalidOperationError}) {
            QVERIFY(!ImmichError::transport(code, QStringLiteral("x")).isRetryable());
        }
    }

    /// Qt collapses every handshake failure into one code, so it must not be retried
    /// as if it were weather.
    void mapsHandshakeFailuresToACertificateProblem()
    {
        const ImmichError error =
            ImmichError::fromNetworkError(QNetworkReply::SslHandshakeFailedError,
                                          QStringLiteral("bad cert"),
                                          QStringLiteral("immich.example.com"));
        QCOMPARE(error.kind(), ImmichError::Kind::ServerCertificateUntrusted);
        QVERIFY(!error.isRetryable());
        QVERIFY(error.message().contains(QStringLiteral("immich.example.com")));
    }

    void neverRetriesDecodingOrProtocolConfusion()
    {
        QVERIFY(!ImmichError::decoding(QStringLiteral("GET /x"), QStringLiteral("bad")).isRetryable());
        QVERIFY(!ImmichError::notAnImmichServer(QStringLiteral("GET /x")).isRetryable());
        QVERIFY(!ImmichError::invalidServerUrl(QStringLiteral("nope")).isRetryable());
    }

    void messagesNameTheProblem()
    {
        QVERIFY(ImmichError::invalidServerUrl(QStringLiteral("nope"))
                    .message()
                    .contains(QStringLiteral("nope")));
        QVERIFY(ImmichError::http(404,
                                  QStringLiteral("GET /albums"),
                                  QStringLiteral("Not Found"),
                                  std::nullopt)
                    .message()
                    .contains(QStringLiteral("Not Found")));
        QVERIFY(ImmichError::notAnImmichServer(QStringLiteral("GET /server/ping"))
                    .message()
                    .contains(QStringLiteral("reverse proxy")));
        QVERIFY(ImmichError().message().isEmpty());
    }

    void carriesRetryAfterThrough()
    {
        const ImmichError error = ImmichError::http(429,
                                                    QStringLiteral("POST /assets"),
                                                    QString(),
                                                    Milliseconds{5000});
        QVERIFY(error.retryAfter().has_value());
        QCOMPARE(error.retryAfter()->count(), 5000);
    }

    void parsesRetryAfterAsSeconds()
    {
        QCOMPARE(parseRetryAfter(QStringLiteral("30"))->count(), 30000);
        QCOMPARE(parseRetryAfter(QStringLiteral(" 5 "))->count(), 5000);
        QVERIFY(!parseRetryAfter(QString()).has_value());
        QVERIFY(!parseRetryAfter(QStringLiteral("soon")).has_value());
    }

    /// A hostile or confused header must not be able to stall a cycle for a day.
    void capsRetryAfterAtFiveMinutes()
    {
        QCOMPARE(parseRetryAfter(QStringLiteral("86400"))->count(), 300000);
        QCOMPARE(parseRetryAfter(QStringLiteral("-10"))->count(), 0);
    }

    void parsesRetryAfterAsAnHttpDate()
    {
        const QDateTime future = QDateTime::currentDateTimeUtc().addSecs(60);
        const QString header =
            QLocale::c().toString(future, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
        const auto parsed = parseRetryAfter(header);
        QVERIFY(parsed.has_value());
        // Allow a couple of seconds of slack for the clock ticking during the test.
        QVERIFY(parsed->count() > 50000 && parsed->count() <= 60000);
    }

    void aDefaultErrorIsNull()
    {
        const ImmichError error;
        QVERIFY(error.isNull());
        QCOMPARE(error.kind(), ImmichError::Kind::None);
        QVERIFY(!error.isRetryable());
    }
};

QTEST_APPLESS_MAIN(ImmichErrorTest)
#include "ImmichErrorTest.moc"
