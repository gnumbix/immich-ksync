#pragma once

#include <QList>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <functional>

namespace immichksync {

namespace TaskPool {

/// Runs `work` over `items` with at most `concurrency` operations in flight,
/// returning results in input order.
///
/// The bound is what matters: each unit here is a multi-megabyte transfer, and an
/// unbounded map would open one connection per asset. A private QThreadPool rather
/// than the global one, so a large download batch cannot starve everything else.
template<typename Item, typename Result>
QList<Result> map(const QList<Item> &items,
                  int concurrency,
                  const std::function<Result(const Item &)> &work)
{
    QList<Result> results;
    if (items.isEmpty()) {
        return results;
    }

    const int limit = std::max(1, concurrency);
    if (limit == 1 || items.size() == 1) {
        results.reserve(items.size());
        for (const Item &item : items) {
            results.append(work(item));
        }
        return results;
    }

    QThreadPool pool;
    pool.setMaxThreadCount(std::min(limit, static_cast<int>(items.size())));
    return QtConcurrent::blockingMapped<QList<Result>>(&pool, items, work);
}

} // namespace TaskPool

/// Splits into consecutive slices of at most `size` elements. Used everywhere the
/// Immich API caps a batch (1000 album IDs, 5000 checksums).
template<typename T>
QList<QList<T>> chunked(const QList<T> &items, int size)
{
    Q_ASSERT(size > 0);
    QList<QList<T>> chunks;
    if (items.isEmpty()) {
        return chunks;
    }
    for (qsizetype offset = 0; offset < items.size(); offset += size) {
        chunks.append(items.mid(offset, size));
    }
    return chunks;
}

} // namespace immichksync
