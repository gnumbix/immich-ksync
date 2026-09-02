#include "credentials/CertificateSummary.h"

#include <QCryptographicHash>
#include <QLocale>

namespace immichksync {

namespace {

QString formatFingerprint(const QByteArray &digest)
{
    QStringList parts;
    const QByteArray hex = digest.toHex().toUpper();
    for (int i = 0; i < hex.size(); i += 2) {
        parts.append(QString::fromLatin1(hex.mid(i, 2)));
    }
    return parts.join(QLatin1Char(':'));
}

QString firstOrEmpty(const QStringList &values)
{
    return values.isEmpty() ? QString() : values.first();
}

} // namespace

CertificateSummary CertificateSummary::of(const QSslCertificate &certificate)
{
    CertificateSummary summary;
    summary.commonName = firstOrEmpty(certificate.subjectInfo(QSslCertificate::CommonName));
    if (summary.commonName.isEmpty()) {
        // Some certificates carry the name only in a SAN; showing the raw DN beats
        // showing nothing at all.
        summary.commonName = firstOrEmpty(certificate.subjectInfo(QSslCertificate::Organization));
    }
    if (summary.commonName.isEmpty()) {
        summary.commonName = QStringLiteral("(unnamed certificate)");
    }
    summary.issuer = firstOrEmpty(certificate.issuerInfo(QSslCertificate::CommonName));
    summary.notBefore = certificate.effectiveDate();
    summary.notAfter = certificate.expiryDate();
    summary.fingerprint = formatFingerprint(certificate.digest(QCryptographicHash::Sha256));
    return summary;
}

bool CertificateSummary::isExpired() const
{
    return notAfter.isValid() && notAfter < QDateTime::currentDateTimeUtc();
}

bool CertificateSummary::isNotYetValid() const
{
    return notBefore.isValid() && notBefore > QDateTime::currentDateTimeUtc();
}

QString CertificateSummary::validityDescription() const
{
    const QLocale locale;
    if (isExpired()) {
        return QStringLiteral("Expired %1").arg(locale.toString(notAfter.date(), QLocale::ShortFormat));
    }
    if (isNotYetValid()) {
        return QStringLiteral("Not valid until %1")
            .arg(locale.toString(notBefore.date(), QLocale::ShortFormat));
    }
    if (!notAfter.isValid()) {
        return QStringLiteral("No expiry date");
    }
    return QStringLiteral("Valid until %1").arg(locale.toString(notAfter.date(), QLocale::ShortFormat));
}

bool CertificateSummary::operator==(const CertificateSummary &other) const
{
    return fingerprint == other.fingerprint;
}

// MARK: - CertificateImportError

CertificateImportError CertificateImportError::unreadableFile(const QString &name,
                                                              const QString &reason)
{
    CertificateImportError error;
    error.m_kind = Kind::UnreadableFile;
    error.m_detail = QStringLiteral("%1: %2").arg(name, reason);
    return error;
}

CertificateImportError CertificateImportError::notAPkcs12File()
{
    CertificateImportError error;
    error.m_kind = Kind::NotAPkcs12File;
    return error;
}

CertificateImportError CertificateImportError::wrongPassphrase()
{
    CertificateImportError error;
    error.m_kind = Kind::WrongPassphrase;
    return error;
}

CertificateImportError CertificateImportError::noIdentityInFile()
{
    CertificateImportError error;
    error.m_kind = Kind::NoIdentityInFile;
    return error;
}

CertificateImportError CertificateImportError::notACertificateFile()
{
    CertificateImportError error;
    error.m_kind = Kind::NotACertificateFile;
    return error;
}

CertificateImportError CertificateImportError::multipleCertificates(int count)
{
    CertificateImportError error;
    error.m_kind = Kind::MultipleCertificates;
    error.m_count = count;
    return error;
}

CertificateImportError CertificateImportError::storageFailed(const QString &reason)
{
    CertificateImportError error;
    error.m_kind = Kind::StorageFailed;
    error.m_detail = reason;
    return error;
}

QString CertificateImportError::message() const
{
    switch (m_kind) {
    case Kind::None:
        return {};
    case Kind::UnreadableFile:
        return QStringLiteral("That file could not be read — %1").arg(m_detail);
    case Kind::NotAPkcs12File:
        return QStringLiteral("That does not look like a PKCS#12 file. It is usually named "
                              ".p12 or .pfx.");
    case Kind::WrongPassphrase:
        return QStringLiteral("The passphrase does not match that file.");
    case Kind::NoIdentityInFile:
        return QStringLiteral("That file contains no certificate and private key pair, so it "
                              "cannot be used to identify this machine.");
    case Kind::NotACertificateFile:
        return QStringLiteral("That does not look like a certificate. Both PEM and DER are "
                              "accepted.");
    case Kind::MultipleCertificates:
        return QStringLiteral("That file contains %1 certificates. Choose the file containing "
                              "only the certificate authority itself — anchoring the first "
                              "entry of a chain would usually anchor an intermediate, which is "
                              "weaker than it looks.")
            .arg(m_count);
    case Kind::StorageFailed:
        return QStringLiteral("The certificate was valid but could not be stored: %1").arg(m_detail);
    }
    return {};
}

} // namespace immichksync
