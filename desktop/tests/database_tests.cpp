#include "infrastructure/database/game_repository.h"
#include "infrastructure/database/migration_runner.h"
#include "infrastructure/security/password_hasher.h"
#include "infrastructure/assets/image_decoder.h"
#include "domain/formula_evaluator.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QCryptographicHash>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QUuid>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <QtTest>

using namespace gmst;

namespace {

bool testExec(QSqlDatabase database, const QString &sql, const QVariantList &bindings = {}, QString *error = nullptr)
{
    QSqlQuery query(database);
    if (!query.prepare(sql)) {
        if (error) *error = query.lastError().text();
        return false;
    }
    for (const QVariant &binding : bindings) query.addBindValue(binding);
    if (query.exec()) return true;
    if (error) *error = query.lastError().text();
    return false;
}

QByteArray testFileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

} // namespace

class DatabaseTests final : public QObject {
    Q_OBJECT

private slots:
    void migrationSplitsTriggerBodies()
    {
        const QString script = QStringLiteral(
            "CREATE TABLE x(v INTEGER);"
            "CREATE TRIGGER tr AFTER INSERT ON x BEGIN "
            "SELECT CASE WHEN NEW.v < 0 THEN RAISE(ABORT,'bad; value') END; "
            "UPDATE x SET v=v+1 WHERE rowid=NEW.rowid; END;"
            "INSERT INTO x(v) VALUES(3);");
        const QStringList statements = MigrationRunner::splitSqlStatements(script);
        QCOMPARE(statements.size(), 3);
        QVERIFY(statements.at(1).contains(QStringLiteral("bad; value")));
        QVERIFY(statements.at(1).endsWith(QStringLiteral("END;")));
    }

    void appliesCurrentSourceMigrationsAndTriggers()
    {
        const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(QStringLiteral(":memory:"));
            QVERIFY2(database.open(), qPrintable(database.lastError().text()));
            QString error;
            QVERIFY2(MigrationRunner::apply(database, &error), qPrintable(error));
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 11);
            QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger'")));
            QVERIFY(query.next());
            QVERIFY(query.value(0).toInt() >= 20);
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void gameAndNestedSectionCrudUsesDisposableDatabase()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("test.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString id;
        QVERIFY2(repository.createGame(QStringLiteral("Test Game"), QStringLiteral("Disposable"), QStringLiteral("🎮"), &id, &error), qPrintable(error));
        QCOMPARE(repository.games(&error).size(), 1);
        QVERIFY2(repository.createSection(id, {}, QStringLiteral("Root"), QStringLiteral("✦"), &error), qPrintable(error));
        const QVector<Section> rootSections = repository.sections(id, &error);
        QCOMPARE(rootSections.size(), 1);
        QVERIFY2(repository.createSection(id, rootSections.first().id, QStringLiteral("Nested"), QStringLiteral("◈"), &error), qPrintable(error));
        QCOMPARE(repository.sections(id, &error).size(), 2);
    }

