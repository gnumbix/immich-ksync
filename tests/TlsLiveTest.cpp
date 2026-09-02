#include "credentials/SecretStore.h"
#include "credentials/TlsCertificateStore.h"
#include "immich/Transport.h"

#include <QFile>
#include <QTest>

using namespace immichksync;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(IMMICHKSYNC_FIXTURE_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QUrl serverUrl()
{
    const QByteArray port = qgetenv("IMMICH_TEST_TLS_PORT");
    return QUrl(QStringLiteral("https://localhost:%1/")
                    .arg(port.isEmpty() ? QStringLiteral("24433") : QString::fromLatin1(port)));
}

constexpr const char *kPassphrase = "test-passphrase";

} // namespace

/// Proves a certificate actually reaches the wire.
///
/// Opt-in, because it needs the throwaway server from tools/run-tls-test-server.sh:
///
///   ./tools/run-tls-test-server.sh &
///   make test-tls
class TlsLiveTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        if (qgetenv("IMMICH_TEST_TLS").isEmpty()) {
            QSKIP("Set IMMICH_TEST_TLS=1 and start tools/run-tls-test-server.sh to run these.");
        }
    }

    /// Without a client certificate the server refuses the handshake. This is the
    /// negative control: if it passes, the positive test below proves nothing.
    void handshakeFailsWithNoClientCertificate()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore certificates(&secrets);
        // The server's own certificate is privately signed, so the CA still has to be
        // anchored — otherwise this would fail for the wrong reason.
        QVERIFY(certificates.importCertificateAuthority(fixture(QStringLiteral("ca.crt")))
                    .succeeded());
        certificates.setTrustedHosts({QStringLiteral("localhost")});

        NetworkTransport transport(&certificates);
        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 10;

        const HttpResponse response = transport.send(request);
        QVERIFY2(!response.isSuccess(), "the server should have refused a certificate-less client");
    }

    /// The positive case: with the identity installed, the handshake completes.
    void handshakeSucceedsWithTheClientCertificate()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore certificates(&secrets);
        QVERIFY(certificates.importCertificateAuthority(fixture(QStringLiteral("ca.crt")))
                    .succeeded());
        QVERIFY(certificates
                    .importClientCertificate(fixture(QStringLiteral("client.p12")),
                                             QString::fromLatin1(kPassphrase))
                    .succeeded());
        certificates.setTrustedHosts({QStringLiteral("localhost")});

        NetworkTransport transport(&certificates);
        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 10;

        const HttpResponse response = transport.send(request);
        QVERIFY2(response.error.isNull(), qUtf8Printable(response.error.message()));
        QCOMPARE(response.statusCode, 200);
    }

    /// Without the private CA anchored, the server's own certificate is untrusted —
    /// which is the whole point: this feature adds an anchor, it does not disable
    /// verification.
    void serverCertificateIsRejectedWithoutTheAnchor()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore certificates(&secrets);
        QVERIFY(certificates
                    .importClientCertificate(fixture(QStringLiteral("client.p12")),
                                             QString::fromLatin1(kPassphrase))
                    .succeeded());
        // Deliberately no CA import, and no trusted hosts.

        NetworkTransport transport(&certificates);
        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 10;

        const HttpResponse response = transport.send(request);
        QVERIFY(!response.isSuccess());
        QCOMPARE(response.error.kind(), ImmichError::Kind::ServerCertificateUntrusted);
    }

    /// Anchoring the CA for a *different* host must not help this connection: the
    /// anchor is scoped to one address and no other.
    void theAnchorDoesNotApplyToAnotherHost()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore certificates(&secrets);
        QVERIFY(certificates.importCertificateAuthority(fixture(QStringLiteral("ca.crt")))
                    .succeeded());
        QVERIFY(certificates
                    .importClientCertificate(fixture(QStringLiteral("client.p12")),
                                             QString::fromLatin1(kPassphrase))
                    .succeeded());
        certificates.setTrustedHosts({QStringLiteral("immich.example.com")});

        NetworkTransport transport(&certificates);
        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 10;

        const HttpResponse response = transport.send(request);
        QVERIFY2(!response.isSuccess(),
                 "the anchor must not apply to a host it was not configured for");
    }
};

QTEST_MAIN(TlsLiveTest)
#include "TlsLiveTest.moc"
