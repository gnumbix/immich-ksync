#include "credentials/SecretStore.h"
#include "credentials/TlsCertificateStore.h"

#include <QFile>
#include <QSslConfiguration>
#include <QTest>

using namespace immichksync;

namespace {

QByteArray fixture(const QString &name)
{
    QFile file(QStringLiteral(IMMICHKSYNC_FIXTURE_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

constexpr const char *kPassphrase = "test-passphrase";

} // namespace

/// The security-critical half of the app. Two things must hold no matter what:
/// the extra anchor applies to exactly one host and no other, and there is no path
/// anywhere that accepts a certificate the system would have rejected.
class TlsCertificateStoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(!fixture(QStringLiteral("client.p12")).isEmpty(),
                 "run tools/make-test-certificates.sh first");
    }

    // MARK: - Host matching
    //
    // The legacy Immich client used `configured.contains(challengeHost)`, which trusts
    // the anchor for `example.com` when the user configured `immich.example.com`. The
    // reverse test is worse still, accepting `immich.example.com.attacker.net`.

    void matchesTheConfiguredHostExactly()
    {
        const QSet<QString> hosts{QStringLiteral("immich.example.com")};
        QVERIFY(TlsCertificateStore::isTrusted(QStringLiteral("immich.example.com"), hosts));
    }

    void matchesCaseInsensitively()
    {
        const QSet<QString> hosts{QStringLiteral("immich.example.com")};
        QVERIFY(TlsCertificateStore::isTrusted(QStringLiteral("IMMICH.EXAMPLE.COM"), hosts));
        QVERIFY(TlsCertificateStore::isTrusted(QStringLiteral("Immich.Example.Com"), hosts));
    }

    void rejectsAParentDomain()
    {
        const QSet<QString> hosts{QStringLiteral("immich.example.com")};
        QVERIFY(!TlsCertificateStore::isTrusted(QStringLiteral("example.com"), hosts));
    }

    void rejectsASuffixedImpostor()
    {
        const QSet<QString> hosts{QStringLiteral("immich.example.com")};
        QVERIFY(
            !TlsCertificateStore::isTrusted(QStringLiteral("immich.example.com.attacker.net"),
                                            hosts));
    }

    void rejectsASubdomain()
    {
        const QSet<QString> hosts{QStringLiteral("example.com")};
        QVERIFY(!TlsCertificateStore::isTrusted(QStringLiteral("immich.example.com"), hosts));
    }

    void rejectsAnEmptyOrUnknownHost()
    {
        const QSet<QString> hosts{QStringLiteral("immich.example.com")};
        QVERIFY(!TlsCertificateStore::isTrusted(QString(), hosts));
        QVERIFY(!TlsCertificateStore::isTrusted(QStringLiteral("other.example.com"), hosts));
        QVERIFY(!TlsCertificateStore::isTrusted(QStringLiteral("immich.example.com"), {}));
    }

    // MARK: - PKCS#12

    void importsAClientIdentity()
    {
        CertificateImportError error;
        const auto identity =
            TlsCertificateStore::identityFromPkcs12(fixture(QStringLiteral("client.p12")),
                                                    QString::fromLatin1(kPassphrase),
                                                    &error);
        QVERIFY2(identity.has_value(), qUtf8Printable(error.message()));
        QVERIFY(!identity->certificate.isNull());
        QVERIFY(!identity->key.isNull());
        QVERIFY(identity->certificate.subjectInfo(QSslCertificate::CommonName)
                    .contains(QStringLiteral("ImmichKSync Test Client")));
    }

    void rejectsAWrongPassphrase()
    {
        CertificateImportError error;
        const auto identity =
            TlsCertificateStore::identityFromPkcs12(fixture(QStringLiteral("client.p12")),
                                                    QStringLiteral("wrong"),
                                                    &error);
        QVERIFY(!identity.has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::WrongPassphrase);
        QVERIFY(error.message().contains(QStringLiteral("passphrase")));
    }

    void rejectsSomethingThatIsNotAPkcs12File()
    {
        CertificateImportError error;
        QVERIFY(!TlsCertificateStore::identityFromPkcs12(QByteArray("hello"), QString(), &error)
                     .has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::NotAPkcs12File);
    }

    void rejectsAnEmptyPkcs12File()
    {
        CertificateImportError error;
        QVERIFY(!TlsCertificateStore::identityFromPkcs12(QByteArray(), QString(), &error).has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::NotAPkcs12File);
    }

    // MARK: - Certificate authority parsing

    void acceptsAPemCertificate()
    {
        CertificateImportError error;
        const auto certificate =
            TlsCertificateStore::singleCertificate(fixture(QStringLiteral("ca.crt")), &error);
        QVERIFY2(certificate.has_value(), qUtf8Printable(error.message()));
        QVERIFY(certificate->subjectInfo(QSslCertificate::CommonName)
                    .contains(QStringLiteral("ImmichKSync Test CA")));
    }

    /// The extension is no guide — `.crt` is used for both forms — so the format has to
    /// be detected from the content.
    void acceptsADerCertificate()
    {
        const auto pem =
            TlsCertificateStore::singleCertificate(fixture(QStringLiteral("ca.crt")), nullptr);
        QVERIFY(pem.has_value());

        CertificateImportError error;
        const auto der = TlsCertificateStore::singleCertificate(pem->toDer(), &error);
        QVERIFY2(der.has_value(), qUtf8Printable(error.message()));
        QCOMPARE(der->digest(QCryptographicHash::Sha256), pem->digest(QCryptographicHash::Sha256));
    }

    /// Server chains run leaf to root, so anchoring "the first" entry would usually
    /// anchor an intermediate — a weaker anchor than the user believes they installed.
    void refusesToAnchorAChainFile()
    {
        CertificateImportError error;
        const auto certificate =
            TlsCertificateStore::singleCertificate(fixture(QStringLiteral("chain.pem")), &error);
        QVERIFY(!certificate.has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::MultipleCertificates);
        QVERIFY(error.message().contains(QStringLiteral("2 certificates")));
    }

    void rejectsSomethingThatIsNotACertificate()
    {
        CertificateImportError error;
        QVERIFY(!TlsCertificateStore::singleCertificate(QByteArray("hello"), &error).has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::NotACertificateFile);

        QVERIFY(!TlsCertificateStore::singleCertificate(QByteArray(), &error).has_value());
        QCOMPARE(error.kind(), CertificateImportError::Kind::NotACertificateFile);
    }

    // MARK: - Storage round trips

    void storesAndReloadsAClientCertificate()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);

