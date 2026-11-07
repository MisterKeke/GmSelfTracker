#include "infrastructure/database/web_data_import_service.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <utility>

namespace gmst {
namespace {

const QStringList importTables = {
    QStringLiteral("uploaded_assets"), QStringLiteral("users"), QStringLiteral("games"),
    QStringLiteral("sections"), QStringLiteral("object_types"), QStringLiteral("field_definitions"),
    QStringLiteral("objects"), QStringLiteral("field_values"), QStringLiteral("object_relationships"),
    QStringLiteral("section_placements"), QStringLiteral("goals"), QStringLiteral("goal_tasks"),
    QStringLiteral("section_goal_placements"),
    QStringLiteral("object_history_events"), QStringLiteral("object_history_changes"),
    QStringLiteral("field_type_migrations"), QStringLiteral("game_templates"),
    QStringLiteral("object_presets"), QStringLiteral("goal_dependencies"),
    QStringLiteral("game_dashboards"), QStringLiteral("object_gallery")
};

const QStringList expectedVersions = {
    QStringLiteral("001_initial.sql"), QStringLiteral("002_local_features.sql"),
    QStringLiteral("003_asset_lifecycle.sql"), QStringLiteral("004_primary_display.sql"),
    QStringLiteral("005_object_history.sql"), QStringLiteral("006_field_type_migrations.sql"),
    QStringLiteral("007_game_templates.sql"), QStringLiteral("008_object_presets.sql"),
    QStringLiteral("009_goal_dependencies.sql"), QStringLiteral("010_game_dashboards.sql"),
    QStringLiteral("011_object_gallery.sql")
};

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool samePath(const QString &left, const QString &right)
{
    return normalizedPath(left).compare(normalizedPath(right), Qt::CaseInsensitive) == 0;
}

bool execute(QSqlDatabase database, const QString &sql, QString *error)
{
    QSqlQuery query(database);
    if (query.exec(sql)) return true;
    if (error) *error = query.lastError().text();
    return false;
}

bool checkNoForeignKeyErrors(QSqlDatabase database, QString *error)
{
    QSqlQuery check(database);
    if (!check.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
        if (error) *error = check.lastError().text();
        return false;
    }
    if (check.next()) {
        if (error) *error = QStringLiteral("SQLite foreign key validation failed in table %1 (row %2).")
            .arg(check.value(0).toString(), check.value(1).toString());
        return false;
    }
    return true;
}

bool validateSource(QSqlDatabase database, QString *error)
{
    QSqlQuery integrity(database);
    if (!integrity.exec(QStringLiteral("PRAGMA integrity_check")) || !integrity.next() ||
        integrity.value(0).toString().compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0) {
        if (error) *error = integrity.lastError().isValid() ? integrity.lastError().text()
            : QStringLiteral("The source SQLite database failed its integrity check.");
        return false;
    }
    QSqlQuery migrations(database);
    if (!migrations.exec(QStringLiteral("SELECT version FROM schema_migrations ORDER BY version"))) {
        if (error) *error = QStringLiteral("This is not a supported GmSelfTracker database: %1").arg(migrations.lastError().text());
        return false;
    }
    int expected = 1;
    while (migrations.next()) {
        if (migrations.value(0).toInt() != expected || expected > expectedVersions.size()) {
            if (error) *error = QStringLiteral("The source database has an unsupported or non-contiguous migration history.");
            return false;
        }
        ++expected;
    }
    if (expected != expectedVersions.size() + 1) {
        if (error) *error = QStringLiteral("Update the Web database to schema version 11 before importing it.");
        return false;
    }
    QSqlQuery required(database);
    required.prepare(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?"));
    for (const QString &table : importTables + QStringList{QStringLiteral("schema_migrations"), QStringLiteral("asset_references")}) {
        required.bindValue(0, table);
        if (!required.exec() || !required.next() || required.value(0).toInt() != 1) {
            if (error) *error = QStringLiteral("The source schema is missing required table %1.").arg(table);
            return false;
        }
    }
    if (!checkNoForeignKeyErrors(database, error)) return false;
    QSqlQuery deletedReferences(database);
    if (!deletedReferences.exec(QStringLiteral("SELECT a.id FROM uploaded_assets a JOIN asset_references r ON r.asset_id=a.id WHERE a.deleted_at IS NOT NULL LIMIT 1"))) {
        if (error) *error = deletedReferences.lastError().text();
        return false;
    }
    if (deletedReferences.next()) {
        if (error) *error = QStringLiteral("The source links a deleted upload to a saved record. Restore or repair that asset in Web before importing.");
        return false;
    }
    return true;
}

bool sourceAccountsDoNotCollide(QSqlDatabase source, QSqlDatabase target, QString *error)
{
    QSqlQuery users(source);
    if (!users.exec(QStringLiteral("SELECT id,username FROM users ORDER BY username COLLATE NOCASE"))) {
        if (error) *error = users.lastError().text();
        return false;
    }
    while (users.next()) {
        const QString id = users.value(0).toString();
        const QString username = users.value(1).toString();
        QSqlQuery conflict(target);
        conflict.prepare(QStringLiteral("SELECT id,username FROM users WHERE id=? OR username=? COLLATE NOCASE LIMIT 1"));
        conflict.addBindValue(id);
        conflict.addBindValue(username);
        if (!conflict.exec()) {
            if (error) *error = conflict.lastError().text();
            return false;
        }
        if (conflict.next()) {
            if (error) *error = QStringLiteral("Import stopped because account %1 conflicts with an existing desktop account. No records were changed; resolve the username or ID collision and retry.").arg(username);
            return false;
        }
    }
    if (users.lastError().isValid()) {
        if (error) *error = users.lastError().text();
        return false;
    }
    return true;
}

struct AssetFile {
    QString filename;
    QString stagedPath;
    QString targetPath;
    QByteArray digest;
    bool alreadyPresent = false;
};

QByteArray fileDigest(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            if (error) *error = file.errorString();
            return {};
        }
        hash.addData(block);
    }
    return hash.result();
}

bool safeUploadName(const QString &filename)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\.png$"));
    return pattern.match(filename).hasMatch();
}

