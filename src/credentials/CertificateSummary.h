#pragma once

#include <QDateTime>
#include <QSslCertificate>
#include <QString>

namespace immichksync {

/// What is installed, for the settings window.
///
/// Names the certificate rather than only reporting that one exists: the fingerprint
/// is what lets someone check out of band that this is the certificate they were given.
struct CertificateSummary {
    QString commonName;
    QString issuer;
    QDateTime notBefore;
    QDateTime notAfter;
    /// Colon-separated uppercase SHA-256, the form every other tool prints.
    QString fingerprint;

    static CertificateSummary of(const QSslCertificate &certificate);

    bool isExpired() const;
    bool isNotYetValid() const;
    QString validityDescription() const;

    bool operator==(const CertificateSummary &other) const;
};

/// Everything that can go wrong importing certificate material, in the user's words.
class CertificateImportError {
public:
    enum class Kind {
        None,
        UnreadableFile,
        NotAPkcs12File,
        WrongPassphrase,
        NoIdentityInFile,
        NotACertificateFile,
        /// A chain file rather than a single certificate. Anchoring element 0 would be
        /// a quiet mistake: server chains run leaf to root, so "the first" is usually
        /// an intermediate — a weaker anchor than the user believes they installed.
        MultipleCertificates,
        StorageFailed,
    };

    CertificateImportError() = default;
    static CertificateImportError unreadableFile(const QString &name, const QString &reason);
    static CertificateImportError notAPkcs12File();
    static CertificateImportError wrongPassphrase();
    static CertificateImportError noIdentityInFile();
    static CertificateImportError notACertificateFile();
    static CertificateImportError multipleCertificates(int count);
    static CertificateImportError storageFailed(const QString &reason);

    bool isNull() const { return m_kind == Kind::None; }
    Kind kind() const { return m_kind; }
    QString message() const;

private:
    Kind m_kind = Kind::None;
    QString m_detail;
    int m_count = 0;
};

} // namespace immichksync
