#include "credentials/SecretStore.h"

#include "core/Logging.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QEventLoop>
#include <QHash>
#include <QMutexLocker>
#include <QTimer>
#include <QVariantMap>

namespace immichksync {

/// A secret as `org.freedesktop.Secret.Service` defines it: `(o, ay, ay, s)` — the
/// session, the algorithm parameters, the value, and its content type.
///
/// At namespace scope rather than file scope because Q_DECLARE_METATYPE needs a name
/// it can qualify.
struct SecretStruct {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument &operator<<(QDBusArgument &argument, const SecretStruct &secret)
{
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value << secret.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SecretStruct &secret)
{
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    argument.endStructure();
    return argument;
}

/// Receives `org.freedesktop.Secret.Prompt.Completed`.
///
/// A real QObject with a real slot, because a D-Bus signal connected by name cannot
/// take a lambda, and this signal carries the answer we need.
class PromptWatcher : public QObject {
    Q_OBJECT

public:
    bool completed = false;
    bool dismissed = true;

Q_SIGNALS:
    void finished();

public Q_SLOTS:
    void onCompleted(bool wasDismissed, const QDBusVariant &result)
    {
        Q_UNUSED(result)
        completed = true;
        dismissed = wasDismissed;
        Q_EMIT finished();
    }
};

} // namespace immichksync

Q_DECLARE_METATYPE(immichksync::SecretStruct)

namespace immichksync {

namespace {

constexpr const char *kServiceName = "org.freedesktop.secrets";
constexpr const char *kServicePath = "/org/freedesktop/secrets";
constexpr const char *kServiceInterface = "org.freedesktop.Secret.Service";
constexpr const char *kCollectionInterface = "org.freedesktop.Secret.Collection";
constexpr const char *kItemInterface = "org.freedesktop.Secret.Item";
constexpr const char *kPromptInterface = "org.freedesktop.Secret.Prompt";
constexpr const char *kDefaultCollection = "/org/freedesktop/secrets/aliases/default";

/// Registers every custom type this file puts on the wire.
///
/// Marshalling an unregistered type does not fail gracefully: Qt writes nothing into
/// the open container and libdbus then aborts the whole process inside
/// `dbus_message_iter_close_container`. The attribute map (`a{ss}`) is the one that
/// bites — Qt registers QVariantMap and QList<QDBusObjectPath> itself, but not this.
///
/// A magic static rather than a checked flag, because the TLS store reads secrets from
/// a transport worker thread while the settings window writes them from the GUI thread.
void registerTypes()
{
    static const bool registered = []() {
        qDBusRegisterMetaType<SecretStruct>();
        qDBusRegisterMetaType<QMap<QString, QString>>();
        return true;
    }();
    Q_UNUSED(registered)
}

/// One open session with the secret service, plus the helpers that need it.
///
/// The "plain" algorithm is used deliberately: the transport is a Unix domain socket
/// with peer credentials, and DH session encryption would protect against an attacker
/// who can already read the user's own bus — at which point they can read the process's
/// memory too. gnome-keyring's own tooling makes the same choice.
class Session {
public:
    Session()
    {
        registerTypes();
        if (!QDBusConnection::sessionBus().isConnected()) {
            m_error = QStringLiteral("No session D-Bus is available.");
            return;
        }

        QDBusInterface service(QLatin1String(kServiceName),
                               QLatin1String(kServicePath),
                               QLatin1String(kServiceInterface),
                               QDBusConnection::sessionBus());
        if (!service.isValid()) {
            m_error = QStringLiteral(
                "No secret service is running. On Plasma this is ksecretd or kwalletd6; "
                "install or start one so credentials can be stored.");
            return;
        }

        const QDBusMessage reply =
            service.call(QStringLiteral("OpenSession"), QStringLiteral("plain"),
                         QVariant::fromValue(QDBusVariant(QString())));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
            m_error = QStringLiteral("Could not open a secret service session: %1")
                          .arg(reply.errorMessage());
            return;
        }
        m_session = qvariant_cast<QDBusObjectPath>(reply.arguments().at(1));
        m_valid = true;
    }

    bool isValid() const { return m_valid; }
    QString error() const { return m_error; }
    QDBusObjectPath path() const { return m_session; }