bool stageAssets(QSqlDatabase source, const QString &sourceUploadsPath, const QString &targetUploadsPath,
                 const QString &stagingRoot, QVector<AssetFile> *files, int *count, QString *error)
{
    const QFileInfo sourceRootInfo(sourceUploadsPath);
    const QString sourceRoot = QDir::cleanPath(QDir::fromNativeSeparators(sourceRootInfo.canonicalFilePath()));
    if (!sourceRootInfo.exists() || !sourceRootInfo.isDir() || sourceRoot.isEmpty()) {
        if (error) *error = QStringLiteral("Choose the original Web uploads directory.");
        return false;
    }
    const QFileInfo targetRootInfo(targetUploadsPath);
    if (targetRootInfo.exists() && (targetRootInfo.isSymLink() ||
        QDir::cleanPath(QDir::fromNativeSeparators(targetRootInfo.canonicalFilePath())) != normalizedPath(targetUploadsPath))) {
        if (error) *error = QStringLiteral("The desktop image directory resolves outside its expected location.");
        return false;
    }
    const QString rootPrefix = sourceRoot.endsWith(QLatin1Char('/')) ? sourceRoot : sourceRoot + QLatin1Char('/');
    QSqlQuery assets(source);
    if (!assets.exec(QStringLiteral("SELECT filename,size_bytes,deleted_at FROM uploaded_assets ORDER BY filename"))) {
        if (error) *error = assets.lastError().text();
        return false;
    }
    int total = 0;
    while (assets.next()) {
        ++total;
        const QString filename = assets.value(0).toString();
        const qint64 expectedSize = assets.value(1).toLongLong();
        if (!safeUploadName(filename) || expectedSize < 1 || expectedSize > 5LL * 1024 * 1024) {
            if (error) *error = QStringLiteral("The source contains an unsafe or unsupported upload filename or size.");
            return false;
        }
        const QString sourcePath = QDir(sourceRoot).filePath(filename);
        const QFileInfo sourceFileInfo(sourcePath);
        if (!sourceFileInfo.exists()) {
            if (!assets.value(2).isNull()) continue;
            if (error) *error = QStringLiteral("Required upload %1 is missing from the selected Web uploads directory.").arg(filename);
            return false;
        }
        if (!sourceFileInfo.isFile() || sourceFileInfo.isSymLink() ||
            !QDir::cleanPath(QDir::fromNativeSeparators(sourceFileInfo.canonicalFilePath())).startsWith(rootPrefix, Qt::CaseInsensitive) ||
            sourceFileInfo.size() != expectedSize) {
            if (error) *error = QStringLiteral("Upload %1 is outside the selected directory or does not match its saved size.").arg(filename);
            return false;
        }
        const QString stagedPath = QDir(stagingRoot).filePath(filename);
        if (!QFile::copy(sourcePath, stagedPath)) {
            if (error) *error = QStringLiteral("Could not stage upload %1.").arg(filename);
            return false;
        }
        QString digestError;
        const QByteArray sourceDigest = fileDigest(stagedPath, &digestError);
        if (sourceDigest.isEmpty()) {
            if (error) *error = QStringLiteral("Could not verify upload %1: %2").arg(filename, digestError);
            return false;
        }
        const QString targetPath = QDir(targetUploadsPath).filePath(filename);
        AssetFile asset{filename, stagedPath, targetPath, sourceDigest, false};
        const QFileInfo existing(targetPath);
        if (existing.exists()) {
            if (!existing.isFile() || existing.isSymLink() || existing.size() != expectedSize) {
                if (error) *error = QStringLiteral("An existing desktop upload conflicts with %1; it was not overwritten.").arg(filename);
                return false;
            }
            const QByteArray targetDigest = fileDigest(targetPath, &digestError);
            if (targetDigest.isEmpty() || targetDigest != sourceDigest) {
                if (error) *error = QStringLiteral("An existing desktop upload conflicts with %1; it was not overwritten.").arg(filename);
                return false;
            }
            asset.alreadyPresent = true;
        }
        files->append(asset);
    }
    if (assets.lastError().isValid()) {
        if (error) *error = assets.lastError().text();
        return false;
    }
    if (count) *count = total;
    return true;
}

