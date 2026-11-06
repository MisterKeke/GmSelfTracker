#include "infrastructure/database/game_repository.h"
#include "infrastructure/database/migration_runner.h"
#include "infrastructure/assets/image_decoder.h"
#include "domain/formula_evaluator.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
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
