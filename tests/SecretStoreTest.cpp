#include "credentials/SecretStore.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QTest>

using namespace immichksync;

/// Credential scoping. The rule that matters most is negative: Sign Out must not be
/// able to reach the TLS material, because without it the server may not be reachable
/// at all — and the user would be locked out of the very screen they need.
class SecretStoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void storesAndReadsAnApiKey()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("secret-key"),
                            QStringLiteral("https://immich.example.com"),
                            SecretStore::Slot::ApiKey,
                            nullptr));

        QCOMPARE(*store.read(QStringLiteral("https://immich.example.com"),
                             SecretStore::Slot::ApiKey),
                 QStringLiteral("secret-key"));
    }

    void reportsNothingWhenNothingIsStored()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(!store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey).has_value());
    }

    /// Changing servers must not silently reuse the previous account's token.
    void scopesSecretsPerServer()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key-a"),
                            QStringLiteral("server-a"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.write(QStringLiteral("key-b"),
                            QStringLiteral("server-b"),
                            SecretStore::Slot::ApiKey,
                            nullptr));

        QCOMPARE(*store.read(QStringLiteral("server-a"), SecretStore::Slot::ApiKey),
                 QStringLiteral("key-a"));
        QCOMPARE(*store.read(QStringLiteral("server-b"), SecretStore::Slot::ApiKey),
                 QStringLiteral("key-b"));
    }

    void keepsTheTwoSlotsApart()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.write(QStringLiteral("token"),
                            QStringLiteral("server"),
                            SecretStore::Slot::SessionToken,
                            nullptr));

        QCOMPARE(*store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey),
                 QStringLiteral("key"));
        QCOMPARE(*store.read(QStringLiteral("server"), SecretStore::Slot::SessionToken),
                 QStringLiteral("token"));
    }

    void resolvesCredentialsForTheSelectedMode()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.write(QStringLiteral("token"),
                            QStringLiteral("server"),
                            SecretStore::Slot::SessionToken,
                            nullptr));

        const auto apiKey = store.credentials(QStringLiteral("server"), ImmichAuthMode::ApiKey);
        QVERIFY(apiKey.has_value());
        QCOMPARE(apiKey->headerField(), QStringLiteral("x-api-key"));
        QCOMPARE(apiKey->headerValue(), QStringLiteral("key"));

        const auto password = store.credentials(QStringLiteral("server"), ImmichAuthMode::Password);
        QVERIFY(password.has_value());
        QCOMPARE(password->headerField(), QStringLiteral("Authorization"));
        QCOMPARE(password->headerValue(), QStringLiteral("Bearer token"));
    }

    void treatsAnEmptySecretAsAbsent()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QString(), QStringLiteral("server"), SecretStore::Slot::ApiKey, nullptr));
        QVERIFY(!store.credentials(QStringLiteral("server"), ImmichAuthMode::ApiKey).has_value());
    }

    void removesASingleSlot()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.remove(QStringLiteral("server"), SecretStore::Slot::ApiKey));
        QVERIFY(!store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey).has_value());
    }

    void signOutClearsBothPerServerSlots()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.write(QStringLiteral("token"),
                            QStringLiteral("server"),
                            SecretStore::Slot::SessionToken,
                            nullptr));

        store.removeAll(QStringLiteral("server"));

        QVERIFY(!store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey).has_value());
        QVERIFY(!store.read(QStringLiteral("server"), SecretStore::Slot::SessionToken).has_value());
    }

    /// The whole point of the separate GlobalSlot enum: without the client certificate
    /// the server may be unreachable, so signing out must not remove it.
    void signOutLeavesTheTlsMaterialAlone()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("key"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.writeData(QByteArray("p12 bytes"),
                                SecretStore::GlobalSlot::ClientCertificate,
                                nullptr));
        QVERIFY(store.write(QStringLiteral("phrase"),
                            SecretStore::GlobalSlot::ClientCertificatePassphrase,
                            nullptr));
        QVERIFY(store.writeData(QByteArray("der bytes"),
                                SecretStore::GlobalSlot::CertificateAuthority,
                                nullptr));

        store.removeAll(QStringLiteral("server"));

        QVERIFY(!store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey).has_value());
        QCOMPARE(*store.readData(SecretStore::GlobalSlot::ClientCertificate),
                 QByteArray("p12 bytes"));
        QCOMPARE(*store.readString(SecretStore::GlobalSlot::ClientCertificatePassphrase),
                 QStringLiteral("phrase"));
        QCOMPARE(*store.readData(SecretStore::GlobalSlot::CertificateAuthority),
                 QByteArray("der bytes"));
    }

    void roundTripsBinaryGlobalSecrets()
    {
        SecretStore store(SecretStore::InMemory);
        // A PKCS#12 blob is binary and contains NUL bytes; a String round trip would
        // silently truncate it.
        const QByteArray binary = QByteArray::fromHex("3082deadbeef0000cafe");
        QVERIFY(store.writeData(binary, SecretStore::GlobalSlot::ClientCertificate, nullptr));
        QCOMPARE(*store.readData(SecretStore::GlobalSlot::ClientCertificate), binary);
    }

    void removesAGlobalSlot()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.writeData(QByteArray("x"),
                                SecretStore::GlobalSlot::CertificateAuthority,
                                nullptr));
        QVERIFY(store.remove(SecretStore::GlobalSlot::CertificateAuthority));
        QVERIFY(!store.readData(SecretStore::GlobalSlot::CertificateAuthority).has_value());
    }

    /// A server address is user input and can contain anything; the two namespaces
    /// still must not collide.
    void perServerAndGlobalNamespacesCannotCollide()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("per-server"),
                            QStringLiteral("global:clientCertificate"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.writeData(QByteArray("global"),
                                SecretStore::GlobalSlot::ClientCertificate,
                                nullptr));

        QCOMPARE(*store.readData(SecretStore::GlobalSlot::ClientCertificate), QByteArray("global"));
        QCOMPARE(*store.read(QStringLiteral("global:clientCertificate"),
                             SecretStore::Slot::ApiKey),
                 QStringLiteral("per-server"));
    }

    void overwritesRatherThanDuplicating()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.write(QStringLiteral("first"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QVERIFY(store.write(QStringLiteral("second"),
                            QStringLiteral("server"),
                            SecretStore::Slot::ApiKey,
                            nullptr));
        QCOMPARE(*store.read(QStringLiteral("server"), SecretStore::Slot::ApiKey),
                 QStringLiteral("second"));
    }

    void theInMemoryStoreIsAlwaysAvailable()
    {
        SecretStore store(SecretStore::InMemory);
        QVERIFY(store.isAvailable());
        QVERIFY(store.unavailableReason().isEmpty());
    }

    // MARK: - D-Bus marshalling
    //
    // These exist because the tests above all use the in-memory double, which meant the
    // entire Secret Service path shipped without ever being executed. Marshalling an
    // unregistered type does not return an error — Qt writes nothing into the open
    // container and libdbus aborts the process — so the registration is asserted
    // directly rather than discovered from a core dump.

    void everyTypeItMarshalsHasADBusSignature()
    {
        QVERIFY2(SecretStore::dbusTypesAreRegistered(),
                 "a type this class puts on the wire has no D-Bus signature; marshalling it "
                 "would abort the process inside libdbus");
    }

    void theAttributeMapMarshalsAsAss()
    {
        SecretStore store(SecretStore::InMemory);
        // The item attributes are a{ss}. This is the exact type whose absence aborted
        // the app when a certificate authority was imported.
        const char *signature =
            QDBusMetaType::typeToSignature(QMetaType::fromType<QMap<QString, QString>>());
        QVERIFY(signature != nullptr);
        QCOMPARE(QByteArray(signature), QByteArray("a{ss}"));
    }

    /// The properties dictionary handed to CreateItem is a{sv} whose values include the
    /// a{ss} attribute map. Marshalling it for real is the only way to know the nesting
    /// is right; org.freedesktop.DBus always exists, so this needs no keyring.
    void marshalsTheCreateItemPropertiesWithoutAborting()
    {
        if (!QDBusConnection::sessionBus().isConnected()) {
            QSKIP("no session bus available");
        }
        SecretStore store(SecretStore::InMemory);

        QMap<QString, QString> attributes;
        attributes.insert(QStringLiteral("service"), QStringLiteral("immichksync"));
        attributes.insert(QStringLiteral("account"), QStringLiteral("apiKey@example.com"));

        QVariantMap properties;
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                          QStringLiteral("ImmichKSync: apiKey@example.com"));
        properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                          QVariant::fromValue(attributes));

        QDBusMessage message =
            QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                           QStringLiteral("/"),
                                           QStringLiteral("org.freedesktop.DBus"),
                                           QStringLiteral("ImmichKSyncMarshallingProbe"));
        message.setArguments({QVariant(properties)});

        // Reaching the next line at all is the assertion: an unregistered type aborts
        // here rather than returning. UnknownMethod is the expected, healthy answer.
        const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
        QCOMPARE(reply.errorName(), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"));
    }

    /// The search and unlock calls pass QList<QDBusObjectPath> ("ao"). Qt registers it
    /// itself, but that is an assumption worth pinning: if it ever stops being true,
    /// reading a secret would abort exactly as writing one did.
    void objectPathListsMarshalWithoutAborting()
    {
        if (!QDBusConnection::sessionBus().isConnected()) {
            QSKIP("no session bus available");
        }
        const QList<QDBusObjectPath> paths{QDBusObjectPath(QStringLiteral("/item/1")),
                                           QDBusObjectPath(QStringLiteral("/item/2"))};

        QDBusMessage message =
            QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                           QStringLiteral("/"),
                                           QStringLiteral("org.freedesktop.DBus"),
                                           QStringLiteral("ImmichKSyncMarshallingProbe"));
        message.setArguments({QVariant::fromValue(paths)});

        const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
        QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    }

    /// A full round trip through whatever keyring is actually running. Skipped rather
    /// than failed when there is none, because a headless CI container has no keyring —
    /// but on any real desktop this is the test that exercises the shipping path.
    void roundTripsThroughTheRealSecretService()
    {
        SecretStore store(QStringLiteral("immichksync-test-suite"));
        // Skipped rather than run on a locked wallet: unlocking needs a password typed
        // into a dialog, and a test that waits for one is a test that hangs.
        switch (store.readiness()) {
        case SecretStore::Readiness::Unavailable:
            QSKIP("no secret service is running");
        case SecretStore::Readiness::Locked:
            QSKIP("the keyring is locked; unlock it to run this test");
        case SecretStore::Readiness::Ready:
            break;
        }

        const QString server = QStringLiteral("https://test.invalid");
        store.removeAll(server);

        QString error;
        QVERIFY2(store.write(QStringLiteral("round-trip-key"),
                             server,
                             SecretStore::Slot::ApiKey,
                             &error),
                 qUtf8Printable(error));

        const auto read = store.read(server, SecretStore::Slot::ApiKey);
        QVERIFY(read.has_value());
        QCOMPARE(*read, QStringLiteral("round-trip-key"));

        // Binary, because a PKCS#12 blob contains NUL bytes and this is the path a
        // client certificate takes.
        const QByteArray binary = QByteArray::fromHex("3082deadbeef0000cafe");
        QVERIFY2(store.writeData(binary, SecretStore::GlobalSlot::ClientCertificate, &error),
                 qUtf8Printable(error));
        QCOMPARE(*store.readData(SecretStore::GlobalSlot::ClientCertificate), binary);

        store.removeAll(server);
        store.remove(SecretStore::GlobalSlot::ClientCertificate);
        QVERIFY(!store.read(server, SecretStore::Slot::ApiKey).has_value());
        QVERIFY(!store.readData(SecretStore::GlobalSlot::ClientCertificate).has_value());
    }
};

QTEST_APPLESS_MAIN(SecretStoreTest)
#include "SecretStoreTest.moc"
