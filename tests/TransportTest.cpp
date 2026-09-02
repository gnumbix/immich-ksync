#include "core/TaskPool.h"
#include "immich/Transport.h"

#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>

using namespace immichksync;

namespace {

/// The smallest HTTP server that will satisfy the transport, running in-process so the
/// suite needs no network and no fixtures.
class TinyHttpServer : public QTcpServer {
    Q_OBJECT

public:
    explicit TinyHttpServer(QObject *parent = nullptr) : QTcpServer(parent) {}

    quint16 port() const { return m_port.loadAcquire(); }

public Q_SLOTS:
    /// Runs on the server's own thread, so the listening socket belongs to it.
    void start()
    {
        if (listen(QHostAddress::LocalHost)) {
            m_port.storeRelease(serverPort());
        }
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            const QByteArray request = socket->readAll();
            if (!request.contains("\r\n\r\n")) {
                return;
            }
            const QByteArray body = QByteArrayLiteral(R"({"res":"pong"})");
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->flush();
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    QAtomicInt m_port = 0;
};

} // namespace

/// The transport is used from transfer pools that are created and destroyed once per
/// cycle. That lifecycle is the whole point of these tests: a manager cached against a
/// thread that has since exited never delivers `finished()`, so the request waits for
/// ever — no error, no timeout, and the sync thread pinned along with it.
class TransportTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // The server needs its own thread with its own event loop. TaskPool::map blocks
        // the calling thread without running events, so a server living here would stop
        // answering for exactly the duration of the test that needs it most.
        m_serverThread = new QThread(this);
        m_serverThread->setObjectName(QStringLiteral("test-http-server"));
        m_server = new TinyHttpServer();
        m_server->moveToThread(m_serverThread);

        connect(m_serverThread, &QThread::started, m_server, &TinyHttpServer::start);
        connect(m_serverThread, &QThread::finished, m_server, &QObject::deleteLater);
        m_serverThread->start();

        QVERIFY(QTest::qWaitFor([this]() { return m_server->port() != 0; }, 5000));
        m_port = m_server->port();
    }

    void cleanupTestCase()
    {
        m_serverThread->quit();
        QVERIFY(m_serverThread->wait(5000));
    }

    void performsASimpleRequest()
    {
        NetworkTransport transport;
        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 10;

        const HttpResponse response = transport.send(request);
        QVERIFY2(response.error.isNull(), qUtf8Printable(response.error.message()));
        QCOMPARE(response.statusCode, 200);
        QVERIFY(response.body.contains("pong"));
    }

    /// The regression test. Each round mimics one cycle's transfer pool: a fresh
    /// QThreadPool whose threads exit when it goes out of scope, releasing their
    /// operating-system thread ids for the next round to reuse.
    void survivesSuccessiveTransferPools()
    {
        NetworkTransport transport;

        for (int round = 0; round < 6; ++round) {
            QList<int> items;
            for (int i = 0; i < 4; ++i) {
                items.append(i);
            }

            QElapsedTimer timer;
            timer.start();
            const QList<bool> results = TaskPool::map<int, bool>(items, 4, [&](const int &) {
                HttpRequest request;
                request.url = serverUrl();
                request.transferTimeoutSeconds = 5;
                return transport.send(request).statusCode == 200;
            });

            for (int i = 0; i < results.size(); ++i) {
                QVERIFY2(results.at(i),
                         qUtf8Printable(QStringLiteral("round %1, item %2 did not complete")
                                            .arg(round)
                                            .arg(i)));
            }
            // Each request is a loopback round trip. Anything near the timeout means a
            // request waited on a manager that could never answer it.
            QVERIFY2(timer.elapsed() < 4000,
                     qUtf8Printable(QStringLiteral("round %1 took %2ms")
                                        .arg(round)
                                        .arg(timer.elapsed())));
        }
    }

    /// A manager must belong to the thread using it. This is the invariant whose
    /// absence caused the hang, asserted directly rather than inferred from a timeout.
    void neverHandsAManagerToTheWrongThread()
    {
        NetworkTransport transport;

        for (int round = 0; round < 4; ++round) {
            QList<int> items{0, 1, 2, 3};
            const QList<bool> sameThread = TaskPool::map<int, bool>(items, 4, [&](const int &) {
                HttpRequest request;
                request.url = serverUrl();
                request.transferTimeoutSeconds = 5;
                // A reply is parented to the manager's thread, so a completed request
                // is itself the evidence that the manager was usable here.
                return transport.send(request).error.isNull();
            });
            for (const bool ok : sameThread) {
                QVERIFY(ok);
            }
        }
    }

    /// Cancellation has to reach a transfer that is already running, or Quit waits for
    /// it to finish.
    void cancellationRefusesFurtherWork()
    {
        NetworkTransport transport;
        QVERIFY(!transport.isCancelled());

        HttpRequest request;
        request.url = serverUrl();
        request.transferTimeoutSeconds = 5;
        QVERIFY(transport.send(request).error.isNull());

        transport.cancelAll();
        QVERIFY(transport.isCancelled());

        QElapsedTimer timer;
        timer.start();
        const HttpResponse response = transport.send(request);
        QVERIFY(!response.error.isNull());
        // Immediately, not after a timeout.
        QVERIFY(timer.elapsed() < 1000);
    }

    /// A request to a port with nothing behind it must fail rather than wait, or one
    /// unreachable server would pin a transfer slot for ever.
    void reportsAConnectionRefusalPromptly()
    {
        NetworkTransport transport;
        HttpRequest request;
        // Port 1 is reserved and nothing listens there.
        request.url = QUrl(QStringLiteral("http://127.0.0.1:1/"));
        request.transferTimeoutSeconds = 5;

        QElapsedTimer timer;
        timer.start();
        const HttpResponse response = transport.send(request);
        QVERIFY(!response.error.isNull());
        QVERIFY2(timer.elapsed() < 10000,
                 qUtf8Printable(QStringLiteral("took %1ms").arg(timer.elapsed())));
    }

private:
    QUrl serverUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_port));
    }

    QThread *m_serverThread = nullptr;
    TinyHttpServer *m_server = nullptr;
    quint16 m_port = 0;
};

QTEST_MAIN(TransportTest)
#include "TransportTest.moc"
