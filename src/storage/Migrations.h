#pragma once

#include "storage/SqliteDatabase.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace immichksync {

/// Ordered, forward-only schema migrations applied against `PRAGMA user_version`.
///
/// Every migration must be additive or safely destructive of derived data only: the
/// database is a cache of reconciled state, so the worst case is a rebuild, never data
/// loss on disk or on the server.
///
/// The SQL below is byte-identical to the macOS build's. That is the compatibility
/// contract — a sync folder and its state must be readable by either implementation —
/// so any change here has to be made in both places, as a new migration in both.
namespace SchemaMigrations {

struct Migration {
    int version = 0;
    QStringList statements;
};

QList<Migration> all();
int currentVersion();

bool apply(SqliteDatabase &database, QString *errorMessage);

} // namespace SchemaMigrations

} // namespace immichksync
