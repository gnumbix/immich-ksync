#include "credentials/TlsCertificateStore.h"

#include "core/Logging.h"

#include <QBuffer>
#include <QMutexLocker>

namespace immichksync {

TlsCertificateStore::TlsCertificateStore(SecretStore *secrets)
    : m_secrets(secrets)
{
}

void TlsCertificateStore::warm()
{
    loadClientCertificate();
    loadCertificateAuthority();
}

// MARK: - Loading

void TlsCertificateStore::loadClientCertificate()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_clientLoaded) {
            return;
        }
    }

    // A separate lock from the state one: the read can sit behind an unlock prompt for
    // as long as the user takes to answer it, and holding the state lock across that
    // would block every reader. This one only makes concurrent arrivals queue up behind
    // a single keyring read instead of each starting their own.
    QMutexLocker loading(&m_loadMutex);
    {
        // Another thread may have finished the load while this one waited.
        QMutexLocker locker(&m_mutex);
        if (m_clientLoaded) {
            return;
        }
    }

    std::optional<QByteArray> blob;
    QString passphrase;
    if (m_secrets) {
        blob = m_secrets->readData(SecretStore::GlobalSlot::ClientCertificate);
        passphrase =
            m_secrets->readString(SecretStore::GlobalSlot::ClientCertificatePassphrase).value_or(QString());
    }

    std::optional<Identity> identity;
    std::optional<CertificateSummary> summary;
    if (blob) {
        CertificateImportError error;
        identity = identityFromPkcs12(*blob, passphrase, &error);
        if (identity) {
            summary = CertificateSummary::of(identity->certificate);
        } else {
            // Stored bytes that no longer import is a real state — a keyring restored
            // from another machine, say. Say so rather than failing silently later.
            log::credentials.error(QStringLiteral("The stored client certificate could not be "
                                                  "read: %1")
                                       .arg(error.message()));
        }
    }

    QMutexLocker locker(&m_mutex);
    m_clientIdentity = identity;
    m_clientSummary = summary;
    m_clientLoaded = true;
}

void TlsCertificateStore::loadCertificateAuthority()
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_authorityLoaded) {
            return;
        }
    }

    QMutexLocker loading(&m_loadMutex);
    {
        QMutexLocker locker(&m_mutex);
        if (m_authorityLoaded) {
            return;
        }
    }

    std::optional<QByteArray> der;
    if (m_secrets) {
        der = m_secrets->readData(SecretStore::GlobalSlot::CertificateAuthority);
    }

    std::optional<QSslCertificate> authority;
    std::optional<CertificateSummary> summary;
    if (der) {
        const QSslCertificate certificate(*der, QSsl::Der);
        if (!certificate.isNull()) {
            authority = certificate;
            summary = CertificateSummary::of(certificate);
        } else {
            log::credentials.error(
                QStringLiteral("The stored certificate authority could not be read."));
        }
    }

    QMutexLocker locker(&m_mutex);
    m_authority = authority;
    m_authoritySummary = summary;
    m_authorityLoaded = true;
}

// MARK: - Reads

std::optional<CertificateSummary> TlsCertificateStore::installedClientCertificate()
{
    loadClientCertificate();
    QMutexLocker locker(&m_mutex);
    return m_clientSummary;
}

std::optional<CertificateSummary> TlsCertificateStore::installedCertificateAuthority()
{
    loadCertificateAuthority();
    QMutexLocker locker(&m_mutex);
    return m_authoritySummary;
}

bool TlsCertificateStore::apply(QSslConfiguration &configuration, const QString &host)
{
    bool changed = false;

    loadClientCertificate();
    {
        QMutexLocker locker(&m_mutex);
        if (m_clientIdentity) {
            configuration.setLocalCertificate(m_clientIdentity->certificate);
            configuration.setPrivateKey(m_clientIdentity->key);
            if (!m_clientIdentity->chain.isEmpty()) {
                QList<QSslCertificate> chain{m_clientIdentity->certificate};
                chain += m_clientIdentity->chain;
                configuration.setLocalCertificateChain(chain);
            }
            changed = true;
        }
    }

    // Check the host first: it costs nothing and means an unconfigured app never pays
    // for a keyring read while connecting to an ordinary public server.
    if (!isTrusted(host, trustedHosts())) {
        return changed;
    }

    loadCertificateAuthority();
    QMutexLocker locker(&m_mutex);
    if (m_authority) {
        // Added to the system anchors, never substituted for them. Replacing the list
        // would break the server the day it moves to a publicly issued certificate,
        // and would silently distrust every other host this configuration touches.
        QList<QSslCertificate> anchors = QSslConfiguration::systemCaCertificates();
        anchors.append(*m_authority);
        configuration.setCaCertificates(anchors);
        changed = true;
    }
    return changed;
}

// MARK: - Trusted hosts

void TlsCertificateStore::setTrustedHosts(const QSet<QString> &hosts)
{
    QMutexLocker locker(&m_mutex);
    m_trustedHosts = hosts;
}

QSet<QString> TlsCertificateStore::trustedHosts() const
{
    QMutexLocker locker(&m_mutex);
    return m_trustedHosts;
}

