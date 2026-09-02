#pragma once

#include "immich/Transport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QFile>

#include <functional>

namespace immichksync {

/// Captures outgoing requests and replays canned responses, so the client's wire
/// format can be asserted without a server.
///
/// The equivalent of the macOS build's `StubURLProtocol`, but a plain object rather
/// than a protocol-stack replacement — which is the whole reason `Transport` is an
/// interface.
class StubTransport : public Transport {
public:
    struct Exchange {
        int statusCode = 200;
        QByteArray body;
        QMap<QString, QString> headers{{QStringLiteral("Content-Type"),
                                        QStringLiteral("application/json")}};
        /// When set, the request fails at the transport layer instead of answering.
        ImmichError error;
    };

    HttpResponse send(const HttpRequest &request) override
    {
        QMutexLocker locker(&m_mutex);
        m_requests.append(request);

        Exchange exchange;
        if (!m_queue.isEmpty()) {
            exchange = m_queue.takeFirst();
        } else if (m_handler) {
            exchange = m_handler(request);
        } else {
            exchange.statusCode = 404;
            exchange.body = QByteArray(R"({"message":"no stub queued"})");
        }

        HttpResponse response;
        response.statusCode = exchange.statusCode;
        response.headers = exchange.headers;
        response.error = exchange.error;
        if (!request.downloadFilePath.isEmpty() && exchange.error.isNull()) {
            QFile file(request.downloadFilePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(exchange.body);
                response.downloadedBytes = exchange.body.size();
            }
        } else {
            response.body = exchange.body;
        }
        return response;
    }

    /// Queues one canned answer. Answers are consumed in order.
    void enqueue(Exchange exchange)
    {
        QMutexLocker locker(&m_mutex);
        m_queue.append(std::move(exchange));
    }

    void enqueueJson(const QJsonObject &object, int statusCode = 200)
    {
        enqueue({statusCode, QJsonDocument(object).toJson(QJsonDocument::Compact), {}, {}});
    }

    void enqueueJson(const QJsonArray &array, int statusCode = 200)
    {
        enqueue({statusCode, QJsonDocument(array).toJson(QJsonDocument::Compact), {}, {}});
    }

    void enqueueError(int statusCode, const QString &message)
    {
        QJsonObject body;
        body.insert(QStringLiteral("message"), message);
        body.insert(QStringLiteral("statusCode"), statusCode);
        enqueue({statusCode, QJsonDocument(body).toJson(QJsonDocument::Compact), {}, {}});
    }

    /// Answers every request from a function rather than a queue, for pagination tests.
    void setHandler(std::function<Exchange(const HttpRequest &)> handler)
    {
        QMutexLocker locker(&m_mutex);
        m_handler = std::move(handler);
    }

    QList<HttpRequest> requests() const
    {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

    HttpRequest lastRequest() const
    {
        QMutexLocker locker(&m_mutex);
        return m_requests.isEmpty() ? HttpRequest{} : m_requests.last();
    }

    int requestCount() const
    {
        QMutexLocker locker(&m_mutex);
        return static_cast<int>(m_requests.size());
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_requests.clear();
        m_queue.clear();
    }

    /// The JSON body of request `index`, for asserting on what went out.
    static QJsonObject bodyOf(const HttpRequest &request)
    {
        return QJsonDocument::fromJson(request.body).object();
    }

private:
    mutable QMutex m_mutex;
    QList<HttpRequest> m_requests;
    QList<Exchange> m_queue;
    std::function<Exchange(const HttpRequest &)> m_handler;
};

} // namespace immichksync
