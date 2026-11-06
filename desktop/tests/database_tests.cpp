#include "infrastructure/database/game_repository.h"
#include "infrastructure/database/migration_runner.h"
#include "infrastructure/assets/image_decoder.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <QtTest>

using namespace gmst;

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
};

QTEST_GUILESS_MAIN(DatabaseTests)
#include "database_tests.moc"