bool TlsCertificateStore::isTrusted(const QString &host, const QSet<QString> &hosts)
{
    if (host.isEmpty()) {
        return false;
    }
    for (const QString &candidate : hosts) {
        // Qt::CaseInsensitive rather than toLower(), which is locale-sensitive and
        // folds `I` to a dotless `ı` under a Turkish locale.
        if (candidate.compare(host, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// MARK: - Import and removal

TlsCertificateStore::ImportResult
TlsCertificateStore::importClientCertificate(const QByteArray &pkcs12, const QString &passphrase)
{
    ImportResult result;

    // Validate before storing, so a mistyped passphrase never displaces a working
    // certificate.
    CertificateImportError error;
    const auto identity = identityFromPkcs12(pkcs12, passphrase, &error);
    if (!identity) {
        result.error = error;
        return result;
    }

    QString storageError;
    if (!m_secrets
        || !m_secrets->writeData(pkcs12, SecretStore::GlobalSlot::ClientCertificate, &storageError)
        || !m_secrets->write(passphrase,
                             SecretStore::GlobalSlot::ClientCertificatePassphrase,
                             &storageError)) {
        result.error = CertificateImportError::storageFailed(storageError);
        return result;
    }

    const CertificateSummary summary = CertificateSummary::of(identity->certificate);
    {
        QMutexLocker locker(&m_mutex);
        m_clientIdentity = identity;
        m_clientSummary = summary;
        m_clientLoaded = true;
    }
    result.summary = summary;
    return result;
}

TlsCertificateStore::ImportResult
TlsCertificateStore::importCertificateAuthority(const QByteArray &fileContents)
{
    ImportResult result;

    CertificateImportError error;
    const auto certificate = singleCertificate(fileContents, &error);
    if (!certificate) {
        result.error = error;
        return result;
    }

    const QByteArray der = certificate->toDer();
    QString storageError;
    if (!m_secrets
        || !m_secrets->writeData(der, SecretStore::GlobalSlot::CertificateAuthority, &storageError)) {
        result.error = CertificateImportError::storageFailed(storageError);
        return result;
    }

    const CertificateSummary summary = CertificateSummary::of(*certificate);
    {
        QMutexLocker locker(&m_mutex);
        m_authority = certificate;
        m_authoritySummary = summary;
        m_authorityLoaded = true;
    }
    result.summary = summary;
    return result;
}

void TlsCertificateStore::removeClientCertificate()
{
    if (m_secrets) {
        m_secrets->remove(SecretStore::GlobalSlot::ClientCertificate);
        m_secrets->remove(SecretStore::GlobalSlot::ClientCertificatePassphrase);
    }
    QMutexLocker locker(&m_mutex);
    m_clientIdentity.reset();
    m_clientSummary.reset();
    m_clientLoaded = true;
}

void TlsCertificateStore::removeCertificateAuthority()
{
    if (m_secrets) {
        m_secrets->remove(SecretStore::GlobalSlot::CertificateAuthority);
    }
    QMutexLocker locker(&m_mutex);
    m_authority.reset();
    m_authoritySummary.reset();
    m_authorityLoaded = true;
}

// MARK: - Parsing

std::optional<TlsCertificateStore::Identity>
TlsCertificateStore::identityFromPkcs12(const QByteArray &data,
                                        const QString &passphrase,
                                        CertificateImportError *error)
{
    const auto fail = [error](CertificateImportError value) {
        if (error) {
            *error = value;
        }
        return std::nullopt;
    };

    if (data.isEmpty()) {
        return fail(CertificateImportError::notAPkcs12File());
    }

    QByteArray mutableData = data;
    QBuffer buffer(&mutableData);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return fail(CertificateImportError::notAPkcs12File());
    }

    Identity identity;
    if (!QSslCertificate::importPkcs12(&buffer,
                                       &identity.key,
                                       &identity.certificate,
                                       &identity.chain,
                                       passphrase.toUtf8())) {
        // Qt gives one boolean for "wrong passphrase" and "not a PKCS#12 file" alike.
        // An empty passphrase against a real file is overwhelmingly the former, and
        // telling someone to check the passphrase they just typed is the more useful
        // guess; a genuinely malformed file usually fails the DER sniff below too.
        const bool looksLikeDer = static_cast<unsigned char>(data.at(0)) == 0x30;
        return fail(looksLikeDer ? CertificateImportError::wrongPassphrase()
                                 : CertificateImportError::notAPkcs12File());
    }

    if (identity.certificate.isNull() || identity.key.isNull()) {
        return fail(CertificateImportError::noIdentityInFile());
    }
    if (error) {
        *error = CertificateImportError();
    }
    return identity;
}

std::optional<QSslCertificate>
TlsCertificateStore::singleCertificate(const QByteArray &fileContents,
                                       CertificateImportError *error)
{
    const auto fail = [error](CertificateImportError value) {
        if (error) {
            *error = value;
        }
        return std::nullopt;
    };

    if (fileContents.isEmpty()) {
        return fail(CertificateImportError::notACertificateFile());
    }

    // A DER certificate is an ASN.1 SEQUENCE, so it always begins 0x30. PEM armour
    // begins with '-'.
    if (static_cast<unsigned char>(fileContents.at(0)) == 0x30) {
        const QSslCertificate certificate(fileContents, QSsl::Der);
        if (certificate.isNull()) {
            return fail(CertificateImportError::notACertificateFile());
        }
        if (error) {
            *error = CertificateImportError();
        }
        return certificate;
    }

    const QList<QSslCertificate> certificates =
        QSslCertificate::fromData(fileContents, QSsl::Pem);
    if (certificates.isEmpty()) {
        return fail(CertificateImportError::notACertificateFile());
    }
    if (certificates.size() > 1) {
        return fail(CertificateImportError::multipleCertificates(
            static_cast<int>(certificates.size())));
    }
    if (certificates.first().isNull()) {
        return fail(CertificateImportError::notACertificateFile());
    }
    if (error) {
        *error = CertificateImportError();
    }
    return certificates.first();
}

} // namespace immichksync
