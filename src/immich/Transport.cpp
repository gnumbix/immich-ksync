#include "immich/Transport.h"

#include "core/Logging.h"
#include "credentials/TlsCertificateStore.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAtomicInt>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QThread>
#include <QThreadStorage>
#include <QTimer>

namespace immichksync {

QString HttpResponse::header(const QString &name) const
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return {};
}

QString HttpResponse::suggestedFilename() const
{
    const QString disposition = header(QStringLiteral("Content-Disposition"));
    if (disposition.isEmpty()) {
        return {};
    }
    // `attachment; filename="IMG_0001.HEIC"` — the quoted form is what Immich sends.
    const int marker = disposition.indexOf(QStringLiteral("filename="), 0, Qt::CaseInsensitive);
    if (marker < 0) {
        return {};
    }
    QString value = disposition.mid(marker + 9).trimmed();
    if (value.startsWith(QLatin1Char('"'))) {
        value.remove(0, 1);
        const int end = value.indexOf(QLatin1Char('"'));
        if (end >= 0) {
            value.truncate(end);
        }
    } else {
        const int end = value.indexOf(QLatin1Char(';'));
        if (end >= 0) {
            value.truncate(end);
        }
    }
    return value;
}

// MARK: - NetworkTransport

class NetworkTransport::Private {
public:
    explicit Private(TlsCertificateStore *certificates)
        : certificates(certificates)
    {
    }

    /// This thread's manager.
    ///
    /// QThreadStorage rather than a hash keyed on the thread id: the transfer pools are
    /// created per cycle, so their threads die and the operating system hands the same
    /// ids to the next batch. A manager cached under a recycled id belongs to a thread
    /// that no longer exists — it has no event dispatcher, so `finished()` never
    /// arrives, the reply's own timeout never fires, and the request waits for ever.
    /// QThreadStorage ties the manager's lifetime to the thread instead, and destroys
    /// it there when the thread exits.
    QNetworkAccessManager *manager()
    {
        if (!managers.hasLocalData()) {
            auto *created = new QNetworkAccessManager();
            created->setAutoDeleteReplies(false);
            // Our own retry policy sits above this, so failing fast and backing off
            // beats Qt silently waiting for connectivity.
            created->setTransferTimeout(std::chrono::seconds(0));
            managers.setLocalData(created);
            generations.setLocalData(generation.loadAcquire());
        }

        // A certificate changed since this thread last looked. Reaching into another
        // thread's manager is what the old code could not do safely, so each thread
        // drops its own pooled connections when it notices the bump.
        const int current = generation.loadAcquire();
        if (generations.localData() != current) {
            managers.localData()->clearConnectionCache();
            managers.localData()->clearAccessCache();
            generations.setLocalData(current);
        }
        return managers.localData();
    }

    void reset() { generation.fetchAndAddOrdered(1); }

    TlsCertificateStore *certificates;
    /// Owns one manager per thread and deletes it when that thread exits.
    QThreadStorage<QNetworkAccessManager *> managers;
    QThreadStorage<int> generations;
    QAtomicInt generation = 0;
    QAtomicInt cancelled = 0;
};

NetworkTransport::NetworkTransport(TlsCertificateStore *certificates)
    : d(std::make_unique<Private>(certificates))
{
}

NetworkTransport::~NetworkTransport() = default;

QString NetworkTransport::userAgent()
{
    return QStringLiteral("ImmichKSync/%1 (Linux)").arg(QLatin1String(IMMICHKSYNC_VERSION));
}

void NetworkTransport::resetConnections()
{
    d->reset();
}

void NetworkTransport::cancelAll()
{
    d->cancelled.storeRelease(1);
}

bool NetworkTransport::isCancelled() const
{
    return d->cancelled.loadAcquire() != 0;
}