    /// Makes sure the collection we are about to write into is unlocked.
    ///
    /// `CreateItem` on a locked collection fails outright, and the service does not
    /// unlock it on our behalf — so writing a credential to a locked wallet did nothing
    /// but return an opaque error until this was here.
    bool ensureCollectionUnlocked(QString *errorMessage)
    {
        const QDBusObjectPath collection{QLatin1String(kDefaultCollection)};

        // Cheap check first: an unlocked collection needs no prompt round trip.
        QDBusInterface properties(QLatin1String(kServiceName),
                                  collection.path(),
                                  QStringLiteral("org.freedesktop.DBus.Properties"),
                                  QDBusConnection::sessionBus());
        const QDBusMessage locked = properties.call(QStringLiteral("Get"),
                                                    QLatin1String(kCollectionInterface),
                                                    QStringLiteral("Locked"));
        if (locked.type() == QDBusMessage::ReplyMessage && !locked.arguments().isEmpty()) {
            const QVariant value = locked.arguments().at(0).value<QDBusVariant>().variant();
            if (!value.toBool()) {
                return true;
            }
        }

        log::credentials.info(
            QStringLiteral("The keyring is locked; asking for it to be unlocked."));
        const QList<QDBusObjectPath> unlocked = unlock({collection}, errorMessage);
        return unlocked.contains(collection);
    }

    /// Finds the item paths matching `attributes`, unlocking the collection if needed.
    QList<QDBusObjectPath> search(const QVariantMap &attributes)
    {
        QDBusInterface service(QLatin1String(kServiceName),
                               QLatin1String(kServicePath),
                               QLatin1String(kServiceInterface),
                               QDBusConnection::sessionBus());

        QMap<QString, QString> lookup;
        for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
            lookup.insert(it.key(), it.value().toString());
        }

        const QDBusMessage reply = service.call(QStringLiteral("SearchItems"),
                                                QVariant::fromValue(lookup));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            return {};
        }

        QList<QDBusObjectPath> unlocked =
            qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
        QList<QDBusObjectPath> locked;
        if (reply.arguments().size() > 1) {
            locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
        }

        if (!locked.isEmpty()) {
            // A locked keyring is a normal state after login on many setups. The caller
            // treats a still-locked item as "not stored yet" rather than failing.
            unlocked += unlock(locked, nullptr);
        }
        return unlocked;
    }

    /// Unlocks `objects`, driving the unlock prompt if the service raises one.
    ///
    /// `Unlock` returning is not the same as the objects being unlocked: when the
    /// service needs the user's password it answers with a prompt path, and nothing
    /// happens until someone calls `Prompt` on it. Ignoring that is why a locked wallet
    /// silently refused every read and write.
    QList<QDBusObjectPath> unlock(const QList<QDBusObjectPath> &objects, QString *errorMessage)
    {
        QDBusInterface service(QLatin1String(kServiceName),
                               QLatin1String(kServicePath),
                               QLatin1String(kServiceInterface),
                               QDBusConnection::sessionBus());

        const QDBusMessage reply =
            service.call(QStringLiteral("Unlock"), QVariant::fromValue(objects));
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not unlock the keyring: %1")
                                    .arg(reply.errorMessage());
            }
            return {};
        }

        QList<QDBusObjectPath> unlocked =
            qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));

        const QDBusObjectPath prompt = reply.arguments().size() > 1
            ? qvariant_cast<QDBusObjectPath>(reply.arguments().at(1))
            : QDBusObjectPath();
        // "/" is the service's way of saying no prompt was needed.
        if (prompt.path().isEmpty() || prompt.path() == QLatin1String("/")) {
            return unlocked;
        }
        if (!runPrompt(prompt, errorMessage)) {
            return unlocked;
        }
        return objects;
    }

private:
    /// Shows the service's unlock prompt and waits for the user to answer it.
    ///
    /// Bounded: an unattended sync cycle must not sit on a dialog for ever if the user
    /// has walked away. Failing after the timeout leaves the app reporting that it is
    /// waiting for credentials, which is recoverable; blocking the sync thread
    /// permanently is not.
    bool runPrompt(const QDBusObjectPath &prompt, QString *errorMessage)
    {
        static constexpr int kPromptTimeoutMs = 120000;

        QEventLoop loop;
        PromptWatcher watcher;
        QObject::connect(&watcher, &PromptWatcher::finished, &loop, &QEventLoop::quit);

        if (!QDBusConnection::sessionBus().connect(QLatin1String(kServiceName),
                                                   prompt.path(),
                                                   QLatin1String(kPromptInterface),
                                                   QStringLiteral("Completed"),
                                                   &watcher,
                                                   SLOT(onCompleted(bool, QDBusVariant)))) {
            if (errorMessage) {
                *errorMessage =
                    QStringLiteral("Could not listen for the keyring unlock prompt result.");
            }
            return false;
        }

        QDBusInterface promptInterface(QLatin1String(kServiceName),
                                       prompt.path(),
                                       QLatin1String(kPromptInterface),
                                       QDBusConnection::sessionBus());
        // An empty window id: there is no window to parent the dialog to when the sync
        // thread is the one asking.
        const QDBusMessage shown = promptInterface.call(QStringLiteral("Prompt"), QString());
        if (shown.type() != QDBusMessage::ReplyMessage) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not show the keyring unlock prompt: %1")
                                    .arg(shown.errorMessage());
            }
            return false;
        }

        QTimer::singleShot(kPromptTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);

        if (!watcher.completed) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("The keyring unlock prompt was not answered.");
            }
            return false;
        }
        if (watcher.dismissed) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("The keyring unlock prompt was dismissed.");
            }
            return false;
        }
        return true;
    }

    bool m_valid = false;
    QString m_error;
    QDBusObjectPath m_session;
};

