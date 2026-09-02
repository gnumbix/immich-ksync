#pragma once

#include "credentials/CertificateSummary.h"
#include "credentials/SecretStore.h"

#include <QMutex>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QString>

#include <optional>

namespace immichksync {

/// Owns the TLS material the app presents to, and accepts from, the Immich server.
///
/// Two independent things live here. A **client identity** (PKCS#12) for servers that
/// demand mutual TLS, and a **private certificate authority** for servers whose
/// certificate the system does not already trust. Neither is scoped to a server
/// address: see `SecretStore::GlobalSlot` for why they outlive Sign Out.
///
/// There is deliberately no method anywhere in this class that disables verification.
/// The authority is *added* to the system anchors, never substituted for them, and the
/// hostname and validity period are still checked in full.
class TlsCertificateStore {
public:
    explicit TlsCertificateStore(SecretStore *secrets);

    /// Loads both items into memory. Called once at startup so the first request does
    /// not pay for a keyring round trip.
    void warm();

    // MARK: - Reads

    std::optional<CertificateSummary> installedClientCertificate();
    std::optional<CertificateSummary> installedCertificateAuthority();

    /// Applies whatever is configured to `configuration`, for a connection to `host`.
    ///
    /// Returns false when nothing needed to change, which lets the caller keep Qt's
    /// default configuration rather than a materialised copy of it.
    bool apply(QSslConfiguration &configuration, const QString &host);

    // MARK: - Trusted hosts

    /// The hosts the extra anchor may be used for. Pushed in from the settings layer,
    /// because the transport must not read the config file from a worker thread.
    void setTrustedHosts(const QSet<QString> &hosts);
    QSet<QString> trustedHosts() const;

    /// Whether the extra anchor applies to `host`.
    ///
    /// Exact match, case-insensitively — never a prefix, suffix or substring test. A
    /// `contains` test would trust the anchor for `example.com` when the user
    /// configured `immich.example.com`; the reverse test is worse still, accepting
    /// `immich.example.com.attacker.net`.
    ///
    /// The port is deliberately not part of the match: moving a server to another port
    /// does not change whose certificate it is.
    static bool isTrusted(const QString &host, const QSet<QString> &hosts);

    // MARK: - Import and removal

    struct ImportResult {
        std::optional<CertificateSummary> summary;
        CertificateImportError error;
        bool succeeded() const { return summary.has_value(); }
    };

    ImportResult importClientCertificate(const QByteArray &pkcs12, const QString &passphrase);
    ImportResult importCertificateAuthority(const QByteArray &fileContents);
    void removeClientCertificate();
    void removeCertificateAuthority();

    // MARK: - Parsing (pure, so the tests can reach it without a keyring)

    struct Identity {
        QSslKey key;
        QSslCertificate certificate;
        QList<QSslCertificate> chain;
    };

    /// Materialises an identity from a PKCS#12 blob without storing anything.
    static std::optional<Identity> identityFromPkcs12(const QByteArray &data,
                                                      const QString &passphrase,
                                                      CertificateImportError *error);

    /// Accepts DER bytes or PEM armour, returning the single certificate inside.
    ///
    /// The same certificate authority is handed out in both forms and the extension is
    /// no guide — `.crt` is used for each — so the format is detected from the content.
    static std::optional<QSslCertificate> singleCertificate(const QByteArray &fileContents,
                                                            CertificateImportError *error);

private:
    void loadClientCertificate();
    void loadCertificateAuthority();

    SecretStore *m_secrets;

    mutable QMutex m_mutex;
    /// Serialises the keyring reads themselves, separately from the state above.
    ///
    /// Four transfer threads reaching an unloaded store at once would otherwise each
    /// read the keyring, and on a locked one that means four unlock prompts. Waiting
    /// behind whichever thread got there first is slower for the losers and correct.
    QMutex m_loadMutex;
    bool m_clientLoaded = false;
    bool m_authorityLoaded = false;
    std::optional<Identity> m_clientIdentity;
    std::optional<CertificateSummary> m_clientSummary;
    std::optional<QSslCertificate> m_authority;
    std::optional<CertificateSummary> m_authoritySummary;
    QSet<QString> m_trustedHosts;
};

} // namespace immichksync