HttpResponse NetworkTransport::send(const HttpRequest &request)
{
    HttpResponse response;

    if (isCancelled()) {
        response.error = ImmichError::local(QStringLiteral("The application is shutting down."));
        return response;
    }

    QNetworkRequest networkRequest(request.url);
    networkRequest.setRawHeader("Accept", "application/json");
    networkRequest.setRawHeader("User-Agent", userAgent().toUtf8());
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        networkRequest.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                request.followRedirects
                                    ? QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy)
                                    : QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
    // Aborts a stalled transfer without capping a slow large one.
    networkRequest.setTransferTimeout(std::chrono::seconds(request.transferTimeoutSeconds));

    if (d->certificates && request.url.scheme() == QLatin1String("https")) {
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        if (d->certificates->apply(configuration, request.url.host())) {
            networkRequest.setSslConfiguration(configuration);
        }
    }

    std::unique_ptr<QFile> uploadFile;
    if (!request.uploadFilePath.isEmpty()) {
        uploadFile = std::make_unique<QFile>(request.uploadFilePath);
        if (!uploadFile->open(QIODevice::ReadOnly)) {
            response.error = ImmichError::local(
                QStringLiteral("Could not read %1 for upload.")
                    .arg(QFileInfo(request.uploadFilePath).fileName()));
            return response;
        }
    }

    QNetworkAccessManager *manager = d->manager();
    QNetworkReply *reply = nullptr;
    const QByteArray verb = request.method.toUtf8();

    if (uploadFile) {
        reply = manager->sendCustomRequest(networkRequest, verb, uploadFile.get());
    } else if (request.body.isEmpty() && request.method == QLatin1String("GET")) {
        reply = manager->get(networkRequest);
    } else {
        reply = manager->sendCustomRequest(networkRequest, verb, request.body);
    }

    // Streaming to disk as the data arrives is what keeps a 4 GiB original from being
    // buffered in memory before it is written.
    std::unique_ptr<QFile> downloadFile;
    if (!request.downloadFilePath.isEmpty()) {
        downloadFile = std::make_unique<QFile>(request.downloadFilePath);
        if (!downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            reply->abort();
            reply->deleteLater();
            response.error = ImmichError::local(QStringLiteral("Could not write to %1.")
                                                    .arg(request.downloadFilePath));
            return response;
        }
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
            const QByteArray chunk = reply->readAll();
            downloadFile->write(chunk);
            response.downloadedBytes += chunk.size();
        });
    }

    // Never `ignoreSslErrors()`. A certificate the system and the configured anchor
    // both reject must fail the request, and the error it produces names the reason.
    QObject::connect(reply,
                     &QNetworkReply::sslErrors,
                     reply,
                     [](const QList<QSslError> &errors) {
                         for (const QSslError &error : errors) {
                             log::api.warning(QStringLiteral("TLS: %1").arg(error.errorString()));
                         }
                     });

    QElapsedTimer elapsed;
    elapsed.start();
    log::api.debug(QStringLiteral("→ %1 %2").arg(request.method, request.url.toString()));

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Shutdown has to reach a transfer that is already running, or Quit waits for a
    // multi-gigabyte download to finish before the process can exit.
    QTimer cancellation;
    cancellation.setInterval(250);
    QObject::connect(&cancellation, &QTimer::timeout, reply, [this, reply]() {
        if (isCancelled()) {
            reply->abort();
        }
    });
    cancellation.start();
    // Safe here because this always runs on a worker thread whose event loop has
    // nothing else queued on it; the GUI thread never calls send().
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    if (downloadFile) {
        const QByteArray tail = reply->readAll();
        downloadFile->write(tail);
        response.downloadedBytes += tail.size();
        downloadFile->close();
    } else {
        response.body = reply->readAll();
    }

    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    response.statusCode = status.isValid() ? status.toInt() : 0;

    const auto headers = reply->rawHeaderPairs();
    for (const auto &pair : headers) {
        response.headers.insert(QString::fromUtf8(pair.first), QString::fromUtf8(pair.second));
    }

    if (reply->error() != QNetworkReply::NoError && !status.isValid()) {
        // No status at all: the failure happened below HTTP, so it is a transport or
        // TLS problem rather than something the server said.
        response.error = ImmichError::fromNetworkError(reply->error(),
                                                       reply->errorString(),
                                                       request.url.host());
    }

    log::api.debug(QStringLiteral("← %1 %2 — HTTP %3 in %4 ms%5")
                       .arg(request.method,
                            request.url.toString())
                       .arg(response.statusCode)
                       .arg(elapsed.elapsed())
                       .arg(response.error.isNull()
                                ? QString()
                                : QStringLiteral(" (%1)").arg(response.error.message())));

    reply->deleteLater();
    return response;
}

} // namespace immichksync