QVariantMap attributesFor(const QString &service, const QString &account)
{
    return {{QStringLiteral("service"), service}, {QStringLiteral("account"), account}};
}

} // namespace

SecretStore::SecretStore(QString service)
    : m_service(std::move(service))
{
    // Here rather than only in Session, so no code path can reach a D-Bus call with
    // the types still unregistered.
    registerTypes();
}

SecretStore::SecretStore(InMemoryTag, QString service)
    : m_service(std::move(service))
    , m_inMemory(true)
{
    registerTypes();
}

bool SecretStore::dbusTypesAreRegistered()
{
    registerTypes();
    return QDBusMetaType::typeToSignature(QMetaType::fromType<QMap<QString, QString>>())
        && QDBusMetaType::typeToSignature(QMetaType::fromType<SecretStruct>());
}

bool SecretStore::isAvailable() const
{
    return readiness() != Readiness::Unavailable;
}

SecretStore::Readiness SecretStore::readiness() const
{
    if (m_inMemory) {
        return Readiness::Ready;
    }
    if (!Session().isValid()) {
        return Readiness::Unavailable;
    }

    // Read the property rather than calling Unlock: asking whether the wallet is
    // locked must not itself pop a password dialog.
    QDBusInterface properties(QLatin1String(kServiceName),
                              QLatin1String(kDefaultCollection),
                              QStringLiteral("org.freedesktop.DBus.Properties"),
                              QDBusConnection::sessionBus());
    const QDBusMessage locked = properties.call(QStringLiteral("Get"),
                                                QLatin1String(kCollectionInterface),
                                                QStringLiteral("Locked"));
    if (locked.type() != QDBusMessage::ReplyMessage || locked.arguments().isEmpty()) {
        // The service answered the session call but not this one; treat it as usable
        // and let the actual read or write report a real error.
        return Readiness::Ready;
    }
    const QVariant value = locked.arguments().at(0).value<QDBusVariant>().variant();
    return value.toBool() ? Readiness::Locked : Readiness::Ready;
}

QString SecretStore::unavailableReason() const
{
    switch (readiness()) {
    case Readiness::Ready:
        return {};
    case Readiness::Locked:
        return QStringLiteral("Your keyring is locked. You will be asked to unlock it the "
                              "first time a credential is stored or read.");
    case Readiness::Unavailable:
        break;
    }
    return Session().error();
}

// MARK: - Account naming

QString SecretStore::accountFor(const QString &server, Slot slot)
{
    const QString name = slot == Slot::ApiKey ? QStringLiteral("apiKey")
                                              : QStringLiteral("sessionToken");
    return QStringLiteral("%1@%2").arg(name, server);
}

QString SecretStore::accountFor(GlobalSlot slot)
{
    // Global accounts carry a prefix instead of the `@server` suffix, so the two
    // namespaces cannot collide however odd a server address becomes.
    switch (slot) {
    case GlobalSlot::ClientCertificate:
        return QStringLiteral("global:clientCertificate");
    case GlobalSlot::ClientCertificatePassphrase:
        return QStringLiteral("global:clientCertificatePassphrase");
    case GlobalSlot::CertificateAuthority:
        return QStringLiteral("global:certificateAuthority");
    }
    return {};
}

// MARK: - Raw access

std::optional<QByteArray> SecretStore::readRaw(const QString &account) const
{
    if (m_inMemory) {
        QMutexLocker locker(&m_mutex);
        const auto it = m_memory.constFind(account);
        return it == m_memory.cend() ? std::nullopt : std::optional<QByteArray>(*it);
    }

    Session session;
    if (!session.isValid()) {
        log::credentials.warning(session.error());
        return std::nullopt;
    }

    const QList<QDBusObjectPath> items = session.search(attributesFor(m_service, account));
    if (items.isEmpty()) {
        return std::nullopt;
    }

    QDBusInterface item(QLatin1String(kServiceName),
                        items.first().path(),
                        QLatin1String(kItemInterface),
                        QDBusConnection::sessionBus());
    const QDBusMessage reply =
        item.call(QStringLiteral("GetSecret"), QVariant::fromValue(session.path()));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        log::credentials.error(
            QStringLiteral("Could not read the stored secret: %1").arg(reply.errorMessage()));
        return std::nullopt;
    }

    SecretStruct secret;
    const auto argument = reply.arguments().at(0).value<QDBusArgument>();
    argument >> secret;
    return secret.value;
}