bool copyTable(QSqlDatabase source, QSqlDatabase target, const QString &table, QString *error)
{
    QSqlQuery rows(source);
    const QString selectSql = QStringLiteral("SELECT * FROM \"%1\"").arg(table);
    if (!rows.exec(selectSql)) {
        if (error) *error = QStringLiteral("Could not read %1: %2").arg(table, rows.lastError().text());
        return false;
    }
    const QSqlRecord record = rows.record();
    QStringList columns;
    QStringList placeholders;
    for (int index = 0; index < record.count(); ++index) {
        const QString column = record.fieldName(index);
        columns.append(QStringLiteral("\"%1\"").arg(column));
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery insert(target);
    insert.prepare(QStringLiteral("INSERT INTO \"%1\"(%2) VALUES(%3)")
        .arg(table, columns.join(QLatin1Char(',')), placeholders.join(QLatin1Char(','))));
    while (rows.next()) {
        for (int index = 0; index < record.count(); ++index) {
            QVariant value = rows.value(index);
            if (table == QStringLiteral("object_types") && record.fieldName(index) == QStringLiteral("primary_display_field_id"))
                value = QStringLiteral("");
            insert.bindValue(index, value);
        }
        if (!insert.exec()) {
            if (error) *error = QStringLiteral("Could not import %1; an ID collision or incompatible row stopped the transaction: %2")
                .arg(table, insert.lastError().text());
            return false;
        }
    }
    if (rows.lastError().isValid()) {
        if (error) *error = rows.lastError().text();
        return false;
    }
    return true;
}

bool restorePrimaryDisplayFields(QSqlDatabase source, QSqlDatabase target, QString *error)
{
    QSqlQuery rows(source);
    if (!rows.exec(QStringLiteral("SELECT id,primary_display_field_id FROM object_types WHERE primary_display_field_id<>'' ORDER BY id"))) {
        if (error) *error = rows.lastError().text();
        return false;
    }
    while (rows.next()) {
        QSqlQuery update(target);
        update.prepare(QStringLiteral("UPDATE object_types SET primary_display_field_id=? WHERE id=?"));
        update.addBindValue(rows.value(1));
        update.addBindValue(rows.value(0));
        if (!update.exec() || update.numRowsAffected() != 1) {
            if (error) *error = update.lastError().isValid() ? update.lastError().text()
                : QStringLiteral("Could not restore an imported primary display field.");
            return false;
        }
    }
    if (rows.lastError().isValid()) {
        if (error) *error = rows.lastError().text();
        return false;
    }
    return true;
}

bool copyAssetReferences(QSqlDatabase source, QSqlDatabase target, QString *error)
{
    QSqlQuery rows(source);
    if (!rows.exec(QStringLiteral("SELECT entity_type,entity_id,slot,asset_id FROM asset_references ORDER BY entity_type,entity_id,slot"))) {
        if (error) *error = rows.lastError().text();
        return false;
    }
    while (rows.next()) {
        const QString entityType = rows.value(0).toString();
        const QString entityId = rows.value(1).toString();
        const QString slot = rows.value(2).toString();
        const QString assetId = rows.value(3).toString();
        QSqlQuery existing(target);
        existing.prepare(QStringLiteral("SELECT asset_id FROM asset_references WHERE entity_type=? AND entity_id=? AND slot=?"));
        existing.addBindValue(entityType);
        existing.addBindValue(entityId);
        existing.addBindValue(slot);
        if (!existing.exec()) {
            if (error) *error = existing.lastError().text();
            return false;
        }
        if (existing.next()) {
            if (existing.value(0).toString() != assetId) {
                if (error) *error = QStringLiteral("An existing desktop image reference conflicts with imported entity %1; no data was overwritten.").arg(entityId);
                return false;
            }
            continue;
        }
        QSqlQuery insert(target);
        insert.prepare(QStringLiteral("INSERT INTO asset_references(entity_type,entity_id,slot,asset_id) VALUES(?,?,?,?)"));
        insert.addBindValue(entityType);
        insert.addBindValue(entityId);
        insert.addBindValue(slot);
        insert.addBindValue(assetId);
        if (!insert.exec()) {
            if (error) *error = insert.lastError().text();
            return false;
        }
    }
    if (rows.lastError().isValid()) {
        if (error) *error = rows.lastError().text();
        return false;
    }
    return true;
}

bool setGalleryValidationTriggers(QSqlDatabase database, bool create, QString *error)
{
    const QStringList statements = create ? QStringList{
        QStringLiteral("CREATE TRIGGER object_gallery_validate_insert BEFORE INSERT ON object_gallery WHEN NOT EXISTS (SELECT 1 FROM uploaded_assets a JOIN objects o ON o.id=NEW.object_id JOIN games g ON g.id=o.game_id JOIN object_types t ON t.id=o.object_type_id WHERE a.id=NEW.asset_id AND a.user_id=g.user_id AND a.deleted_at IS NULL AND o.archived=0 AND t.archived=0 AND g.archived=0) BEGIN SELECT RAISE(ABORT,'gallery image or object is unavailable'); END"),
        QStringLiteral("CREATE TRIGGER object_gallery_validate_asset_update BEFORE UPDATE OF asset_id ON object_gallery WHEN NOT EXISTS (SELECT 1 FROM uploaded_assets a JOIN objects o ON o.id=NEW.object_id JOIN games g ON g.id=o.game_id JOIN object_types t ON t.id=o.object_type_id WHERE a.id=NEW.asset_id AND a.user_id=g.user_id AND a.deleted_at IS NULL AND o.archived=0 AND t.archived=0 AND g.archived=0) BEGIN SELECT RAISE(ABORT,'gallery image or object is unavailable'); END")
    } : QStringList{
        QStringLiteral("DROP TRIGGER IF EXISTS object_gallery_validate_insert"),
        QStringLiteral("DROP TRIGGER IF EXISTS object_gallery_validate_asset_update")
    };
    for (const QString &statement : statements) {
        if (!execute(database, statement, error)) return false;
    }
    return true;
}

bool installAssets(const QVector<AssetFile> &files, QStringList *installedPaths, QString *error)
{
    if (files.isEmpty()) return true;
    const QString rootPath = normalizedPath(QFileInfo(files.first().targetPath).absolutePath());
    if (!QDir().mkpath(rootPath)) {
        if (error) *error = QStringLiteral("Could not create the desktop uploads directory.");
        return false;
    }
    const QFileInfo rootInfo(rootPath);
    const QString canonicalRoot = QDir::cleanPath(QDir::fromNativeSeparators(rootInfo.canonicalFilePath()));
    if (canonicalRoot.isEmpty() || canonicalRoot != rootPath || rootInfo.isSymLink()) {
        if (error) *error = QStringLiteral("The desktop uploads directory resolves outside its expected location.");
        return false;
    }
    const QString prefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    for (const AssetFile &asset : files) {
        if (asset.alreadyPresent) continue;
        const QString destination = normalizedPath(asset.targetPath);
        if (!destination.startsWith(prefix, Qt::CaseInsensitive) || QFileInfo::exists(destination)) {
            if (error) *error = QStringLiteral("The destination upload path changed during import; no existing file was overwritten.");
            return false;
        }
        QFile input(asset.stagedPath);
        if (!input.open(QIODevice::ReadOnly)) {
            if (error) *error = input.errorString();
            return false;
        }
        QSaveFile output(destination);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) {
            if (error) *error = output.errorString();
            return false;
        }
        while (!input.atEnd()) {
            const QByteArray block = input.read(64 * 1024);
            if (block.isEmpty() && input.error() != QFileDevice::NoError) {
                output.cancelWriting();
                if (error) *error = input.errorString();
                return false;
            }
            if (output.write(block) != block.size()) {
                output.cancelWriting();
                if (error) *error = output.errorString();
                return false;
            }
        }
        if (!output.commit()) {
            if (error) *error = output.errorString();
            return false;
        }
        installedPaths->append(destination);
    }
    return true;
}