    void nativeAccountsVerifyGoBcryptAndPreserveLegacyOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("accounts.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString legacyGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Legacy library"), {}, {}, &legacyGameId, &error), qPrintable(error));
        QString importedHashId;
        const QByteArray goHash = QByteArrayLiteral("$2a$10$KjpSF9.nYuJs7YPB//ta2.hSnimkaYDMfMNjX8i.SeEcTk2MncVHS");
        {
            const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY2(database.open(), qPrintable(database.lastError().text()));
            QVERIFY2(testExec(database, QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES(?,?,?,?)"),
                {QStringLiteral("go-bcrypt-user"), QStringLiteral("Go_User"), QString::fromLatin1(goHash), QStringLiteral("2026-11-07T00:00:00Z")}, &error), qPrintable(error));
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
            importedHashId = QStringLiteral("go-bcrypt-user");
        }
        QVERIFY2(repository.authenticate(QStringLiteral("gO_uSeR"), QStringLiteral("imported-go-password"), nullptr, &error), qPrintable(error));
        QCOMPARE(repository.activeUsername(), QStringLiteral("Go_User"));
        QVERIFY(!repository.authenticate(QStringLiteral("Go_User"), QStringLiteral("wrong-password-123"), nullptr, &error));

        QString registeredId;
        QVERIFY2(repository.registerAccount(QStringLiteral("second_local"), QStringLiteral("native-password-123"), &registeredId, &error), qPrintable(error));
        QVERIFY(registeredId != importedHashId);
        QCOMPARE(repository.activeUsername(), QStringLiteral("second_local"));
        QVERIFY(repository.games(&error).isEmpty());
        QVERIFY(!repository.authenticate(QStringLiteral("second_local"), QStringLiteral("incorrect-password"), nullptr, &error));
        QVERIFY2(repository.changePassword(QStringLiteral("native-password-123"), QStringLiteral("changed-password-456"), &error), qPrintable(error));
        QVERIFY(!repository.authenticate(QStringLiteral("second_local"), QStringLiteral("native-password-123"), nullptr, &error));
        QVERIFY2(repository.authenticate(QStringLiteral("SECOND_LOCAL"), QStringLiteral("changed-password-456"), nullptr, &error), qPrintable(error));

        QVERIFY2(repository.secureLegacyProfile(QStringLiteral("original_owner"), QStringLiteral("legacy-password-789"), nullptr, &error), qPrintable(error));
        QCOMPARE(repository.activeUsername(), QStringLiteral("original_owner"));
        QVERIFY(repository.games(&error).size() == 1);
        const QString inspectConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), inspectConnection);
            inspect.setDatabaseName(repository.databasePath());
            QVERIFY(inspect.open());
            QSqlQuery user(inspect);
            QVERIFY(user.prepare(QStringLiteral("SELECT username,password_hash FROM users WHERE id='native-desktop-local'")));
            QVERIFY(user.exec());
            QVERIFY(user.next());
            QCOMPARE(user.value(0).toString(), QStringLiteral("original_owner"));
            QVERIFY(user.value(1).toString().startsWith(QStringLiteral("$2a$10$")));
            QSqlQuery owned(inspect);
            QVERIFY(owned.exec(QStringLiteral("SELECT COUNT(*) FROM games WHERE user_id='native-desktop-local'")));
            QVERIFY(owned.next());
            QCOMPARE(owned.value(0).toInt(), 1);
            inspect.close();
            inspect = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(inspectConnection);
        QVERIFY(PasswordHasher::verify(QStringLiteral("imported-go-password"), goHash));
    }

    void webImportSnapshotsActiveWalAndPreservesFullFixture()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourceDatabasePath = directory.filePath(QStringLiteral("web.sqlite"));
        const QString sourceUploads = directory.filePath(QStringLiteral("web-uploads"));
        QVERIFY(QDir().mkpath(sourceUploads));
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(QColor(QStringLiteral("#d66db4")));
        QByteArray png;
        QBuffer imageBuffer(&png);
        QVERIFY(imageBuffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&imageBuffer, "PNG"));

        const QString sourceConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase source = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), sourceConnection);
            source.setDatabaseName(sourceDatabasePath);
            QVERIFY2(source.open(), qPrintable(source.lastError().text()));
            QString error;
            QVERIFY2(MigrationRunner::apply(source, &error), qPrintable(error));
            QVERIFY2(testExec(source, QStringLiteral("PRAGMA wal_autocheckpoint=0"), {}, &error), qPrintable(error));

            const QString userId = QStringLiteral("web-user-0001");
            const QString gameId = QStringLiteral("web-game-0001");
            const QString sectionId = QStringLiteral("web-section-0001");
            const QString typeId = QStringLiteral("web-type-0001");
            const QString relatedTypeId = QStringLiteral("web-type-0002");
            const QString archivedTypeId = QStringLiteral("web-type-0003");
            const QString numberFieldId = QStringLiteral("web-field-num-0001");
            const QString formulaFieldId = QStringLiteral("web-field-form-01");
            const QString relationFieldId = QStringLiteral("web-field-ref-0001");
            const QString archivedFieldId = QStringLiteral("web-field-arch-01");
            const QString relatedFieldId = QStringLiteral("web-field-num-0002");
            const QString objectId = QStringLiteral("web-object-0001");
            const QString archivedObjectId = QStringLiteral("web-object-0002");
            const QString relatedObjectId = QStringLiteral("web-object-0003");
            const QString assetId = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            const QString assetReference = QStringLiteral("/api/assets/") + assetId;
            const QString createdAt = QStringLiteral("2026-11-07T00:00:00Z");
            const QString passwordHash = QStringLiteral("$2a$10$KjpSF9.nYuJs7YPB//ta2.hSnimkaYDMfMNjX8i.SeEcTk2MncVHS");
            const auto exec = [&](const QString &sql, const QVariantList &bindings = QVariantList{}) {
                return testExec(source, sql, bindings, &error);
            };
            QVERIFY2(exec(QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES(?,?,?,?)"),
                {userId, QStringLiteral("web_owner"), passwordHash, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO games(id,user_id,name,description,icon,personal_notes,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)"),
                {gameId, userId, QStringLiteral("Imported campaign"), QStringLiteral("From Web"), QStringLiteral("🎮"), QStringLiteral("private note"), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO games(id,user_id,name,description,icon,personal_notes,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)"),
                {QStringLiteral("web-game-0002"), userId, QStringLiteral("Second imported game"), QStringLiteral("Another world"), QStringLiteral("✦"), QStringLiteral(""), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO sections(id,game_id,name,icon,sort_order,display_style,display_config,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"),
                {sectionId, gameId, QStringLiteral("Archive"), QStringLiteral("✦"), 0, QStringLiteral("cards"), QStringLiteral("{}"), createdAt, createdAt}), qPrintable(error));
            const QString assetFilename = assetId + QStringLiteral(".png");
            QFile assetFile(QDir(sourceUploads).filePath(assetFilename));
            QVERIFY(assetFile.open(QIODevice::WriteOnly));
            QCOMPARE(assetFile.write(png), static_cast<qint64>(png.size()));
            assetFile.close();
            QVERIFY2(exec(QStringLiteral("INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) VALUES(?,?,?,?,?,?,?)"),
                {assetId, userId, gameId, assetFilename, QStringLiteral("image/png"), png.size(), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("UPDATE games SET icon=?,cover_image=? WHERE id=?"), {assetReference, assetReference, gameId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("UPDATE sections SET icon=? WHERE id=?"), {assetReference, sectionId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_types(id,game_id,name,description,icon,image,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"),
                {typeId, gameId, QStringLiteral("Entry"), QStringLiteral("Canonical type"), assetReference, assetReference, 0, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_types(id,game_id,name,description,icon,image,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"),
                {relatedTypeId, gameId, QStringLiteral("Target"), QStringLiteral(""), QStringLiteral("◈"), QStringLiteral(""), 1, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_types(id,game_id,name,description,icon,image,sort_order,created_at,updated_at,archived) VALUES(?,?,?,?,?,?,?,?,?,1)"),
                {archivedTypeId, gameId, QStringLiteral("Archived type"), QStringLiteral(""), QStringLiteral("◈"), QStringLiteral(""), 2, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
                {numberFieldId, typeId, QStringLiteral("Count"), QStringLiteral("integer"), QStringLiteral("✦"), QStringLiteral(""), QStringLiteral("0"), QStringLiteral("{}"), 0, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
                {formulaFieldId, typeId, QStringLiteral("Doubled"), QStringLiteral("formula"), QStringLiteral("✦"), QStringLiteral(""), QVariant(),
                 QStringLiteral("{\"expression\":\"@{web-field-num-0001} * 2\"}"), 1, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
                {relationFieldId, typeId, QStringLiteral("Target"), QStringLiteral("reference"), QStringLiteral("✦"), QStringLiteral(""), QVariant(),
                 QStringLiteral("{\"allowed_type_id\":\"web-type-0002\"}"), 2, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at,archived) VALUES(?,?,?,?,?,?,?,?,?,?,?,1)"),
                {archivedFieldId, typeId, QStringLiteral("Archived field"), QStringLiteral("text"), QStringLiteral("✦"), QStringLiteral(""), QVariant(), QStringLiteral("{}"), 3, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)"),
                {relatedFieldId, relatedTypeId, QStringLiteral("Power"), QStringLiteral("integer"), QStringLiteral("✦"), QStringLiteral(""), QVariant(), QStringLiteral("{}"), 0, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("UPDATE object_types SET primary_display_field_id=? WHERE id=?"), {numberFieldId, typeId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("UPDATE object_types SET icon=?,image=? WHERE id=?"), {assetReference, assetReference, typeId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO objects(id,game_id,object_type_id,name,description,image,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)"),
                {objectId, gameId, typeId, QStringLiteral("Active record"), QStringLiteral("value survives"), assetReference, createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO objects(id,game_id,object_type_id,name,description,image,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)"),
                {archivedObjectId, gameId, typeId, QStringLiteral("Archived record"), QStringLiteral(""), QStringLiteral(""), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO objects(id,game_id,object_type_id,name,description,image,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)"),
                {relatedObjectId, gameId, relatedTypeId, QStringLiteral("Target record"), QStringLiteral(""), QStringLiteral(""), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?)"),
                {objectId, typeId, numberFieldId, QStringLiteral("21"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?)"),
                {archivedObjectId, typeId, numberFieldId, QStringLiteral("7"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?)"),
                {relatedObjectId, relatedTypeId, relatedFieldId, QStringLiteral("5"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_relationships(game_id,object_id,object_type_id,field_id,referenced_object_id) VALUES(?,?,?,?,?)"),
                {gameId, objectId, typeId, relationFieldId, relatedObjectId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO section_placements(section_id,game_id,object_id,created_at) VALUES(?,?,?,?)"),
                {sectionId, gameId, objectId, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_gallery(id,object_id,asset_id,caption,sort_order,is_primary,created_at) VALUES(?,?,?,?,?,?,?)"),
                {QStringLiteral("gallery-entry-0001"), archivedObjectId, assetId, QStringLiteral("Shared image"), 0, 1, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("UPDATE objects SET archived=1 WHERE id=?"), {archivedObjectId}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO goals(id,game_id,title,goal_type,created_at,updated_at) VALUES(?,?,?,?,?,?)"),
                {QStringLiteral("goal-prereq-0001"), gameId, QStringLiteral("First goal"), QStringLiteral("manual"), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO goals(id,game_id,title,goal_type,created_at,updated_at) VALUES(?,?,?,?,?,?)"),
                {QStringLiteral("goal-dependent-01"), gameId, QStringLiteral("Second goal"), QStringLiteral("checklist"), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO goal_tasks(id,goal_id,title,completed,sort_order,created_at) VALUES(?,?,?,?,?,?)"),
                {QStringLiteral("goal-task-000001"), QStringLiteral("goal-dependent-01"), QStringLiteral("Finish step"), 1, 0, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO goal_dependencies(game_id,goal_id,prerequisite_goal_id,created_at) VALUES(?,?,?,?)"),
                {gameId, QStringLiteral("goal-dependent-01"), QStringLiteral("goal-prereq-0001"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO section_goal_placements(section_id,game_id,goal_id,created_at) VALUES(?,?,?,?)"),
                {sectionId, gameId, QStringLiteral("goal-dependent-01"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) VALUES(?,?,?,?,?,?,?,?)"),
                {QStringLiteral("history-event-0001"), gameId, objectId, QStringLiteral("Active record"), userId, QStringLiteral("created"), QStringLiteral("web"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_history_changes(event_id,field_id,field_name,field_type,before_json,after_json) VALUES(?,?,?,?,?,?)"),
                {QStringLiteral("history-event-0001"), numberFieldId, QStringLiteral("Count"), QStringLiteral("integer"), QVariant(), QStringLiteral("21")}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO field_type_migrations(id,game_id,field_id,field_name,previous_type,new_type,policy_json,converted_values,affected_objects,archived_objects_affected,incompatible_values,outcome,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,'applied',?)"),
                {QStringLiteral("migration-audit-0001"), gameId, numberFieldId, QStringLiteral("Count"), QStringLiteral("text"), QStringLiteral("integer"), QStringLiteral("{}"), 2, 2, 1, 0, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO game_templates(id,user_id,name,description,icon,template_version,data_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"),
                {QStringLiteral("template-entry-0001"), userId, QStringLiteral("Campaign template"), QStringLiteral(""), QStringLiteral("📘"), 1, QStringLiteral("{\"version\":1,\"game\":{}}"), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO object_presets(id,object_id,game_id,name,description,sort_order,values_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"),
                {QStringLiteral("preset-entry-0001"), objectId, gameId, QStringLiteral("Preset"), QStringLiteral(""), 0, QStringLiteral("{\"web-field-num-0001\":12}"), createdAt, createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO game_dashboards(game_id,layout_json,updated_at) VALUES(?,?,?)"),
                {gameId, QStringLiteral("{\"widgets\":[{\"type\":\"gallery\"}]}"), createdAt}), qPrintable(error));
            QVERIFY2(exec(QStringLiteral("INSERT INTO asset_references(entity_type,entity_id,slot,asset_id) VALUES('dashboard',?,'widget-0',?)"),
                {gameId, assetId}), qPrintable(error));

            const QString walPath = sourceDatabasePath + QStringLiteral("-wal");
            QVERIFY2(QFileInfo::exists(walPath), "Fixture must retain committed source rows in an active WAL.");
            const QByteArray mainHashBefore = testFileHash(sourceDatabasePath);
            const QByteArray walHashBefore = testFileHash(walPath);
            QVERIFY(!mainHashBefore.isEmpty());
            QVERIFY(!walHashBefore.isEmpty());

            GameRepository target(directory.filePath(QStringLiteral("desktop.sqlite")));
            QVERIFY2(target.open(&error), qPrintable(error));
            QString existingDesktopGameId;
            QVERIFY2(target.createGame(QStringLiteral("Existing desktop library"), {}, {}, &existingDesktopGameId, &error), qPrintable(error));
            WebImportSummary summary;
            QVERIFY2(target.importWebData(sourceDatabasePath, sourceUploads, &summary, &error), qPrintable(error));
            QCOMPARE(summary.accounts, 1);
            QCOMPARE(summary.games, 2);
            QCOMPARE(summary.objects, 3);
            QCOMPARE(summary.assets, 1);
            QCOMPARE(testFileHash(sourceDatabasePath), mainHashBefore);
            QCOMPARE(testFileHash(walPath), walHashBefore);
            const QVector<Game> existingGames = target.games(&error);
            QCOMPARE(existingGames.size(), 1);
            QCOMPARE(existingGames.first().id, existingDesktopGameId);
            QString authenticatedUserId;
            QVERIFY2(target.authenticate(QStringLiteral("WEB_OWNER"), QStringLiteral("imported-go-password"), &authenticatedUserId, &error), qPrintable(error));
            QCOMPARE(authenticatedUserId, QStringLiteral("web-user-0001"));
            const QVector<Game> importedGames = target.games(&error);
            QCOMPARE(importedGames.size(), 2);
            const auto importedCampaign = std::find_if(importedGames.cbegin(), importedGames.cend(), [&](const Game &item) { return item.id == gameId; });
            QVERIFY(importedCampaign != importedGames.cend());
            QCOMPARE(importedCampaign->personalNotes, QStringLiteral("private note"));
            QCOMPARE(importedCampaign->icon, assetReference);
            QVERIFY(target.loadAssetImage(assetReference).size() == QSize(2, 2));
            const QVector<ObjectType> importedTypes = target.objectTypes(gameId, &error);
            QCOMPARE(importedTypes.size(), 2);
            QCOMPARE(importedTypes.first().primaryDisplayFieldId, numberFieldId);
            const QVector<ObjectRecord> importedObjects = target.objects(gameId, &error);
            QCOMPARE(importedObjects.size(), 2);
            const auto active = std::find_if(importedObjects.cbegin(), importedObjects.cend(), [&](const ObjectRecord &item) { return item.id == objectId; });
            QVERIFY(active != importedObjects.cend());
            QCOMPARE(active->values.value(numberFieldId).toInt(), 21);
            QCOMPARE(active->values.value(formulaFieldId).toInt(), 42);
            QCOMPARE(active->references.value(relationFieldId), QStringList{relatedObjectId});
            const QVector<ArchivedEntry> archived = target.archivedEntries(&error);
            QVERIFY(std::any_of(archived.cbegin(), archived.cend(), [&](const ArchivedEntry &entry) { return entry.id == archivedObjectId; }));
            QVERIFY(std::any_of(archived.cbegin(), archived.cend(), [&](const ArchivedEntry &entry) { return entry.id == archivedTypeId; }));
            QVERIFY(std::any_of(archived.cbegin(), archived.cend(), [&](const ArchivedEntry &entry) { return entry.id == archivedFieldId; }));

            const QString verifyConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase verify = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnection);
                verify.setDatabaseName(target.databasePath());
                QVERIFY(verify.open());
                QSqlQuery importedAccount(verify);
                QVERIFY(importedAccount.exec(QStringLiteral("SELECT id,password_hash FROM users WHERE username='web_owner'")));
                QVERIFY(importedAccount.next());
                QCOMPARE(importedAccount.value(0).toString(), QStringLiteral("web-user-0001"));
                QCOMPARE(importedAccount.value(1).toString(), QStringLiteral("$2a$10$KjpSF9.nYuJs7YPB//ta2.hSnimkaYDMfMNjX8i.SeEcTk2MncVHS"));
                const QStringList tables{QStringLiteral("goal_dependencies"), QStringLiteral("section_goal_placements"),
                    QStringLiteral("object_history_events"), QStringLiteral("object_history_changes"), QStringLiteral("field_type_migrations"),
                    QStringLiteral("game_templates"), QStringLiteral("object_presets"), QStringLiteral("game_dashboards"), QStringLiteral("object_gallery")};
                for (const QString &table : tables) {
                    QSqlQuery count(verify);
                    QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM \"%1\"").arg(table)));
                    QVERIFY(count.next());
                    QVERIFY2(count.value(0).toInt() > 0, qPrintable(table));
                }
                QSqlQuery foreignKeys(verify);
                QVERIFY(foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check")));
                QVERIFY(!foreignKeys.next());
                verify.close();
                verify = QSqlDatabase();
            }
            QSqlDatabase::removeDatabase(verifyConnection);

            const int gamesBeforeRepeat = target.games(&error).size();
            QVERIFY(!target.importWebData(sourceDatabasePath, sourceUploads, nullptr, &error));
            QVERIFY(error.contains(QStringLiteral("conflicts"), Qt::CaseInsensitive));
            QCOMPARE(target.games(&error).size(), gamesBeforeRepeat);
            QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("uploads/") + assetFilename)));
        }
        QSqlDatabase::removeDatabase(sourceConnection);
    }

    void webImportMissingAssetRollsBackWithoutAddingAccounts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourceUploads = directory.filePath(QStringLiteral("missing-uploads"));
        QVERIFY(QDir().mkpath(sourceUploads));
        const QString sourcePath = directory.filePath(QStringLiteral("missing.sqlite"));
        const QString sourceConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase source = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), sourceConnection);
            source.setDatabaseName(sourcePath);
            QVERIFY(source.open());
            QString error;
            QVERIFY2(MigrationRunner::apply(source, &error), qPrintable(error));
            QVERIFY2(testExec(source, QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES(?,?,?,?)"),
                {QStringLiteral("missing-user-0001"), QStringLiteral("missing_user"),
                 QStringLiteral("$2a$10$KjpSF9.nYuJs7YPB//ta2.hSnimkaYDMfMNjX8i.SeEcTk2MncVHS"), QStringLiteral("2026-11-07T00:00:00Z")}, &error), qPrintable(error));
            QVERIFY2(testExec(source, QStringLiteral("INSERT INTO uploaded_assets(id,user_id,filename,mime_type,size_bytes,created_at) VALUES(?,?,?,?,?,?)"),
                {QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"), QStringLiteral("missing-user-0001"),
                 QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb.png"), QStringLiteral("image/png"), 32, QStringLiteral("2026-11-07T00:00:00Z")}, &error), qPrintable(error));
            GameRepository target(directory.filePath(QStringLiteral("target.sqlite")));
            QVERIFY2(target.open(&error), qPrintable(error));
            QVERIFY(!target.importWebData(sourcePath, sourceUploads, nullptr, &error));
            QVERIFY(error.contains(QStringLiteral("missing"), Qt::CaseInsensitive));
            QSqlQuery users(source);
            Q_UNUSED(users);
            const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
                inspect.setDatabaseName(target.databasePath());
                QVERIFY(inspect.open());
                QSqlQuery count(inspect);
                QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM users")));
                QVERIFY(count.next());
                QCOMPARE(count.value(0).toInt(), 1);
                QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM games")));
                QVERIFY(count.next());
                QCOMPARE(count.value(0).toInt(), 0);
                inspect.close();
                inspect = QSqlDatabase();
            }
            QSqlDatabase::removeDatabase(connection);
            source.close();
            source = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(sourceConnection);
    }

    void webImportRejectsUnsupportedSchemaAndAccountCollisions()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourceUploads = directory.filePath(QStringLiteral("collision-uploads"));
        QVERIFY(QDir().mkpath(sourceUploads));
        const QString sourcePath = directory.filePath(QStringLiteral("unsupported.sqlite"));
        const QString blankConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase blank = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), blankConnection);
            blank.setDatabaseName(sourcePath);
            QVERIFY(blank.open());
            blank.close();
            blank = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(blankConnection);

        GameRepository target(directory.filePath(QStringLiteral("target.sqlite")));
        QString error;
        QVERIFY2(target.open(&error), qPrintable(error));
        QVERIFY2(target.registerAccount(QStringLiteral("desktop_owner"), QStringLiteral("desktop-password-123"), nullptr, &error), qPrintable(error));
        QString preservedGameId;
        QVERIFY2(target.createGame(QStringLiteral("Keep this game"), {}, {}, &preservedGameId, &error), qPrintable(error));
        QVERIFY(!target.importWebData(sourcePath, sourceUploads, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("supported"), Qt::CaseInsensitive));
        QCOMPARE(target.games(&error).size(), 1);
        QCOMPARE(target.games(&error).first().id, preservedGameId);

        const QString validSourcePath = directory.filePath(QStringLiteral("collision.sqlite"));
        const QString sourceConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase source = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), sourceConnection);
            source.setDatabaseName(validSourcePath);
            QVERIFY(source.open());
            QVERIFY2(MigrationRunner::apply(source, &error), qPrintable(error));
            QVERIFY2(testExec(source, QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES(?,?,?,?)"),
                {QStringLiteral("web-collision-user"), QStringLiteral("DESKTOP_OWNER"),
                 QStringLiteral("$2a$10$KjpSF9.nYuJs7YPB//ta2.hSnimkaYDMfMNjX8i.SeEcTk2MncVHS"), QStringLiteral("2026-11-07T00:00:00Z")}, &error), qPrintable(error));
            QVERIFY(!target.importWebData(validSourcePath, sourceUploads, nullptr, &error));
            QVERIFY(error.contains(QStringLiteral("conflicts"), Qt::CaseInsensitive));
            QCOMPARE(target.games(&error).size(), 1);
            QCOMPARE(target.games(&error).first().id, preservedGameId);
            source.close();
            source = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(sourceConnection);
    }

    void placementsValidateSectionAndKeepOneObject()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("placements.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Collection"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Item"), {}, {}, &typeId, &error));
        QVERIFY(repository.createSection(gameId, {}, QStringLiteral("A"), {}, &error));
        QVERIFY(repository.createSection(gameId, {}, QStringLiteral("B"), {}, &error));
        const auto sections = repository.sections(gameId, &error);
        QCOMPARE(sections.size(), 2);
        const QString first = sections.at(0).id;
        const QString second = sections.at(1).id;
        ObjectRecord object;
        object.name = QStringLiteral("Shared item");
        object.gameId = gameId;
        object.typeId = typeId;
        QVERIFY(!repository.saveObject(object, {}, QStringLiteral("missing section"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(object.id.isEmpty());
        QVERIFY(repository.objects(gameId).isEmpty());
        QVERIFY2(repository.saveObject(object, {}, first, &error), qPrintable(error));
        QVERIFY2(repository.setObjectPlacement(object.id, second, true, &error), qPrintable(error));
        QVERIFY2(repository.setObjectPlacement(object.id, second, true, &error), qPrintable(error));
        auto objects = repository.objects(gameId, &error);
        QCOMPARE(objects.size(), 1);
        QCOMPARE(objects.first().sectionIds.size(), 2);
        QVERIFY(repository.archiveSection(first, &error));
        QVERIFY(!repository.createSection(gameId, first, QStringLiteral("Hidden child"), {}, &error));
        QVERIFY(!repository.setObjectPlacement(object.id, first, true, &error));
        QVERIFY2(repository.setObjectPlacement(object.id, second, false, &error), qPrintable(error));
        objects = repository.objects(gameId, &error);
        QCOMPARE(objects.size(), 1);
        QVERIFY(objects.first().sectionIds.isEmpty());
        QVERIFY(repository.restoreArchivedEntry({first, QStringLiteral("Section"), {}, {}}, &error));
        QCOMPARE(repository.objects(gameId).first().sectionIds, QStringList{first});
    }

    void historyPagesKeepStableFieldsAndExcludeUnchangedSaves()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        ObjectRecord object;
        object.name = QStringLiteral("Tracked item");
        QVERIFY(repository.createGame(QStringLiteral("Collection"), {}, {}, &object.gameId, &error));
        QVERIFY(repository.createObjectType(object.gameId, QStringLiteral("Item"), {}, {}, &object.typeId, &error));
        QString fieldId;
        QVERIFY(repository.createField(object.typeId, QStringLiteral("Level"), QStringLiteral("integer"), {}, {}, &fieldId, &error));
        const auto types = repository.objectTypes(object.gameId, &error);
        QCOMPARE(types.size(), 1);
        object.values[fieldId] = 0;
        QVERIFY2(repository.saveObject(object, types.first().fields, {}, &error), qPrintable(error));
        QVERIFY2(repository.saveObject(object, types.first().fields, {}, &error), qPrintable(error));
        QCOMPARE(repository.objectHistory(object.id, {}, &error).events.size(), 1);
        const QDate since = QDateTime::currentDateTimeUtc().date();
        for (int level = 1; level <= 21; ++level) {
            object.values[fieldId] = level;
            QVERIFY2(repository.saveObject(object, types.first().fields, {}, &error), qPrintable(error));
        }
        QCOMPARE(repository.historyFields(object.id, &error).value(fieldId), QStringLiteral("Level"));
        HistoryFilter filter;
        filter.fieldId = fieldId;
        filter.eventType = QStringLiteral("updated");
        filter.since = since;
        filter.until = QDateTime::currentDateTimeUtc().date();
        const HistoryPage first = repository.objectHistory(object.id, filter, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(first.events.size(), 20);
        QVERIFY(first.hasMore);
        QCOMPARE(first.events.first().changes.first().before.toInt(), 20);
        QCOMPARE(first.events.first().changes.first().after.toInt(), 21);
        filter.page = 1;
        const HistoryPage second = repository.objectHistory(object.id, filter, &error);
        QCOMPARE(second.events.size(), 1);
        QVERIFY(!second.hasMore);
        QCOMPARE(second.events.first().changes.first().fieldId, fieldId);
        HistoryFilter chartFilter;
        chartFilter.fieldId = fieldId;
        chartFilter.limit = 100;
        const HistoryPage chart = repository.objectHistory(object.id, chartFilter, &error);
        QCOMPARE(chart.events.size(), 22);
        QVERIFY(!chart.hasMore);
        QCOMPARE(chart.events.first().changes.first().after.toDouble(), 21.0);
        QCOMPARE(chart.events.last().changes.first().after.toDouble(), 0.0);
        QVERIFY(repository.archiveObject(object.id, &error));
        QVERIFY(repository.restoreArchivedEntry({object.id, QStringLiteral("Object"), {}, {}}, &error));
        filter = {};
        filter.eventType = QStringLiteral("archived");
        QCOMPARE(repository.objectHistory(object.id, filter, &error).events.size(), 1);
        filter.eventType = QStringLiteral("restored");
        QCOMPARE(repository.objectHistory(object.id, filter, &error).events.size(), 1);
    }

    void objectTypeFieldValueAndReferenceCrudIsTransactional()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("objects.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));

        QString gameId;
        QVERIFY2(repository.createGame(QStringLiteral("Collection"), {}, {}, &gameId, &error), qPrintable(error));
        QString characterTypeId;
        QVERIFY2(repository.createObjectType(gameId, QStringLiteral("Character"), {}, {}, &characterTypeId, &error), qPrintable(error));
        QString weaponTypeId;
        QVERIFY2(repository.createObjectType(gameId, QStringLiteral("Weapon"), {}, {}, &weaponTypeId, &error), qPrintable(error));
        QVERIFY(!repository.createField(characterTypeId, QStringLiteral("Empty select"), QStringLiteral("select"), {}, {}, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("choices"), Qt::CaseInsensitive));

        QString levelFieldId;
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Level"), QStringLiteral("integer"), {}, {}, &levelFieldId, &error), qPrintable(error));
        QString rarityFieldId;
        const QJsonObject rarityOptions{{QStringLiteral("choices"), QJsonArray{QStringLiteral("Common"), QStringLiteral("Rare")}}};
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Rarity"), QStringLiteral("select"), {}, rarityOptions, &rarityFieldId, &error), qPrintable(error));
        QString relatedFieldId;
        const QJsonObject relationOptions{{QStringLiteral("allowed_type_id"), weaponTypeId}};
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Weapon"), QStringLiteral("reference"), {}, relationOptions, &relatedFieldId, &error), qPrintable(error));
        QString manyFieldId;
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Collection"), QStringLiteral("references"), {}, relationOptions, &manyFieldId, &error), qPrintable(error));
        QString progressFieldId;
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Ascension"), QStringLiteral("progress"), {}, {}, &progressFieldId, &error), qPrintable(error));

        ObjectRecord weapon;
        weapon.gameId = gameId;
        weapon.typeId = weaponTypeId;
        weapon.name = QStringLiteral("Skyward Blade");
        QVERIFY2(repository.saveObject(weapon, {}, {}, &error), qPrintable(error));

        QVector<ObjectType> types = repository.objectTypes(gameId, &error);
        QCOMPARE(types.size(), 2);
        const auto characterType = std::find_if(types.cbegin(), types.cend(), [&](const ObjectType &type) { return type.id == characterTypeId; });
        QVERIFY(characterType != types.cend());
        QCOMPARE(characterType->fields.size(), 5);

        ObjectRecord character;
        character.gameId = gameId;
        character.typeId = characterTypeId;
        character.name = QStringLiteral("Traveler");
        character.values.insert(levelFieldId, 80);
        character.values.insert(rarityFieldId, QStringLiteral("Rare"));
        character.values.insert(progressFieldId, QJsonObject{{QStringLiteral("current"), 4}, {QStringLiteral("max"), 6}});
        character.references.insert(relatedFieldId, QStringList{weapon.id});
        character.references.insert(manyFieldId, QStringList{weapon.id});
        QVERIFY2(repository.saveObject(character, characterType->fields, {}, &error), qPrintable(error));

        QVector<ObjectRecord> objects = repository.objects(gameId, &error);
        QCOMPARE(objects.size(), 2);
        const auto saved = std::find_if(objects.cbegin(), objects.cend(), [&](const ObjectRecord &object) { return object.id == character.id; });
        QVERIFY(saved != objects.cend());
        QCOMPARE(saved->values.value(levelFieldId).toInt(), 80);
        QCOMPARE(saved->values.value(rarityFieldId).toString(), QStringLiteral("Rare"));
        QCOMPARE(saved->values.value(progressFieldId).toObject().value(QStringLiteral("max")).toInt(), 6);
        QCOMPARE(saved->references.value(relatedFieldId), QStringList{weapon.id});
        QCOMPARE(saved->references.value(manyFieldId), QStringList{weapon.id});

        ObjectRecord updated = *saved;
        updated.name = QStringLiteral("Traveler II");
        updated.values[levelFieldId] = 90;
        QVERIFY2(repository.saveObject(updated, characterType->fields, {}, &error), qPrintable(error));
        const auto typesAfterUpdate = repository.objectTypes(gameId, &error);
        const auto characterTypeAfterUpdate = std::find_if(typesAfterUpdate.cbegin(), typesAfterUpdate.cend(), [&](const ObjectType &type) { return type.id == characterTypeId; });
        QVERIFY(characterTypeAfterUpdate != typesAfterUpdate.cend());

        QString otherGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Other Collection"), {}, {}, &otherGameId, &error), qPrintable(error));
        QString otherTypeId;
        QVERIFY2(repository.createObjectType(otherGameId, QStringLiteral("Other"), {}, {}, &otherTypeId, &error), qPrintable(error));
        const QJsonObject foreignTypeOptions{{QStringLiteral("allowed_type_id"), otherTypeId}};
        QVERIFY(!repository.createField(characterTypeId, QStringLiteral("Foreign type"), QStringLiteral("reference"), {}, foreignTypeOptions, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("same game"), Qt::CaseInsensitive));
        ObjectRecord foreign;
        foreign.gameId = otherGameId;
        foreign.typeId = otherTypeId;
        foreign.name = QStringLiteral("Foreign object");
        QVERIFY2(repository.saveObject(foreign, {}, {}, &error), qPrintable(error));

        ObjectRecord invalid = updated;
        invalid.name = QStringLiteral("Must roll back");
        invalid.references[relatedFieldId] = QStringList{foreign.id};
        QVERIFY(!repository.saveObject(invalid, characterTypeAfterUpdate->fields, {}, &error));
        QCOMPARE(repository.objects(gameId, &error).size(), 2);
        const auto afterRollback = repository.objects(gameId, &error);
        const auto current = std::find_if(afterRollback.cbegin(), afterRollback.cend(), [&](const ObjectRecord &object) { return object.id == character.id; });
        QVERIFY(current != afterRollback.cend());
        QCOMPARE(current->name, QStringLiteral("Traveler II"));
        QCOMPARE(current->values.value(levelFieldId).toInt(), 90);

        ObjectRecord invalidProgress = updated;
        invalidProgress.values[progressFieldId] = QJsonObject{{QStringLiteral("current"), 4}, {QStringLiteral("max"), 0}};
        QVERIFY(!repository.saveObject(invalidProgress, characterTypeAfterUpdate->fields, {}, &error));
        QCOMPARE(repository.objects(gameId, &error).size(), 2);

        QVERIFY2(repository.archiveObject(weapon.id, &error), qPrintable(error));
        ObjectRecord keepArchivedLink = *current;
        QVERIFY2(repository.saveObject(keepArchivedLink, characterTypeAfterUpdate->fields, {}, &error), qPrintable(error));
        const auto afterArchive = repository.objects(gameId, &error);
        QCOMPARE(afterArchive.size(), 1);
        QCOMPARE(afterArchive.first().references.value(relatedFieldId), QStringList{weapon.id});
    }

    void objectTypesAndFieldsCanBeEditedArchivedAndRestored()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("schema-edit.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));

        QString gameId;
        QString typeId;
        QString fieldId;
        QVERIFY(repository.createGame(QStringLiteral("Schema"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Entry"), QStringLiteral("Before"), QStringLiteral("◈"), &typeId, &error));
        QJsonObject selectOptions{{QStringLiteral("choices"), QJsonArray{QStringLiteral("Common"), QStringLiteral("Rare")}}};
        QVERIFY(repository.createField(typeId, QStringLiteral("Rarity"), QStringLiteral("select"), QStringLiteral("✦"),
            selectOptions, &fieldId, &error, QStringLiteral("Original help"), QStringLiteral("Common")));

        const QVector<ObjectType> beforeTypes = repository.objectTypes(gameId, &error);
        QCOMPARE(beforeTypes.size(), 1);
        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Sword");
        object.values.insert(fieldId, QStringLiteral("Rare"));
        QVERIFY2(repository.saveObject(object, beforeTypes.first().fields, {}, &error), qPrintable(error));

        ObjectType editedType = beforeTypes.first();
        editedType.name = QStringLiteral("Equipment");
        editedType.description = QStringLiteral("Updated description");
        editedType.icon = QStringLiteral("⚔");
        editedType.primaryDisplayFieldId = fieldId;
        QVERIFY2(repository.updateObjectType(editedType, &error), qPrintable(error));
        QVERIFY2(repository.setPrimaryDisplayField(typeId, fieldId, &error), qPrintable(error));
        QCOMPARE(repository.objectTypes(gameId, &error).first().primaryDisplayFieldId, fieldId);

        QVector<ObjectType> currentTypes = repository.objectTypes(gameId, &error);
        FieldDefinition editedField = currentTypes.first().fields.first();
        QCOMPARE(editedField.id, fieldId);
        editedField.name = QStringLiteral("Tier");
        editedField.description = QStringLiteral("Updated help");
        editedField.sortOrder = 4;
        editedField.defaultValue = QStringLiteral("Common");
        editedField.options = QJsonObject{{QStringLiteral("choices"), QJsonArray{QStringLiteral("Basic"), QStringLiteral("Epic")}}};
        QVERIFY2(repository.updateField(editedField, {{QStringLiteral("Common"), QStringLiteral("Basic")},
            {QStringLiteral("Rare"), QStringLiteral("Epic")}}, &error), qPrintable(error));
        QCOMPARE(repository.objects(gameId, &error).first().values.value(fieldId).toString(), QStringLiteral("Epic"));
        currentTypes = repository.objectTypes(gameId, &error);
        QCOMPARE(currentTypes.first().fields.first().id, fieldId);
        QCOMPARE(currentTypes.first().fields.first().name, QStringLiteral("Tier"));
        QCOMPARE(currentTypes.first().fields.first().defaultValue.toString(), QStringLiteral("Basic"));

        QVERIFY(!repository.setFieldArchived(fieldId, true, &error));
        QVERIFY2(error.contains(QStringLiteral("primary display"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(repository.setPrimaryDisplayField(typeId, {}, &error), qPrintable(error));
        QVERIFY2(repository.setFieldArchived(fieldId, true, &error), qPrintable(error));
        const QVector<ObjectType> archivedFieldTypes = repository.objectTypes(gameId, &error);
        QVERIFY(archivedFieldTypes.first().fields.first().archived);
        const QVector<ArchivedEntry> fieldEntries = repository.archivedEntries(&error);
        const auto archivedField = std::find_if(fieldEntries.cbegin(), fieldEntries.cend(), [&](const ArchivedEntry &entry) {
            return entry.kind == QStringLiteral("Field") && entry.id == fieldId;
        });
        QVERIFY(archivedField != fieldEntries.cend());
        QVERIFY2(repository.restoreArchivedEntry(*archivedField, &error), qPrintable(error));
        QVERIFY(!repository.objectTypes(gameId, &error).first().fields.first().archived);

        QVERIFY2(repository.setObjectTypeArchived(typeId, true, &error), qPrintable(error));
        QVERIFY(repository.objectTypes(gameId, &error).isEmpty());
        const QVector<ArchivedEntry> typeEntries = repository.archivedEntries(&error);
        const auto archivedType = std::find_if(typeEntries.cbegin(), typeEntries.cend(), [&](const ArchivedEntry &entry) {
            return entry.kind == QStringLiteral("Object Type") && entry.id == typeId;
        });
        QVERIFY(archivedType != typeEntries.cend());
        QVERIFY2(repository.restoreArchivedEntry(*archivedType, &error), qPrintable(error));
        QCOMPARE(repository.objectTypes(gameId, &error).first().id, typeId);
        QCOMPARE(repository.objects(gameId, &error).first().id, object.id);
        QCOMPARE(repository.objectTypes(gameId, &error).first().name, QStringLiteral("Equipment"));
    }

    void fieldMigrationConvertsSupportedValuesAndArchivedObjects()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("field-migration-matrix.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Migration matrix"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Record"), {}, {}, &typeId, &error));

        auto makeField = [&](const QString &name, const QString &fieldType, const QJsonObject &options = QJsonObject{},
                             const QJsonValue &defaultValue = QJsonValue()) {
            QString id;
            if (!repository.createField(typeId, name, fieldType, {}, options, &id, &error, {}, defaultValue)) return QString();
            return id;
        };
        auto makeObject = [&](const QString &name, const QString &fieldId, const QJsonValue &value, bool archived = false) {
            ObjectRecord object;
            object.gameId = gameId;
            object.typeId = typeId;
            object.name = name;
            object.values.insert(fieldId, value);
            if (!repository.saveObject(object, repository.objectTypes(gameId, &error).first().fields, {}, &error)) return QString();
            if (archived && !repository.archiveObject(object.id, &error)) return QString();
            return object.id;
        };
        auto migrate = [&](const QString &fieldId, const QString &targetType, const QJsonObject &options = QJsonObject{},
                           const QJsonObject &policy = QJsonObject{}) {
            FieldMigrationRequest request;
            request.targetType = targetType;
            request.targetOptions = options;
            request.policy = policy;
            const FieldMigrationPreview preview = repository.previewFieldTypeMigration(fieldId, request, &error);
            if (!error.isEmpty() || !preview.canApply) return false;
            FieldMigrationPreview result;
            return repository.applyFieldTypeMigration(fieldId, request, preview.previewToken, &result, &error);
        };
        auto activeValue = [&](const QString &objectId, const QString &fieldId) {
            const auto objects = repository.objects(gameId, &error);
            const auto found = std::find_if(objects.cbegin(), objects.cend(), [&](const ObjectRecord &item) { return item.id == objectId; });
            return found == objects.cend() ? QJsonValue() : found->values.value(fieldId);
        };

        QString fieldId = makeField(QStringLiteral("Numeric text"), QStringLiteral("text"));
        QString objectId = makeObject(QStringLiteral("Integer source"), fieldId, QStringLiteral("42"));
        FieldMigrationPreview preview;
        QVERIFY2(!fieldId.isEmpty() && !objectId.isEmpty(), qPrintable(error));
        QVERIFY2(migrate(fieldId, QStringLiteral("integer")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toDouble(), 42.0);

        QString preciseTextField = makeField(QStringLiteral("Precise number text"), QStringLiteral("text"));
        const QString preciseTextObject = makeObject(QStringLiteral("Precise text"), preciseTextField, QStringLiteral("1.0000000000000001"));
        FieldMigrationRequest exactInteger;
        exactInteger.targetType = QStringLiteral("integer");
        preview = repository.previewFieldTypeMigration(preciseTextField, exactInteger, &error);
        QVERIFY2(!preview.canApply && preview.incompatibleValues == 1, qPrintable(QStringLiteral("canApply=%1 incompatible=%2 saved=%3 examples=%4")
            .arg(preview.canApply).arg(preview.incompatibleValues).arg(preview.savedValues)
            .arg(preview.examples.isEmpty() ? QString() : preview.examples.first().reason)));
        QVERIFY(preview.examples.first().reason.contains(QStringLiteral("number"), Qt::CaseInsensitive));

        QString largeIntegerField = makeField(QStringLiteral("Imported integer"), QStringLiteral("integer"));
        const QString largeIntegerObject = makeObject(QStringLiteral("Imported precision"), largeIntegerField, 1);
        {
            const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QVERIFY2(testExec(database, QStringLiteral("UPDATE field_values SET value_json='9007199254740993' WHERE object_id=? AND field_id=?"),
                {largeIntegerObject, largeIntegerField}, &error), qPrintable(error));
            QVERIFY2(testExec(database, QStringLiteral("UPDATE field_definitions SET default_json='9007199254740993' WHERE id=?"),
                {largeIntegerField}, &error), qPrintable(error));
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
        }
        FieldMigrationRequest exactDecimal;
        exactDecimal.targetType = QStringLiteral("decimal");
        preview = repository.previewFieldTypeMigration(largeIntegerField, exactDecimal, &error);
        QVERIFY2(preview.canApply, qPrintable(QStringLiteral("canApply=%1 incompatible=%2 saved=%3 examples=%4 blockers=%5")
            .arg(preview.canApply).arg(preview.incompatibleValues).arg(preview.savedValues)
            .arg(preview.examples.isEmpty() ? QString() : preview.examples.first().reason)
            .arg(preview.blockers.join(QLatin1Char('\n')))));
        QVERIFY2(repository.applyFieldTypeMigration(largeIntegerField, exactDecimal, preview.previewToken, nullptr, &error), qPrintable(error));
        {
            const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QSqlQuery exactValue(database);
            exactValue.prepare(QStringLiteral("SELECT value_json FROM field_values WHERE object_id=? AND field_id=?"));
            exactValue.addBindValue(largeIntegerObject);
            exactValue.addBindValue(largeIntegerField);
            QVERIFY(exactValue.exec() && exactValue.next());
            QCOMPARE(exactValue.value(0).toString(), QStringLiteral("9007199254740993"));
            QSqlQuery exactDefault(database);
            exactDefault.prepare(QStringLiteral("SELECT default_json FROM field_definitions WHERE id=?"));
            exactDefault.addBindValue(largeIntegerField);
            QVERIFY(exactDefault.exec() && exactDefault.next());
            QCOMPARE(exactDefault.value(0).toString(), QStringLiteral("9007199254740993"));
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
        }
        Q_UNUSED(preciseTextObject);

        fieldId = makeField(QStringLiteral("Integer"), QStringLiteral("integer"));
        objectId = makeObject(QStringLiteral("Integer value"), fieldId, 17);
        QVERIFY2(migrate(fieldId, QStringLiteral("decimal")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toDouble(), 17.0);

        fieldId = makeField(QStringLiteral("Decimal"), QStringLiteral("decimal"));
        objectId = makeObject(QStringLiteral("Out of range"), fieldId, 125);
        FieldMigrationRequest percentage;
        percentage.targetType = QStringLiteral("percentage");
        preview = repository.previewFieldTypeMigration(fieldId, percentage, &error);
        QVERIFY(preview.incompatibleValues == 1 && !preview.canApply);
        percentage.policy.insert(QStringLiteral("percentage_strategy"), QStringLiteral("clamp"));
        preview = repository.previewFieldTypeMigration(fieldId, percentage, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(fieldId, percentage, preview.previewToken, nullptr, &error), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toDouble(), 100.0);

        fieldId = makeField(QStringLiteral("Boolean"), QStringLiteral("boolean"));
        objectId = makeObject(QStringLiteral("Boolean text"), fieldId, true);
        QVERIFY2(migrate(fieldId, QStringLiteral("text")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("true"));

        fieldId = makeField(QStringLiteral("Boolean source"), QStringLiteral("text"));
        objectId = makeObject(QStringLiteral("Boolean value"), fieldId, QStringLiteral(" TRUE "));
        QVERIFY2(migrate(fieldId, QStringLiteral("boolean")), qPrintable(error));
        QVERIFY(activeValue(objectId, fieldId).toBool());

        fieldId = makeField(QStringLiteral("Choice source"), QStringLiteral("text"));
        objectId = makeObject(QStringLiteral("Choice value"), fieldId, QStringLiteral("bronze"));
        FieldMigrationRequest selectRequest;
        selectRequest.targetType = QStringLiteral("select");
        selectRequest.targetOptions = QJsonObject{{QStringLiteral("choices"), QJsonArray{QStringLiteral("Bronze"), QStringLiteral("Silver")}}};
        preview = repository.previewFieldTypeMigration(fieldId, selectRequest, &error);
        QVERIFY(!preview.canApply && preview.incompatibleValues == 1);
        selectRequest.policy.insert(QStringLiteral("select_replacements"), QJsonObject{{QStringLiteral("bronze"), QStringLiteral("Bronze")}});
        preview = repository.previewFieldTypeMigration(fieldId, selectRequest, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(fieldId, selectRequest, preview.previewToken, nullptr, &error), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("Bronze"));

        fieldId = makeField(QStringLiteral("Select source"), QStringLiteral("select"),
            QJsonObject{{QStringLiteral("choices"), QJsonArray{QStringLiteral("On"), QStringLiteral("Off")}}});
        objectId = makeObject(QStringLiteral("Select text"), fieldId, QStringLiteral("On"));
        QVERIFY2(migrate(fieldId, QStringLiteral("long_text")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("On"));

        fieldId = makeField(QStringLiteral("Date text"), QStringLiteral("text"));
        objectId = makeObject(QStringLiteral("Date value"), fieldId, QStringLiteral("2026-11-07"));
        QVERIFY2(migrate(fieldId, QStringLiteral("date")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("2026-11-07"));
        QVERIFY2(migrate(fieldId, QStringLiteral("text")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("2026-11-07"));

        fieldId = makeField(QStringLiteral("Progress target"), QStringLiteral("decimal"));
        objectId = makeObject(QStringLiteral("Progress source"), fieldId, 12.5, true);
        FieldMigrationRequest toProgress;
        toProgress.targetType = QStringLiteral("progress");
        toProgress.policy.insert(QStringLiteral("maximum"), 50);
        preview = repository.previewFieldTypeMigration(fieldId, toProgress, &error);
        QVERIFY2(preview.canApply && preview.archivedObjectsAffected == 1, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(fieldId, toProgress, preview.previewToken, nullptr, &error), qPrintable(error));
        QSqlDatabase inspect = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("migration-archived-check"));
        inspect.setDatabaseName(repository.databasePath());
        QVERIFY(inspect.open());
        QSqlQuery archivedValue(inspect);
        archivedValue.prepare(QStringLiteral("SELECT value_json FROM field_values WHERE object_id=? AND field_id=?"));
        archivedValue.addBindValue(objectId);
        archivedValue.addBindValue(fieldId);
        QVERIFY(archivedValue.exec() && archivedValue.next());
        QCOMPARE(QJsonDocument::fromJson(archivedValue.value(0).toString().toUtf8()).object().value(QStringLiteral("current")).toDouble(), 12.5);
        inspect.close();
        inspect = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("migration-archived-check"));
        FieldMigrationRequest fromProgress;
        fromProgress.targetType = QStringLiteral("percentage");
        fromProgress.policy.insert(QStringLiteral("progress_value"), QStringLiteral("percentage"));
        preview = repository.previewFieldTypeMigration(fieldId, fromProgress, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(fieldId, fromProgress, preview.previewToken, nullptr, &error), qPrintable(error));

        for (const QString &strategy : {QStringLiteral("current"), QStringLiteral("maximum")}) {
            const QString progressField = makeField(QStringLiteral("Progress %1").arg(strategy), QStringLiteral("progress"));
            const QJsonObject rawProgress{{QStringLiteral("current"), 7}, {QStringLiteral("max"), 10}};
            const QString progressObject = makeObject(QStringLiteral("Progress %1 object").arg(strategy), progressField, rawProgress);
            FieldMigrationRequest numericRequest;
            numericRequest.targetType = QStringLiteral("integer");
            numericRequest.policy.insert(QStringLiteral("progress_value"), strategy);
            preview = repository.previewFieldTypeMigration(progressField, numericRequest, &error);
            QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
            QVERIFY2(repository.applyFieldTypeMigration(progressField, numericRequest, preview.previewToken, nullptr, &error), qPrintable(error));
            QCOMPARE(activeValue(progressObject, progressField).toDouble(), strategy == QStringLiteral("current") ? 7.0 : 10.0);
        }

        fieldId = makeField(QStringLiteral("Percentage source"), QStringLiteral("percentage"));
        objectId = makeObject(QStringLiteral("Percentage decimal"), fieldId, 75);
        QVERIFY2(migrate(fieldId, QStringLiteral("decimal")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toDouble(), 75.0);

        fieldId = makeField(QStringLiteral("Short text"), QStringLiteral("text"));
        objectId = makeObject(QStringLiteral("Text length"), fieldId, QStringLiteral("kept exactly"));
        QVERIFY2(migrate(fieldId, QStringLiteral("long_text")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("kept exactly"));
        QVERIFY2(migrate(fieldId, QStringLiteral("text")), qPrintable(error));
        QCOMPARE(activeValue(objectId, fieldId).toString(), QStringLiteral("kept exactly"));

        fieldId = makeField(QStringLiteral("Formula input"), QStringLiteral("integer"), {}, 8);
        const QString inputObject = makeObject(QStringLiteral("Formula input record"), fieldId, 8);
        QString formulaId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Computed"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{%1} * 3").arg(fieldId)}}, &formulaId, &error));
        QVERIFY2(repository.archiveObject(inputObject, &error), qPrintable(error));
        FieldMigrationRequest freezeFormula;
        freezeFormula.targetType = QStringLiteral("integer");
        preview = repository.previewFieldTypeMigration(formulaId, freezeFormula, &error);
        QVERIFY(!preview.canApply);
        freezeFormula.policy.insert(QStringLiteral("freeze_formula"), true);
        preview = repository.previewFieldTypeMigration(formulaId, freezeFormula, &error);
        QVERIFY2(preview.canApply, qPrintable(QStringLiteral("%1; saved=%2 incompatible=%3 examples=%4")
            .arg(preview.blockers.join(QLatin1Char('\n'))).arg(preview.savedValues).arg(preview.incompatibleValues)
            .arg(preview.examples.isEmpty() ? QString() : preview.examples.first().reason)));
        QVERIFY2(repository.applyFieldTypeMigration(formulaId, freezeFormula, preview.previewToken, &preview, &error), qPrintable(error));
        QCOMPARE(preview.savedValues, 16);
        QCOMPARE(preview.archivedObjectsAffected, 2);
        const auto frozenType = repository.objectTypes(gameId, &error, true).first();
        const auto frozenField = std::find_if(frozenType.fields.cbegin(), frozenType.fields.cend(), [&](const FieldDefinition &item) { return item.id == formulaId; });
        QVERIFY(frozenField != frozenType.fields.cend());
        QCOMPARE(frozenField->fieldType, QStringLiteral("integer"));
        const QString frozenConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSqlDatabase frozenDatabase = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), frozenConnection);
        frozenDatabase.setDatabaseName(repository.databasePath());
        QVERIFY(frozenDatabase.open());
        QSqlQuery frozenValue(frozenDatabase);
        frozenValue.prepare(QStringLiteral("SELECT value_json FROM field_values WHERE object_id=? AND field_id=?"));
        frozenValue.addBindValue(inputObject);
        frozenValue.addBindValue(formulaId);
        QVERIFY(frozenValue.exec() && frozenValue.next());
        QCOMPARE(frozenValue.value(0).toDouble(), 24.0);
        frozenDatabase.close();
        frozenDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(frozenConnection);
    }

    void fieldMigrationRejectsDependenciesPresetsAndStalePreviewsAndRollsBack()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("field-migration-safety.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Migration safety"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Record"), {}, {}, &typeId, &error));
        QString valueField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Source"), QStringLiteral("integer"), {}, {}, &valueField, &error));
        QString formulaField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Dependent"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{%1} * 2").arg(valueField)}}, &formulaField, &error));
        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Safety object");
        object.values.insert(valueField, 4);
        QVERIFY(repository.saveObject(object, repository.objectTypes(gameId, &error).first().fields, {}, &error));

        FieldMigrationRequest request;
        request.targetType = QStringLiteral("text");
        FieldMigrationPreview preview = repository.previewFieldTypeMigration(valueField, request, &error);
        QVERIFY(!preview.canApply);
        QVERIFY(preview.dependencies.contains(QStringLiteral("formula: Dependent")));

        const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            const QString createdAt = QStringLiteral("2026-11-07T01:00:00Z");
            QVERIFY2(testExec(database, QStringLiteral("INSERT INTO object_presets(id,game_id,object_id,name,values_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?)"),
                {QStringLiteral("preset-migration-0001"), gameId, object.id, QStringLiteral("Preset"), QStringLiteral("{\"%1\":7}").arg(valueField), createdAt, createdAt}, &error), qPrintable(error));
            QVERIFY2(testExec(database, QStringLiteral("CREATE TRIGGER reject_field_migration BEFORE UPDATE OF field_type ON field_definitions BEGIN SELECT RAISE(ABORT,'simulated migration failure'); END"), {}, &error), qPrintable(error));
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(connection);
        request.targetType = QStringLiteral("decimal");
        preview = repository.previewFieldTypeMigration(valueField, request, &error);
        QCOMPARE(preview.presetOverrides, 1);
        QVERIFY(!preview.canApply);
        QVERIFY(preview.blockers.join(QLatin1Char('\n')).contains(QStringLiteral("preset"), Qt::CaseInsensitive));
        // Remove the test preset and use a clean field to verify stale-preview and rollback behavior.
        {
            const QString cleanupConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), cleanupConnection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QVERIFY(testExec(database, QStringLiteral("DROP TRIGGER reject_field_migration"), {}, &error));
            QVERIFY(testExec(database, QStringLiteral("DELETE FROM object_presets WHERE id='preset-migration-0001'"), {}, &error));
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(cleanupConnection);
        }
        QString independentField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Independent"), QStringLiteral("integer"), {}, {}, &independentField, &error));
        FieldMigrationRequest staleRequest;
        staleRequest.targetType = QStringLiteral("decimal");
        FieldMigrationPreview stalePreview = repository.previewFieldTypeMigration(independentField, staleRequest, &error);
        QVERIFY(stalePreview.canApply);
        object.values.insert(valueField, 9);
        QVERIFY(repository.saveObject(object, repository.objectTypes(gameId, &error).first().fields, {}, &error));
        QVERIFY(!repository.applyFieldTypeMigration(independentField, staleRequest, stalePreview.previewToken, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("changed after preview"), Qt::CaseInsensitive));
        stalePreview = repository.previewFieldTypeMigration(independentField, staleRequest, &error);
        QVERIFY(stalePreview.canApply);
        {
            const QString triggerConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), triggerConnection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QVERIFY2(testExec(database, QStringLiteral("CREATE TRIGGER reject_field_migration BEFORE UPDATE OF field_type ON field_definitions WHEN NEW.id='%1' BEGIN SELECT RAISE(ABORT,'simulated migration failure'); END").arg(independentField), {}, &error), qPrintable(error));
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(triggerConnection);
        }
        QVERIFY(!repository.applyFieldTypeMigration(independentField, staleRequest, stalePreview.previewToken, nullptr, &error));
        const auto afterFailure = repository.objectTypes(gameId, &error).first();
        const auto unchanged = std::find_if(afterFailure.fields.cbegin(), afterFailure.fields.cend(), [&](const FieldDefinition &field) { return field.id == independentField; });
        QVERIFY(unchanged != afterFailure.fields.cend());
        QCOMPARE(unchanged->fieldType, QStringLiteral("integer"));
        QSqlDatabase verify = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("migration-audit-verify"));
        verify.setDatabaseName(repository.databasePath());
        QVERIFY(verify.open());
        QSqlQuery audits(verify);
        audits.prepare(QStringLiteral("SELECT COUNT(*) FROM field_type_migrations WHERE field_id=?"));
        audits.addBindValue(independentField);
        QVERIFY(audits.exec() && audits.next());
        QCOMPARE(audits.value(0).toInt(), 0);
        verify.close();
        verify = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("migration-audit-verify"));
    }

    void fieldMigrationPreservesAndValidatesReferenceRelationships()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("field-migration-references.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Reference migrations"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Node"), {}, {}, &typeId, &error));
        QString targetId;
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Target"), {}, {}, &targetId, &error));
        QString referenceId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Link"), QStringLiteral("reference"), {},
            QJsonObject{{QStringLiteral("allowed_type_id"), targetId}}, &referenceId, &error));
        ObjectRecord targetA;
        targetA.gameId = gameId;
        targetA.typeId = targetId;
        targetA.name = QStringLiteral("Target A");
        QVERIFY2(repository.saveObject(targetA, repository.objectTypes(gameId, &error, true).last().fields, {}, &error), qPrintable(error));
        ObjectRecord source;
        source.gameId = gameId;
        source.typeId = typeId;
        source.name = QStringLiteral("Source");
        source.references.insert(referenceId, {targetA.id});
        const QVector<ObjectType> types = repository.objectTypes(gameId, &error, true);
        const auto sourceTypeIt = std::find_if(types.cbegin(), types.cend(), [&](const ObjectType &item) { return item.id == typeId; });
        QVERIFY(sourceTypeIt != types.cend());
        QVERIFY2(repository.saveObject(source, sourceTypeIt->fields, {}, &error), qPrintable(error));

        FieldMigrationRequest refsRequest;
        refsRequest.targetType = QStringLiteral("references");
        refsRequest.targetOptions = QJsonObject{{QStringLiteral("allowed_type_id"), targetId}};
        FieldMigrationPreview preview = repository.previewFieldTypeMigration(referenceId, refsRequest, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(referenceId, refsRequest, preview.previewToken, nullptr, &error), qPrintable(error));
        const auto sourceAfter = repository.objects(gameId, &error);
        const auto migratedSource = std::find_if(sourceAfter.cbegin(), sourceAfter.cend(), [&](const ObjectRecord &item) { return item.id == source.id; });
        QVERIFY(migratedSource != sourceAfter.cend());
        QCOMPARE(migratedSource->references.value(referenceId), QStringList{targetA.id});

        QString oneManyField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Single target in many field"), QStringLiteral("references"), {},
            QJsonObject{{QStringLiteral("allowed_type_id"), targetId}}, &oneManyField, &error));
        source.references.insert(oneManyField, {targetA.id});
        const QVector<ObjectType> beforeSingle = repository.objectTypes(gameId, &error, true);
        const auto beforeSingleType = std::find_if(beforeSingle.cbegin(), beforeSingle.cend(), [&](const ObjectType &item) { return item.id == typeId; });
        QVERIFY(beforeSingleType != beforeSingle.cend());
        QVERIFY2(repository.saveObject(source, beforeSingleType->fields, {}, &error), qPrintable(error));
        FieldMigrationRequest oneRequest;
        oneRequest.targetType = QStringLiteral("reference");
        oneRequest.targetOptions = QJsonObject{{QStringLiteral("allowed_type_id"), targetId}};
        preview = repository.previewFieldTypeMigration(oneManyField, oneRequest, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(oneManyField, oneRequest, preview.previewToken, nullptr, &error), qPrintable(error));
        const QVector<ObjectRecord> afterSingle = repository.objects(gameId, &error);
        const auto oneMigrated = std::find_if(afterSingle.cbegin(), afterSingle.cend(), [&](const ObjectRecord &item) { return item.id == source.id; });
        QVERIFY(oneMigrated != afterSingle.cend());
        QCOMPARE(oneMigrated->references.value(oneManyField), QStringList{targetA.id});

        QString multiField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Many"), QStringLiteral("references"), {},
            QJsonObject{{QStringLiteral("allowed_type_id"), targetId}}, &multiField, &error));
        ObjectRecord targetB;
        targetB.gameId = gameId;
        targetB.typeId = targetId;
        targetB.name = QStringLiteral("Target B");
        const QVector<ObjectType> targetTypes = repository.objectTypes(gameId, &error, true);
        const auto targetTypeIt = std::find_if(targetTypes.cbegin(), targetTypes.cend(), [&](const ObjectType &item) { return item.id == targetId; });
        QVERIFY(targetTypeIt != targetTypes.cend());
        QVERIFY2(repository.saveObject(targetB, targetTypeIt->fields, {}, &error), qPrintable(error));
        source.references.insert(multiField, {targetA.id, targetB.id});
        const QVector<ObjectType> updatedTypes = repository.objectTypes(gameId, &error, true);
        const auto updatedSourceType = std::find_if(updatedTypes.cbegin(), updatedTypes.cend(), [&](const ObjectType &item) { return item.id == typeId; });
        QVERIFY(updatedSourceType != updatedTypes.cend());
        QVERIFY2(repository.saveObject(source, updatedSourceType->fields, {}, &error), qPrintable(error));
        FieldMigrationRequest singleRequest;
        singleRequest.targetType = QStringLiteral("reference");
        singleRequest.targetOptions = QJsonObject{{QStringLiteral("allowed_type_id"), targetId}};
        preview = repository.previewFieldTypeMigration(multiField, singleRequest, &error);
        QVERIFY(!preview.canApply && preview.incompatibleValues == 1);

        QString mappingField;
        QVERIFY(repository.createField(typeId, QStringLiteral("Text mapping"), QStringLiteral("text"), {}, {}, &mappingField, &error));
        source.values.insert(mappingField, QStringLiteral("chosen target"));
        const QVector<ObjectType> beforeMapping = repository.objectTypes(gameId, &error, true);
        const auto beforeMappingType = std::find_if(beforeMapping.cbegin(), beforeMapping.cend(), [&](const ObjectType &item) { return item.id == typeId; });
        QVERIFY(beforeMappingType != beforeMapping.cend());
        QVERIFY2(repository.saveObject(source, beforeMappingType->fields, {}, &error), qPrintable(error));
        FieldMigrationRequest mapRequest;
        mapRequest.targetType = QStringLiteral("reference");
        mapRequest.targetOptions = QJsonObject{{QStringLiteral("allowed_type_id"), targetId}};
        mapRequest.policy.insert(QStringLiteral("reference_mapping"), QJsonObject{{QStringLiteral("chosen target"), targetA.id}});
        preview = repository.previewFieldTypeMigration(mappingField, mapRequest, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(mappingField, mapRequest, preview.previewToken, nullptr, &error), qPrintable(error));
        const QVector<ObjectRecord> afterMapping = repository.objects(gameId, &error);
        const auto mappedSource = std::find_if(afterMapping.cbegin(), afterMapping.cend(), [&](const ObjectRecord &item) { return item.id == source.id; });
        QVERIFY(mappedSource != afterMapping.cend());
        QCOMPARE(mappedSource->references.value(mappingField), QStringList{targetA.id});

        FieldMigrationRequest serializeRequest;
        serializeRequest.targetType = QStringLiteral("text");
        serializeRequest.policy.insert(QStringLiteral("relationship_strategy"), QStringLiteral("serialize_ids"));
        preview = repository.previewFieldTypeMigration(referenceId, serializeRequest, &error);
        QVERIFY2(preview.canApply, qPrintable(preview.blockers.join(QLatin1Char('\n'))));
        QVERIFY2(repository.applyFieldTypeMigration(referenceId, serializeRequest, preview.previewToken, nullptr, &error), qPrintable(error));
        QString serialized;
        {
            const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QSqlQuery value(database);
            value.prepare(QStringLiteral("SELECT value_json FROM field_values WHERE object_id=? AND field_id=?"));
            value.addBindValue(source.id);
            value.addBindValue(referenceId);
            QVERIFY(value.exec() && value.next());
            serialized = QJsonDocument::fromJson(QByteArrayLiteral("[") + value.value(0).toString().toUtf8() + QByteArrayLiteral("]")).array().first().toString();
            database.close();
            database = QSqlDatabase();
            QSqlDatabase::removeDatabase(connection);
        }
        QCOMPARE(serialized, QStringLiteral("[\"%1\"]").arg(targetA.id));
    }

    void formulasUseStableFieldsDefaultsAggregationsAndReportErrors()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("formulas.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Formula game"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Node"), {}, {}, &typeId, &error));
        QString scoreId;
        QString linksId;
        QString bonusId;
        QString enabledId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Score"), QStringLiteral("decimal"), {}, {}, &scoreId, &error));
        QVERIFY(repository.createField(typeId, QStringLiteral("Bonus"), QStringLiteral("integer"), {}, {}, &bonusId, &error));
        QVERIFY(repository.createField(typeId, QStringLiteral("Enabled"), QStringLiteral("boolean"), {}, {}, &enabledId, &error));
        QVERIFY(repository.createField(typeId, QStringLiteral("Children"), QStringLiteral("references"), {},
            QJsonObject{{QStringLiteral("allowed_type_id"), typeId}}, &linksId, &error));
        QString doubledId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Doubled"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{%1} * 2").arg(scoreId)}}, &doubledId, &error));
        QString derivedId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Derived"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("(@{%1} + 4) * 3 - 6 / 2").arg(doubledId)}}, &derivedId, &error));
        QMap<QString, QString> aggregateIds;
        for (const QString &operation : {QStringLiteral("sum"), QStringLiteral("avg"), QStringLiteral("min"),
                                          QStringLiteral("max"), QStringLiteral("count")}) {
            QJsonObject aggregation{{QStringLiteral("relationship_field_id"), linksId},
                                   {QStringLiteral("operation"), operation}};
            if (operation != QStringLiteral("count")) aggregation.insert(QStringLiteral("field_id"), scoreId);
            QString createdFieldId;
            QVERIFY2(repository.createField(typeId, operation + QStringLiteral(" linked score"), QStringLiteral("formula"), {},
                QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{related} + @{%1}").arg(scoreId)},
                            {QStringLiteral("aggregation"), aggregation}},
                &createdFieldId, &error), qPrintable(error));
            aggregateIds.insert(operation + QStringLiteral("Formula"), createdFieldId);
        }
        QString defaultFormulaId;
        QVERIFY(repository.createField(typeId, QStringLiteral("Default formula"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{%1} * 4").arg(bonusId)}}, &defaultFormulaId, &error));
        const auto types = repository.objectTypes(gameId, &error);
        QCOMPARE(types.size(), 1);

        ObjectRecord firstChild;
        firstChild.gameId = gameId;
        firstChild.typeId = typeId;
        firstChild.name = QStringLiteral("First child");
        firstChild.values.insert(scoreId, 3);
        QVERIFY2(repository.saveObject(firstChild, types.first().fields, {}, &error), qPrintable(error));
        ObjectRecord secondChild;
        secondChild.gameId = gameId;
        secondChild.typeId = typeId;
        secondChild.name = QStringLiteral("Second child");
        secondChild.values.insert(scoreId, 10);
        QVERIFY2(repository.saveObject(secondChild, types.first().fields, {}, &error), qPrintable(error));
        ObjectRecord parent;
        parent.gameId = gameId;
        parent.typeId = typeId;
        parent.name = QStringLiteral("Parent");
        parent.values.insert(scoreId, 10);
        parent.references.insert(linksId, {firstChild.id, secondChild.id});
        QVERIFY2(repository.saveObject(parent, types.first().fields, {}, &error), qPrintable(error));

        const QString defaultsConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), defaultsConnection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QSqlQuery defaults(database);
            defaults.prepare(QStringLiteral("UPDATE field_definitions SET default_json=? WHERE id=?"));
            defaults.addBindValue(QStringLiteral("0"));
            defaults.addBindValue(bonusId);
            QVERIFY(defaults.exec());
            defaults.prepare(QStringLiteral("UPDATE field_definitions SET default_json=? WHERE id=?"));
            defaults.addBindValue(QStringLiteral("false"));
            defaults.addBindValue(enabledId);
            QVERIFY(defaults.exec());
            database.close();
        }
        QSqlDatabase::removeDatabase(defaultsConnection);
        ObjectRecord defaultsObject;
        defaultsObject.gameId = gameId;
        defaultsObject.typeId = typeId;
        defaultsObject.name = QStringLiteral("Default values");
        QVERIFY2(repository.saveObject(defaultsObject, types.first().fields, {}, &error), qPrintable(error));

        auto loaded = repository.objects(gameId, &error);
        auto parentIt = std::find_if(loaded.cbegin(), loaded.cend(), [&](const ObjectRecord &item) { return item.id == parent.id; });
        QVERIFY(parentIt != loaded.cend());
        QCOMPARE(parentIt->values.value(doubledId).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(derivedId).toDouble(), 69.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("sumFormula"))).toDouble(), 23.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("avgFormula"))).toDouble(), 16.5);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("minFormula"))).toDouble(), 13.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("maxFormula"))).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("countFormula"))).toDouble(), 12.0);
        const auto defaultIt = std::find_if(loaded.cbegin(), loaded.cend(), [&](const ObjectRecord &item) { return item.id == defaultsObject.id; });
        QVERIFY(defaultIt != loaded.cend());
        QVERIFY(defaultIt->values.contains(bonusId));
        QCOMPARE(defaultIt->values.value(bonusId).toDouble(), 0.0);
        QVERIFY(defaultIt->values.value(enabledId).isBool());
        QVERIFY(!defaultIt->values.value(enabledId).toBool());
        QCOMPARE(defaultIt->values.value(defaultFormulaId).toDouble(), 0.0);
        QVERIFY(defaultIt->formulaErrors.value(doubledId).contains(QStringLiteral("enter a value for Score"), Qt::CaseInsensitive));

        QVERIFY(repository.archiveObject(firstChild.id, &error));
        loaded = repository.objects(gameId, &error);
        parentIt = std::find_if(loaded.cbegin(), loaded.cend(), [&](const ObjectRecord &item) { return item.id == parent.id; });
        QVERIFY(parentIt != loaded.cend());
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("sumFormula"))).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("avgFormula"))).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("minFormula"))).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("maxFormula"))).toDouble(), 20.0);
        QCOMPARE(parentIt->values.value(aggregateIds.value(QStringLiteral("countFormula"))).toDouble(), 11.0);

        QVERIFY(repository.createField(typeId, QStringLiteral("Divide by zero"), QStringLiteral("formula"), {},
            QJsonObject{{QStringLiteral("expression"), QStringLiteral("@{%1} / 0").arg(scoreId)}}, nullptr, &error));
        const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QSqlQuery cycle(database);
            cycle.prepare(QStringLiteral("UPDATE field_definitions SET options_json=? WHERE id=?"));
            cycle.addBindValue(QStringLiteral("{\"expression\":\"@{%1}\"}").arg(derivedId));
            cycle.addBindValue(doubledId);
            QVERIFY(cycle.exec());
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        loaded = repository.objects(gameId, &error);
        parentIt = std::find_if(loaded.cbegin(), loaded.cend(), [&](const ObjectRecord &item) { return item.id == parent.id; });
        QVERIFY(parentIt != loaded.cend());
        QVERIFY(parentIt->formulaErrors.value(doubledId).contains(QStringLiteral("circular"), Qt::CaseInsensitive));
        const auto afterCycleTypes = repository.objectTypes(gameId, &error);
        const auto afterCycleObjects = repository.objects(gameId, &error);
        const auto parentAfterCycle = std::find_if(afterCycleObjects.cbegin(), afterCycleObjects.cend(), [&](const ObjectRecord &item) { return item.id == parent.id; });
        QVERIFY(parentAfterCycle != afterCycleObjects.cend());
        const auto zeroDivision = std::find_if(afterCycleTypes.first().fields.cbegin(), afterCycleTypes.first().fields.cend(), [](const FieldDefinition &field) {
            return field.name == QStringLiteral("Divide by zero");
        });
        QVERIFY(zeroDivision != afterCycleTypes.first().fields.cend());
        QVERIFY(parentAfterCycle->formulaErrors.value(zeroDivision->id).contains(QStringLiteral("division by zero"), Qt::CaseInsensitive));
    }

    void goalsChecklistProgressAndDependenciesRemainConsistent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("goals.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QVERIFY2(repository.createGame(QStringLiteral("Goal game"), {}, {}, &gameId, &error), qPrintable(error));

        Goal manual;
        manual.gameId = gameId;
        manual.title = QStringLiteral("Finish the tutorial");
        manual.goalType = QStringLiteral("manual");
        QVERIFY2(repository.saveGoal(manual, {}, &error), qPrintable(error));

        Goal numeric;
        numeric.gameId = gameId;
        numeric.title = QStringLiteral("Collect ten items");
        numeric.goalType = QStringLiteral("numeric");
        numeric.target = 10;
        numeric.current = 3;
        numeric.prerequisiteIds = QStringList{manual.id};
        QVERIFY2(repository.saveGoal(numeric, {}, &error), qPrintable(error));

        Goal checklist;
        checklist.gameId = gameId;
        checklist.title = QStringLiteral("Daily routine");
        checklist.goalType = QStringLiteral("checklist");
        checklist.prerequisiteIds = QStringList{manual.id};
        const QVector<GoalTask> initialTasks = {
            GoalTask{{}, QStringLiteral("Collect reward"), false, 0},
            GoalTask{{}, QStringLiteral("Spend stamina"), false, 1}
        };
        QVERIFY2(repository.saveGoal(checklist, initialTasks, &error), qPrintable(error));

        QString characterTypeId;
        QVERIFY2(repository.createObjectType(gameId, QStringLiteral("Character"), {}, {}, &characterTypeId, &error), qPrintable(error));
        QString levelFieldId;
        QVERIFY2(repository.createField(characterTypeId, QStringLiteral("Level"), QStringLiteral("integer"), {}, {}, &levelFieldId, &error), qPrintable(error));
        ObjectRecord character;
        character.gameId = gameId;
        character.typeId = characterTypeId;
        character.name = QStringLiteral("Traveler");
        character.values.insert(levelFieldId, 50);
        QVERIFY2(repository.saveObject(character, repository.objectTypes(gameId, &error).first().fields, {}, &error), qPrintable(error));
        Goal fieldTarget;
        fieldTarget.gameId = gameId;
        fieldTarget.title = QStringLiteral("Reach level eighty");
        fieldTarget.goalType = QStringLiteral("field_target");
        fieldTarget.objectId = character.id;
        fieldTarget.fieldId = levelFieldId;
        fieldTarget.target = 80;
        QVERIFY2(repository.saveGoal(fieldTarget, {}, &error), qPrintable(error));
        Goal followup;
        followup.gameId = gameId;
        followup.title = QStringLiteral("Spend the collected items");
        followup.goalType = QStringLiteral("manual");
        followup.prerequisiteIds = QStringList{numeric.id};
        QVERIFY2(repository.saveGoal(followup, {}, &error), qPrintable(error));

        QVector<Goal> items = repository.goals(gameId, &error);
        QCOMPARE(items.size(), 5);
        auto numericItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == numeric.id; });
        QVERIFY(numericItem != items.cend());
        QVERIFY(!numericItem->ready);
        QCOMPARE(numericItem->progress, 30.0);
        auto checklistItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == checklist.id; });
        QVERIFY(checklistItem != items.cend());
        QCOMPARE(checklistItem->tasks.size(), 2);
        QVERIFY(!checklistItem->completed);
        auto fieldTargetItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == fieldTarget.id; });
        QVERIFY(fieldTargetItem != items.cend());
        QCOMPARE(fieldTargetItem->progress, 62.5);
        QVERIFY(!fieldTargetItem->completed);
        auto followupItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == followup.id; });
        QVERIFY(followupItem != items.cend());
        QVERIFY(!followupItem->ready);

        QVERIFY2(repository.setGoalTaskCompleted(checklistItem->tasks.first().id, true, &error), qPrintable(error));
        QVERIFY(!repository.setGoalTaskCompleted(checklistItem->tasks.last().id, true, &error));
        QVERIFY(error.contains(QStringLiteral("prerequisite"), Qt::CaseInsensitive));

        Goal cycle = *std::find_if(items.begin(), items.end(), [&](const Goal &item) { return item.id == manual.id; });
        cycle.prerequisiteIds = QStringList{numeric.id};
        QVERIFY(!repository.saveGoal(cycle, {}, &error));
        QVERIFY(error.contains(QStringLiteral("cycle"), Qt::CaseInsensitive));

        QVERIFY2(repository.setGoalCompleted(manual.id, true, &error), qPrintable(error));
        QVERIFY2(repository.setGoalTaskCompleted(checklistItem->tasks.last().id, true, &error), qPrintable(error));
        QVERIFY2(repository.setNumericGoalCurrent(numeric.id, 10, &error), qPrintable(error));

        items = repository.goals(gameId, &error);
        checklistItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == checklist.id; });
        numericItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == numeric.id; });
        QVERIFY(checklistItem != items.cend());
        QVERIFY(checklistItem->completed);
        QCOMPARE(checklistItem->progress, 100.0);
        QVERIFY(numericItem != items.cend());
        QVERIFY(numericItem->completed);
        QCOMPARE(numericItem->progress, 100.0);
        followupItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == followup.id; });
        QVERIFY(followupItem != items.cend());
        QVERIFY(followupItem->ready);
        character.values[levelFieldId] = 80;
        const QVector<ObjectType> currentTypes = repository.objectTypes(gameId, &error);
        QVERIFY2(repository.saveObject(character, currentTypes.first().fields, {}, &error), qPrintable(error));
        items = repository.goals(gameId, &error);
        fieldTargetItem = std::find_if(items.cbegin(), items.cend(), [&](const Goal &item) { return item.id == fieldTarget.id; });
        QVERIFY(fieldTargetItem != items.cend());
        QVERIFY(fieldTargetItem->completed);
        QCOMPARE(fieldTargetItem->progress, 100.0);

        QString otherGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Other goal game"), {}, {}, &otherGameId, &error), qPrintable(error));
        Goal foreign;
        foreign.gameId = otherGameId;
        foreign.title = QStringLiteral("Foreign prerequisite");
        foreign.goalType = QStringLiteral("manual");
        QVERIFY2(repository.saveGoal(foreign, {}, &error), qPrintable(error));
        Goal invalidDependency = numeric;
        invalidDependency.prerequisiteIds = QStringList{foreign.id};
        QVERIFY(!repository.saveGoal(invalidDependency, {}, &error));
        QVERIFY(error.contains(QStringLiteral("same game"), Qt::CaseInsensitive));

        QVERIFY2(repository.deleteGoal(checklist.id, &error), qPrintable(error));
        const QVector<Goal> afterDelete = repository.goals(gameId, &error);
        QCOMPARE(afterDelete.size(), 4);
        QVERIFY(std::none_of(afterDelete.cbegin(), afterDelete.cend(), [&](const Goal &item) { return item.id == checklist.id; }));
    }

    void archivedGamesSectionsAndObjectsCanBeRestored()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("archives.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString activeGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Active game"), {}, {}, &activeGameId, &error), qPrintable(error));
        QVERIFY2(repository.createSection(activeGameId, {}, QStringLiteral("Archived section"), {}, &error), qPrintable(error));
        const QVector<Section> sections = repository.sections(activeGameId, &error);
        QCOMPARE(sections.size(), 1);
        QString typeId;
        QVERIFY2(repository.createObjectType(activeGameId, QStringLiteral("Item"), {}, {}, &typeId, &error), qPrintable(error));
        ObjectRecord object;
        object.gameId = activeGameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Archived object");
        QVERIFY2(repository.saveObject(object, {}, {}, &error), qPrintable(error));
        QVERIFY2(repository.archiveSection(sections.first().id, &error), qPrintable(error));
        QVERIFY2(repository.archiveObject(object.id, &error), qPrintable(error));

        QString archivedGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Archived game"), {}, {}, &archivedGameId, &error), qPrintable(error));
        QVERIFY2(repository.archiveGame(archivedGameId, &error), qPrintable(error));
        QVector<ArchivedEntry> entries = repository.archivedEntries(&error);
        QCOMPARE(entries.size(), 3);
        for (const ArchivedEntry &entry : entries) QVERIFY2(repository.restoreArchivedEntry(entry, &error), qPrintable(error));

        QCOMPARE(repository.games(&error).size(), 2);
        QCOMPARE(repository.sections(activeGameId, &error).size(), 1);
        QCOMPARE(repository.objects(activeGameId, &error).size(), 1);
        QVERIFY(repository.archivedEntries(&error).isEmpty());
    }

    void imageDecoderRejectsOversizedInputBeforeParsing()
    {
        const QByteArray oversized(ImageDecoder::maximumBytes + 1, '\0');
        QString error;
        QVERIFY(ImageDecoder::decode(oversized, &error).isNull());
        QVERIFY(error.contains(QStringLiteral("5 MB")));
    }

    void objectReadsUseTypedDefaultsWithoutOverwritingStoredValues()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("defaults.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Defaults"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Entry"), {}, {}, &typeId, &error));

        QString boolId;
        QString zeroId;
        QString textId;
        QString progressId;
        QVERIFY2(repository.createField(typeId, QStringLiteral("Enabled"), QStringLiteral("boolean"), {}, {},
            &boolId, &error, QStringLiteral("Enabled by default"), QJsonValue(false)), qPrintable(error));
        QVERIFY2(repository.createField(typeId, QStringLiteral("Count"), QStringLiteral("integer"), {}, {},
            &zeroId, &error, {}, QJsonValue(0)), qPrintable(error));
        QVERIFY2(repository.createField(typeId, QStringLiteral("Label"), QStringLiteral("text"), {}, {},
            &textId, &error, {}, QJsonValue(QStringLiteral("Starter"))), qPrintable(error));
        QVERIFY2(repository.createField(typeId, QStringLiteral("Progress"), QStringLiteral("progress"), {}, {},
            &progressId, &error, {}, QJsonObject{{QStringLiteral("current"), 0}, {QStringLiteral("max"), 10}}), qPrintable(error));
        QVERIFY(!repository.createField(typeId, QStringLiteral("Invalid percentage"), QStringLiteral("percentage"), {}, {},
            nullptr, &error, {}, QJsonValue(101)));
        QVERIFY(error.contains(QStringLiteral("between 0 and 100")));

        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Uses defaults");
        auto types = repository.objectTypes(gameId, &error);
        QVERIFY2(repository.saveObject(object, types.first().fields, {}, &error), qPrintable(error));
        auto objects = repository.objects(gameId, &error);
        QCOMPARE(objects.size(), 1);
        QCOMPARE(objects.first().values.value(boolId).isBool(), true);
        QCOMPARE(objects.first().values.value(boolId).toBool(), false);
        QCOMPARE(objects.first().values.value(zeroId).toDouble(), 0.0);
        QCOMPARE(objects.first().values.value(textId).toString(), QStringLiteral("Starter"));
        QCOMPARE(objects.first().values.value(progressId).toObject().value(QStringLiteral("max")).toDouble(), 10.0);

        ObjectRecord explicitValues = objects.first();
        explicitValues.values[boolId] = false;
        explicitValues.values[zeroId] = 0;
        QVERIFY2(repository.saveObject(explicitValues, types.first().fields, {}, &error), qPrintable(error));
        objects = repository.objects(gameId, &error);
        QCOMPARE(objects.first().values.value(boolId).toBool(), false);
        QCOMPARE(objects.first().values.value(zeroId).toDouble(), 0.0);
        const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(repository.databasePath());
            QVERIFY(database.open());
            QSqlQuery stored(database);
            stored.prepare(QStringLiteral("SELECT field_id,value_json FROM field_values WHERE object_id=?"));
            stored.addBindValue(object.id);
            QVERIFY(stored.exec());
            QMap<QString, QString> raw;
            while (stored.next()) raw.insert(stored.value(0).toString(), stored.value(1).toString());
            QCOMPARE(raw.value(boolId), QStringLiteral("false"));
            QCOMPARE(raw.value(zeroId), QStringLiteral("0"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void assetStorageEnforcesOwnershipQuotaReferencesAndRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString databasePath = directory.filePath(QStringLiteral("assets.sqlite"));
        GameRepository repository(databasePath);
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));

        auto encode = [](const QByteArray &format) {
            QImage source(8, 8, QImage::Format_ARGB32);
            source.fill(QColor(120, 45, 200));
            QByteArray bytes;
            QBuffer buffer(&bytes);
            if (!buffer.open(QIODevice::WriteOnly) || !source.save(&buffer, format.constData())) return QByteArray();
            return bytes;
        };
        const QByteArray png = encode("PNG");
        const QByteArray jpeg = encode("JPEG");
        const QByteArray bmp = encode("BMP");
        QVERIFY(!png.isEmpty());
        QVERIFY(!jpeg.isEmpty());
        QVERIFY(!bmp.isEmpty());

        UploadedAsset cover;
        QVERIFY2(repository.uploadAsset(png, &cover, &error), qPrintable(error));
        QCOMPARE(cover.reference, QStringLiteral("/api/assets/") + cover.id);
        QVERIFY(!repository.loadAssetImage(cover.reference, &error).isNull());
        UploadedAsset converted;
        QVERIFY2(repository.uploadAsset(jpeg, &converted, &error), qPrintable(error));
        QVERIFY(!repository.loadAssetImage(converted.reference, &error).isNull());
        UploadedAsset bitmap;
        QVERIFY2(repository.uploadAsset(bmp, &bitmap, &error), qPrintable(error));
        QVERIFY(!repository.loadAssetImage(bitmap.reference, &error).isNull());
        QVERIFY(!repository.uploadAsset(QByteArray("not an image"), nullptr, &error));
        QVERIFY(!repository.uploadAsset(QByteArray(ImageDecoder::maximumBytes + 1, '\0'), nullptr, &error));

        const QString pathConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase pathDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), pathConnection);
            pathDb.setDatabaseName(databasePath);
            QVERIFY(pathDb.open());
            QSqlQuery tamper(pathDb);
            tamper.prepare(QStringLiteral("UPDATE uploaded_assets SET filename=? WHERE id=?"));
            tamper.addBindValue(QStringLiteral("..\\outside.png"));
            tamper.addBindValue(cover.id);
            QVERIFY(tamper.exec());
            pathDb.close();
        }
        QSqlDatabase::removeDatabase(pathConnection);
        QVERIFY(repository.loadAssetImage(cover.reference, &error).isNull());
        QVERIFY(error.contains(QStringLiteral("stored image name"), Qt::CaseInsensitive));
        const QString restorePathConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase pathDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), restorePathConnection);
            pathDb.setDatabaseName(databasePath);
            QVERIFY(pathDb.open());
            QSqlQuery restore(pathDb);
            restore.prepare(QStringLiteral("UPDATE uploaded_assets SET filename=? WHERE id=?"));
            restore.addBindValue(cover.id + QStringLiteral(".png"));
            restore.addBindValue(cover.id);
            QVERIFY(restore.exec());
            pathDb.close();
        }
        QSqlDatabase::removeDatabase(restorePathConnection);
        QVERIFY(!repository.loadAssetImage(cover.reference, &error).isNull());

        const QString otherConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString foreignAssetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), otherConnection);
            other.setDatabaseName(databasePath);
            QVERIFY2(other.open(), qPrintable(other.lastError().text()));
            QSqlQuery insertUser(other);
            QVERIFY(insertUser.exec(QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES('other-user','Other','hash','now')")));
            QSqlQuery insertAsset(other);
            insertAsset.prepare(QStringLiteral("INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) VALUES(?, 'other-user', NULL, 'foreign.png', 'image/png', 1, 'now')"));
            insertAsset.addBindValue(foreignAssetId);
            QVERIFY2(insertAsset.exec(), qPrintable(insertAsset.lastError().text()));
            other.close();
        }
        QSqlDatabase::removeDatabase(otherConnection);
        QVERIFY(repository.loadAssetImage(QStringLiteral("/api/assets/") + foreignAssetId, &error).isNull());
        QVERIFY(!repository.deleteAsset(foreignAssetId, nullptr, &error));
        QCOMPARE(repository.uploadedAssets(&error).size(), 3);

        QString gameId;
        QVERIFY2(repository.createGame(QStringLiteral("Cover game"), QStringLiteral("Description"), QStringLiteral("🎮"),
                                       &gameId, &error, QStringLiteral("Private notes"), cover.reference), qPrintable(error));
        auto games = repository.games(&error);
        QCOMPARE(games.size(), 1);
        QCOMPARE(games.first().personalNotes, QStringLiteral("Private notes"));
        QCOMPARE(games.first().coverImage, cover.reference);
        Game changed = games.first();
        changed.personalNotes = QStringLiteral("Updated notes");
        QVERIFY2(repository.updateGame(changed, &error), qPrintable(error));
        games = repository.games(&error);
        QCOMPARE(games.first().personalNotes, QStringLiteral("Updated notes"));
        QCOMPARE(games.first().coverImage, cover.reference);

        QString typeId;
        QVERIFY2(repository.createObjectType(gameId, QStringLiteral("Type"), {}, {}, &typeId, &error, converted.reference), qPrintable(error));
        QCOMPARE(repository.objectTypes(gameId, &error).first().image, converted.reference);
        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Archived image owner");
        object.image = cover.reference;
        QVERIFY2(repository.saveObject(object, {}, {}, &error), qPrintable(error));
        QVERIFY2(repository.archiveObject(object.id, &error), qPrintable(error));
        QVERIFY(!repository.deleteAsset(cover.id, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("archived"), Qt::CaseInsensitive));
        QVERIFY(!repository.deleteAsset(converted.id, nullptr, &error));

        QVERIFY(!repository.createGame(QStringLiteral("Bad owner reference"), {}, {}, nullptr, &error,
                                       {}, QStringLiteral("/api/assets/") + foreignAssetId));
        QCOMPARE(repository.games(&error).size(), 1);

        UploadedAsset cleanup;
        QVERIFY2(repository.uploadAsset(png, &cleanup, &error), qPrintable(error));
        const QString cleanupDirectory = directory.filePath(QStringLiteral("uploads/") + cleanup.id + QStringLiteral(".png"));
        QVERIFY(QFile::remove(cleanupDirectory));
        QVERIFY(QDir().mkdir(cleanupDirectory));
        bool pending = false;
        QVERIFY(repository.deleteAsset(cleanup.id, &pending, &error));
        QVERIFY(pending);
        QVERIFY(!error.isEmpty());
        const auto queuedAssets = repository.uploadedAssets(&error);
        bool foundPending = false;
        for (const UploadedAsset &asset : queuedAssets) foundPending = foundPending || (asset.id == cleanup.id && asset.pendingDeletion);
        QVERIFY(foundPending);
        QVERIFY(QDir().rmdir(cleanupDirectory));
        QVERIFY2(repository.retryAssetCleanup(&error), qPrintable(error));
        for (const UploadedAsset &asset : repository.uploadedAssets(&error)) QVERIFY(asset.id != cleanup.id);

        const QString quotaPath = directory.filePath(QStringLiteral("quota.sqlite"));
        GameRepository quotaRepository(quotaPath);
        QVERIFY2(quotaRepository.open(&error), qPrintable(error));
        const QString quotaConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase quotaDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), quotaConnection);
            quotaDb.setDatabaseName(quotaPath);
            QVERIFY2(quotaDb.open(), qPrintable(quotaDb.lastError().text()));
            QVERIFY(quotaDb.transaction());
            QSqlQuery insert(quotaDb);
            insert.prepare(QStringLiteral("INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) VALUES(?, 'native-desktop-local', NULL, 'quota.png', 'image/png', 1, 'now')"));
            for (int index = 0; index < 1000; ++index) {
                insert.bindValue(0, QStringLiteral("quota-%1").arg(index));
                QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
            }
            QVERIFY(quotaDb.commit());
            quotaDb.close();
        }
        QSqlDatabase::removeDatabase(quotaConnection);
        QVERIFY(!quotaRepository.uploadAsset(png, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("1000")));
        QCOMPARE(quotaRepository.uploadedAssets(&error).size(), 1000);

        const QString byteQuotaPath = directory.filePath(QStringLiteral("byte-quota.sqlite"));
        GameRepository byteQuotaRepository(byteQuotaPath);
        QVERIFY2(byteQuotaRepository.open(&error), qPrintable(error));
        const QString byteQuotaConnection = QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase byteDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), byteQuotaConnection);
            byteDb.setDatabaseName(byteQuotaPath);
            QVERIFY(byteDb.open());
            QSqlQuery insert(byteDb);
            insert.prepare(QStringLiteral("INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at) VALUES('full','native-desktop-local',NULL,'full.png','image/png',?, 'now')"));
            insert.addBindValue(250LL * 1024 * 1024);
            QVERIFY(insert.exec());
            byteDb.close();
        }
        QSqlDatabase::removeDatabase(byteQuotaConnection);
        QVERIFY(!byteQuotaRepository.uploadAsset(png, nullptr, &error));
        QVERIFY(error.contains(QStringLiteral("250 MB")));
    }
};

QTEST_GUILESS_MAIN(DatabaseTests)
#include "database_tests.moc"
