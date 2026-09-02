#pragma once

#include "core/Logging.h"
#include "credentials/ImmichCredentials.h"

#include <QByteArray>
#include <QDBusObjectPath>
#include <QHash>
#include <QMutex>
#include <QString>

#include <optional>

namespace immichksync {

/// Secret storage backed by the Secret Service D-Bus API
/// (`org.freedesktop.secrets`), keyed by server address so switching servers does not
/// silently reuse the previous account's token.
///
/// On Plasma 6 this is served by `ksecretd`; on other desktops by `kwalletd6`'s compat
/// layer or `gnome-keyring`. Talking the freedesktop protocol directly rather than
/// linking KWallet keeps the app working on all of them with no extra dependency.
///
/// Secrets never touch the config file or the local database.
class SecretStore {
public:
    /// Per-server secrets.
    enum class Slot {
        ApiKey,
        SessionToken,
    };

    /// Items that belong to the app rather than to one server.
    ///
    /// A TLS client certificate can be what makes the server reachable at all, so
    /// clearing it on Sign Out would lock the user out of the very screen they need in
    /// order to sign back in. These are removed only on explicit request, and
    /// `deleteAll(server)` cannot reach them: it iterates `Slot`, a different type.
    enum class GlobalSlot {
        /// The PKCS#12 blob exactly as the user chose it, re-imported on demand.
        ClientCertificate,
        ClientCertificatePassphrase,
        /// DER bytes of a single X.509 certificate authority.
        CertificateAuthority,
    };

    /// Tag selecting the in-memory implementation, so the test suite never touches a
    /// real keyring. A `static inMemory()` factory cannot work here: the mutex makes
    /// the class non-movable, so it has to be chosen at construction.
    struct InMemoryTag {};
    static constexpr InMemoryTag InMemory{};

    explicit SecretStore(QString service = QString::fromLatin1(kAppId));
    explicit SecretStore(InMemoryTag, QString service = QStringLiteral("immichksync-test"));

    /// Whether the keyring can be used right now, and if not, why.
    enum class Readiness {
        /// No secret service is running at all.
        Unavailable,
        /// A service is running but its collection is locked. Reading or writing will
        /// raise an unlock prompt.
        Locked,
        Ready,
    };

    /// Asks the service without raising a prompt, so a caller can report the state
    /// rather than trigger a password dialog just to find out.
    Readiness readiness() const;

    bool isAvailable() const;

    /// True once every type this class marshals has a D-Bus signature.
    ///
    /// Exposed because the failure mode is not an error return: marshalling an
    /// unregistered type aborts the process inside libdbus, so this is asserted
    /// directly rather than waiting for a crash to reveal it.
    static bool dbusTypesAreRegistered();
    /// Names why the keyring cannot be used, for the settings window.
    QString unavailableReason() const;

    // Per-server secrets
    std::optional<QString> read(const QString &server, Slot slot) const;
    bool write(const QString &secret, const QString &server, Slot slot, QString *errorMessage);
    bool remove(const QString &server, Slot slot);
    void removeAll(const QString &server);

    /// Resolves the credential to use for `server` in `mode`, if one is stored.
    std::optional<ImmichCredentials> credentials(const QString &server, ImmichAuthMode mode) const;

    // App-wide secrets
    std::optional<QByteArray> readData(GlobalSlot slot) const;
    std::optional<QString> readString(GlobalSlot slot) const;
    bool writeData(const QByteArray &data, GlobalSlot slot, QString *errorMessage);
    bool write(const QString &secret, GlobalSlot slot, QString *errorMessage);
    bool remove(GlobalSlot slot);

private:
    std::optional<QByteArray> readRaw(const QString &account) const;
    bool writeRaw(const QByteArray &data, const QString &account, QString *errorMessage);
    bool removeRaw(const QString &account);

    static QString accountFor(const QString &server, Slot slot);
    static QString accountFor(GlobalSlot slot);

    QString m_service;
    /// Set for the test double, which keeps everything in memory and never opens a bus.
    bool m_inMemory = false;
    mutable QMutex m_mutex;
    mutable QHash<QString, QByteArray> m_memory;
};

} // namespace immichksync
