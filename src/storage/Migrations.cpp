#include "storage/Migrations.h"

#include "core/Logging.h"

namespace immichksync {

namespace SchemaMigrations {

QList<Migration> all()
{
    return {
        Migration{
            1,
            {
                QStringLiteral(R"(
                CREATE TABLE album (
                    album_id           TEXT PRIMARY KEY NOT NULL,
                    album_name         TEXT NOT NULL,
                    folder_name        TEXT NOT NULL,
                    remote_updated_at  TEXT,
                    remote_asset_count INTEGER NOT NULL DEFAULT 0,
                    last_deep_scan_at  REAL,
                    last_synced_at     REAL,
                    is_excluded        INTEGER NOT NULL DEFAULT 0,
                    safety_hold        INTEGER NOT NULL DEFAULT 0
                )
                )"),
                QStringLiteral("CREATE UNIQUE INDEX album_folder_name_unique ON album (folder_name)"),
                QStringLiteral(R"(
                CREATE TABLE asset (
                    album_id           TEXT NOT NULL REFERENCES album (album_id) ON DELETE CASCADE,
                    checksum           TEXT NOT NULL,
                    asset_id           TEXT NOT NULL,
                    original_file_name TEXT NOT NULL,
                    relative_path      TEXT NOT NULL,
                    size               INTEGER NOT NULL DEFAULT 0,
                    synced_at          REAL NOT NULL,
                    PRIMARY KEY (album_id, checksum)
                )
                )"),
                QStringLiteral("CREATE INDEX asset_by_album_asset_id ON asset (album_id, asset_id)"),
                QStringLiteral("CREATE INDEX asset_by_relative_path ON asset (relative_path)"),
                QStringLiteral(R"(
                CREATE TABLE local_file (
                    relative_path        TEXT PRIMARY KEY NOT NULL,
                    device_id            INTEGER NOT NULL,
                    inode                INTEGER NOT NULL,
                    size                 INTEGER NOT NULL,
                    modified_at_nanos    INTEGER NOT NULL,
                    checksum             TEXT NOT NULL,
                    hashed_at            REAL NOT NULL
                )
                )"),
                QStringLiteral(R"(
                CREATE TABLE held_removal (
                    album_id     TEXT NOT NULL REFERENCES album (album_id) ON DELETE CASCADE,
                    checksum     TEXT NOT NULL,
                    direction    TEXT NOT NULL,
                    display_name TEXT NOT NULL,
                    detected_at  REAL NOT NULL,
                    PRIMARY KEY (album_id, checksum)
                )
                )"),
                QStringLiteral(R"(
                CREATE TABLE transfer_failure (
                    key             TEXT PRIMARY KEY NOT NULL,
                    attempts        INTEGER NOT NULL DEFAULT 0,
                    last_error      TEXT NOT NULL DEFAULT '',
                    next_attempt_at REAL NOT NULL DEFAULT 0
                )
                )"),
                QStringLiteral(R"(
                CREATE TABLE meta (
                    key   TEXT PRIMARY KEY NOT NULL,
                    value TEXT NOT NULL
                )
                )"),
            }},
    };
}

int currentVersion()
{
    int highest = 0;
    for (const Migration &migration : all()) {
        highest = std::max(highest, migration.version);
    }
    return highest;
}

bool apply(SqliteDatabase &database, QString *errorMessage)
{
    const int existing = database.schemaVersion();
    const int target = currentVersion();
    if (existing >= target) {
        return true;
    }

    for (const Migration &migration : all()) {
        if (migration.version <= existing) {
            continue;
        }
        QString failure;
        const bool ok = database.transaction(
            [&]() {
                for (const QString &statement : migration.statements) {
                    if (!database.execute(statement, &failure)) {
                        return false;
                    }
                }
                return database.setSchemaVersion(migration.version, &failure);
            },
            &failure);

        if (!ok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Schema migration v%1 failed: %2")
                                    .arg(migration.version)
                                    .arg(failure);
            }
            return false;
        }
        log::storage.info(QStringLiteral("Applied schema migration v%1").arg(migration.version));
    }
    return true;
}

} // namespace SchemaMigrations

} // namespace immichksync