        QVERIFY(!store.installedClientCertificate().has_value());

        const auto result = store.importClientCertificate(fixture(QStringLiteral("client.p12")),
                                                          QString::fromLatin1(kPassphrase));
        QVERIFY2(result.succeeded(), qUtf8Printable(result.error.message()));
        QVERIFY(result.summary->commonName.contains(QStringLiteral("Test Client")));
        QVERIFY(!result.summary->fingerprint.isEmpty());

        // A fresh store over the same secrets must find it again.
        TlsCertificateStore reloaded(&secrets);
        QVERIFY(reloaded.installedClientCertificate().has_value());
    }

    void storesAndReloadsACertificateAuthority()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);

        const auto result = store.importCertificateAuthority(fixture(QStringLiteral("ca.crt")));
        QVERIFY2(result.succeeded(), qUtf8Printable(result.error.message()));

        TlsCertificateStore reloaded(&secrets);
        const auto summary = reloaded.installedCertificateAuthority();
        QVERIFY(summary.has_value());
        QCOMPARE(summary->fingerprint, result.summary->fingerprint);
    }

    /// A mistyped passphrase must never displace a working certificate.
    void aFailedImportLeavesTheStoredOneAlone()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);

        QVERIFY(store.importClientCertificate(fixture(QStringLiteral("client.p12")),
                                              QString::fromLatin1(kPassphrase))
                    .succeeded());
        const auto before = store.installedClientCertificate();

        QVERIFY(!store.importClientCertificate(fixture(QStringLiteral("client.p12")),
                                               QStringLiteral("wrong"))
                     .succeeded());
        QCOMPARE(store.installedClientCertificate(), before);
    }

    void removesWhatItStored()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);

        QVERIFY(store.importClientCertificate(fixture(QStringLiteral("client.p12")),
                                              QString::fromLatin1(kPassphrase))
                    .succeeded());
        QVERIFY(store.importCertificateAuthority(fixture(QStringLiteral("ca.crt"))).succeeded());

        store.removeClientCertificate();
        store.removeCertificateAuthority();

        QVERIFY(!store.installedClientCertificate().has_value());
        QVERIFY(!store.installedCertificateAuthority().has_value());
        QVERIFY(!secrets.readData(SecretStore::GlobalSlot::ClientCertificate).has_value());
        QVERIFY(!secrets.readData(SecretStore::GlobalSlot::ClientCertificatePassphrase).has_value());
    }

    // MARK: - Applying to a connection

    void presentsTheClientCertificateToAnyHost()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);
        QVERIFY(store.importClientCertificate(fixture(QStringLiteral("client.p12")),
                                              QString::fromLatin1(kPassphrase))
                    .succeeded());

        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        QVERIFY(store.apply(configuration, QStringLiteral("anything.example.com")));
        QVERIFY(!configuration.localCertificate().isNull());
        QVERIFY(!configuration.privateKey().isNull());
    }

    void anchorsTheAuthorityOnlyForTheConfiguredHost()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);
        QVERIFY(store.importCertificateAuthority(fixture(QStringLiteral("ca.crt"))).succeeded());
        store.setTrustedHosts({QStringLiteral("immich.example.com")});

        QSslConfiguration configured = QSslConfiguration::defaultConfiguration();
        QVERIFY(store.apply(configured, QStringLiteral("immich.example.com")));
        QVERIFY(configured.caCertificates().size() > 1);

        QSslConfiguration other = QSslConfiguration::defaultConfiguration();
        QVERIFY(!store.apply(other, QStringLiteral("elsewhere.example.com")));
        QCOMPARE(other.caCertificates(), QSslConfiguration::defaultConfiguration().caCertificates());
    }

    /// Replacing the anchor list would break the server the day it moves to a publicly
    /// issued certificate, so the authority is added to the system set, never
    /// substituted for it.
    void addsTheAuthorityToTheSystemAnchorsRatherThanReplacingThem()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);
        QVERIFY(store.importCertificateAuthority(fixture(QStringLiteral("ca.crt"))).succeeded());
        store.setTrustedHosts({QStringLiteral("immich.example.com")});

        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        QVERIFY(store.apply(configuration, QStringLiteral("immich.example.com")));

        const auto systemAnchors = QSslConfiguration::systemCaCertificates();
        QCOMPARE(configuration.caCertificates().size(), systemAnchors.size() + 1);
        for (const QSslCertificate &anchor : systemAnchors) {
            QVERIFY(configuration.caCertificates().contains(anchor));
        }
    }

    /// Peer verification is never relaxed. If this ever changes, the whole feature has
    /// become a switch that accepts any certificate.
    void neverDisablesPeerVerification()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);
        QVERIFY(store.importCertificateAuthority(fixture(QStringLiteral("ca.crt"))).succeeded());
        store.setTrustedHosts({QStringLiteral("immich.example.com")});

        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        store.apply(configuration, QStringLiteral("immich.example.com"));
        QCOMPARE(configuration.peerVerifyMode(), QSslSocket::AutoVerifyPeer);
    }

    void appliesNothingWhenNothingIsInstalled()
    {
        SecretStore secrets(SecretStore::InMemory);
        TlsCertificateStore store(&secrets);
        store.setTrustedHosts({QStringLiteral("immich.example.com")});

        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        QVERIFY(!store.apply(configuration, QStringLiteral("immich.example.com")));
    }

    // MARK: - Summaries

    void summarisesACertificateForTheSettingsWindow()
    {
        const auto certificate =
            TlsCertificateStore::singleCertificate(fixture(QStringLiteral("ca.crt")), nullptr);
        QVERIFY(certificate.has_value());

        const CertificateSummary summary = CertificateSummary::of(*certificate);
        QVERIFY(summary.commonName.contains(QStringLiteral("ImmichKSync Test CA")));
        QVERIFY(!summary.isExpired());
        QVERIFY(!summary.isNotYetValid());
        QVERIFY(summary.validityDescription().startsWith(QStringLiteral("Valid until")));
        // Colon-separated uppercase SHA-256: 32 bytes → 95 characters.
        QCOMPARE(summary.fingerprint.size(), 95);
    }
};

QTEST_APPLESS_MAIN(TlsCertificateStoreTest)
#include "TlsCertificateStoreTest.moc"