bool scalarCount(QSqlDatabase database, const QString &sql, int *value, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    *value = query.value(0).toInt();
    return true;
}

} // namespace

WebDataImportService::WebDataImportService(QSqlDatabase targetDatabase, QString targetDatabasePath, QString targetUploadsPath)
    : m_targetDatabase(std::move(targetDatabase)), m_targetDatabasePath(std::move(targetDatabasePath)),
      m_targetUploadsPath(std::move(targetUploadsPath))
{
}

bool WebDataImportService::import(const QString &sourceDatabasePath, const QString &sourceUploadsPath,
                                  WebImportSummary *summary, QString *error)
{
    if (error) error->clear();
    if (summary) *summary = {};
    const QFileInfo sourceInfo(sourceDatabasePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() || samePath(sourceDatabasePath, m_targetDatabasePath)) {
        if (error) *error = QStringLiteral("Choose an existing Web database that is different from the desktop database.");
        return false;
    }

    const QString sourceConnectionName = QStringLiteral("gmst-import-%1").arg(uuid());
    QSqlDatabase source = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), sourceConnectionName);
    source.setDatabaseName(sourceInfo.absoluteFilePath());
    source.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!source.open()) {
        if (error) *error = QStringLiteral("Could not open the Web database read-only: %1").arg(source.lastError().text());
        source = QSqlDatabase();
        QSqlDatabase::removeDatabase(sourceConnectionName);
        return false;
    }
    struct SourceCleanup {
        QString name;
        QSqlDatabase *database;
        ~SourceCleanup() {
            database->close();
            *database = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
        }
    } cleanup{sourceConnectionName, &source};
    if (!execute(source, QStringLiteral("PRAGMA query_only=ON"), error) || !source.transaction()) {
        if (error && error->isEmpty()) *error = source.lastError().text();
        return false;
    }
    // The integrity scan starts the read transaction. Every subsequent read,
    // including rows still in an active WAL file, uses this same SQLite snapshot.
    if (!validateSource(source, error) || !sourceAccountsDoNotCollide(source, m_targetDatabase, error)) return false;

    const QString stagingPattern = QDir(QFileInfo(m_targetDatabasePath).absolutePath()).filePath(QStringLiteral(".gmst-import-XXXXXX"));
    QTemporaryDir staging(stagingPattern);
    if (!staging.isValid()) {
        if (error) *error = QStringLiteral("Could not create an isolated import staging directory.");
        return false;
    }
    QVector<AssetFile> stagedAssets;
    int assetCount = 0;
    if (!stageAssets(source, sourceUploadsPath, m_targetUploadsPath, staging.path(), &stagedAssets, &assetCount, error)) return false;

    WebImportSummary result;
    if (!scalarCount(source, QStringLiteral("SELECT COUNT(*) FROM users"), &result.accounts, error) ||
        !scalarCount(source, QStringLiteral("SELECT COUNT(*) FROM games"), &result.games, error) ||
        !scalarCount(source, QStringLiteral("SELECT COUNT(*) FROM objects"), &result.objects, error)) return false;
    result.assets = assetCount;

    if (!m_targetDatabase.transaction()) {
        if (error) *error = m_targetDatabase.lastError().text();
        return false;
    }
    QStringList installedFiles;
    auto fail = [&](const QString &detail) {
        m_targetDatabase.rollback();
        for (const QString &path : std::as_const(installedFiles)) QFile::remove(path);
        if (error) *error = detail;
        return false;
    };
    QString detail;
    if (!execute(m_targetDatabase, QStringLiteral("PRAGMA defer_foreign_keys=ON"), &detail) ||
        !setGalleryValidationTriggers(m_targetDatabase, false, &detail)) return fail(detail);

    for (const QString &table : importTables) {
        if (!copyTable(source, m_targetDatabase, table, &detail)) return fail(detail);
    }
    if (!restorePrimaryDisplayFields(source, m_targetDatabase, &detail) ||
        !setGalleryValidationTriggers(m_targetDatabase, true, &detail) ||
        !copyAssetReferences(source, m_targetDatabase, &detail) ||
        !checkNoForeignKeyErrors(m_targetDatabase, &detail)) return fail(detail);
    if (!installAssets(stagedAssets, &installedFiles, &detail)) return fail(detail);
    if (!m_targetDatabase.commit()) return fail(m_targetDatabase.lastError().text());
    if (summary) *summary = result;
    return true;
}

} // namespace gmst