bool SecretStore::writeRaw(const QByteArray &data, const QString &account, QString *errorMessage)
{
    if (m_inMemory) {
        QMutexLocker locker(&m_mutex);
        m_memory.insert(account, data);
        return true;
    }

    Session session;
    if (!session.isValid()) {
        if (errorMessage) {
            *errorMessage = session.error();
        }
        return false;
    }
    if (!session.ensureCollectionUnlocked(errorMessage)) {
        return false;
    }

    QMap<QString, QString> attributes;
    attributes.insert(QStringLiteral("service"), m_service);
    attributes.insert(QStringLiteral("account"), account);

    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                      QStringLiteral("ImmichKSync: %1").arg(account));
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                      QVariant::fromValue(attributes));

    SecretStruct secret;
    secret.session = session.path();
    secret.value = data;
    secret.contentType = QStringLiteral("application/octet-stream");

    QDBusInterface collection(QLatin1String(kServiceName),
                              QLatin1String(kDefaultCollection),
                              QLatin1String(kCollectionInterface),
                              QDBusConnection::sessionBus());
    // `replace = true`: writing a credential always supersedes the previous one, and a
    // duplicate item would make reads non-deterministic.
    const QDBusMessage reply = collection.call(QStringLiteral("CreateItem"),
                                               properties,
                                               QVariant::fromValue(secret),
                                               true);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not store the secret: %1").arg(reply.errorMessage());
        }
        return false;
    }
    return true;
}

bool SecretStore::removeRaw(const QString &account)
{
    if (m_inMemory) {
        QMutexLocker locker(&m_mutex);
        m_memory.remove(account);
        return true;
    }

    Session session;
    if (!session.isValid()) {
        return false;
    }
    const QList<QDBusObjectPath> items = session.search(attributesFor(m_service, account));
    bool removedAny = false;
    for (const QDBusObjectPath &path : items) {
        QDBusInterface item(QLatin1String(kServiceName),
                            path.path(),
                            QLatin1String(kItemInterface),
                            QDBusConnection::sessionBus());
        const QDBusMessage reply = item.call(QStringLiteral("Delete"));
        removedAny = removedAny || reply.type() == QDBusMessage::ReplyMessage;
    }
    return removedAny;
}

// MARK: - Per-server secrets

std::optional<QString> SecretStore::read(const QString &server, Slot slot) const
{
    const auto data = readRaw(accountFor(server, slot));
    if (!data) {
        return std::nullopt;
    }
    return QString::fromUtf8(*data);
}

bool SecretStore::write(const QString &secret,
                        const QString &server,
                        Slot slot,
                        QString *errorMessage)
{
    return writeRaw(secret.toUtf8(), accountFor(server, slot), errorMessage);
}

bool SecretStore::remove(const QString &server, Slot slot)
{
    return removeRaw(accountFor(server, slot));
}

void SecretStore::removeAll(const QString &server)
{
    // Iterating `Slot` is what keeps Sign Out away from the TLS material: the global
    // items live in a different enum and cannot be reached from here.
    for (const Slot slot : {Slot::ApiKey, Slot::SessionToken}) {
        remove(server, slot);
    }
}

std::optional<ImmichCredentials> SecretStore::credentials(const QString &server,
                                                          ImmichAuthMode mode) const
{
    const Slot slot = mode == ImmichAuthMode::ApiKey ? Slot::ApiKey : Slot::SessionToken;
    const auto secret = read(server, slot);
    if (!secret || secret->isEmpty()) {
        return std::nullopt;
    }
    return mode == ImmichAuthMode::ApiKey ? ImmichCredentials::apiKey(*secret)
                                          : ImmichCredentials::sessionToken(*secret);
}

// MARK: - App-wide secrets

std::optional<QByteArray> SecretStore::readData(GlobalSlot slot) const
{
    return readRaw(accountFor(slot));
}

std::optional<QString> SecretStore::readString(GlobalSlot slot) const
{
    const auto data = readData(slot);
    return data ? std::optional<QString>(QString::fromUtf8(*data)) : std::nullopt;
}

bool SecretStore::writeData(const QByteArray &data, GlobalSlot slot, QString *errorMessage)
{
    return writeRaw(data, accountFor(slot), errorMessage);
}

bool SecretStore::write(const QString &secret, GlobalSlot slot, QString *errorMessage)
{
    return writeRaw(secret.toUtf8(), accountFor(slot), errorMessage);
}

bool SecretStore::remove(GlobalSlot slot)
{
    return removeRaw(accountFor(slot));
}

} // namespace immichksync


#include "SecretStore.moc"
