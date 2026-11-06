#include "infrastructure/database/game_repository.h"

#include "infrastructure/database/migration_runner.h"
#include "domain/formula_evaluator.h"
#include "infrastructure/assets/image_decoder.h"

#include <QBuffer>
#include <QDir>
#include <QDateTime>
#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <cmath>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <limits>

namespace gmst {
namespace {

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString nonNullTrimmed(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed.isNull() ? QStringLiteral("") : trimmed;
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonValue parseJsonValue(const QString &raw)
{
    if (raw.isEmpty()) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError) {
        if (document.isObject()) return document.object();
        if (document.isArray()) return document.array();
    }
    const QJsonDocument wrapped = QJsonDocument::fromJson((QStringLiteral("[") + raw + QLatin1Char(']')).toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && !wrapped.array().isEmpty()) return wrapped.array().first();
    return {};
}

QByteArray compactJson(const QJsonValue &value)
{
    if (value.isObject()) return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    if (value.isArray()) return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

bool validateDefaultValue(const QString &fieldType, const QJsonObject &options,
                          const QJsonValue &value, QString *error)
{
    if (value.isUndefined() || value.isNull()) return true;
    const auto invalid = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (fieldType == QStringLiteral("formula") || fieldType == QStringLiteral("reference") ||
        fieldType == QStringLiteral("references"))
        return invalid(QStringLiteral("Formula and object-reference fields cannot have a default value."));
    if (fieldType == QStringLiteral("text") || fieldType == QStringLiteral("long_text")) {
        const int maximum = fieldType == QStringLiteral("text") ? 2000 : 50000;
        if (!value.isString()) return invalid(QStringLiteral("Enter text for the field default."));
        if (value.toString().size() > maximum)
            return invalid(QStringLiteral("Text default exceeds the %1 character limit.").arg(maximum));
        return true;
    }
    if (fieldType == QStringLiteral("integer")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble() ||
            static_cast<long double>(value.toDouble()) < static_cast<long double>(std::numeric_limits<qint64>::min()) ||
            static_cast<long double>(value.toDouble()) > static_cast<long double>(std::numeric_limits<qint64>::max()))
            return invalid(QStringLiteral("Integer defaults must be whole numbers within the supported range."));
        return true;
    }
    if (fieldType == QStringLiteral("decimal") || fieldType == QStringLiteral("percentage")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
            return invalid(QStringLiteral("Enter a finite numeric default."));
        if (fieldType == QStringLiteral("percentage") && (value.toDouble() < 0 || value.toDouble() > 100))
            return invalid(QStringLiteral("Percentage defaults must be between 0 and 100."));
        return true;
    }
    if (fieldType == QStringLiteral("boolean")) {
        if (!value.isBool()) return invalid(QStringLiteral("Boolean defaults must be true or false."));
        return true;
    }
    if (fieldType == QStringLiteral("select")) {
        if (!value.isString()) return invalid(QStringLiteral("Choose one of the available select options for the default."));
        for (const QJsonValue &choice : options.value(QStringLiteral("choices")).toArray())
            if (choice.toString() == value.toString()) return true;
        return invalid(QStringLiteral("Select defaults must match one of the available choices."));
    }
    if (fieldType == QStringLiteral("date")) {
        const QString date = value.toString();
        if (!value.isString() || !QDate::fromString(date, Qt::ISODate).isValid() || date.size() != 10)
            return invalid(QStringLiteral("Date defaults must use YYYY-MM-DD."));
        return true;
    }
    if (fieldType == QStringLiteral("progress")) {
        const QJsonObject progress = value.toObject();
        const QJsonValue current = progress.value(QStringLiteral("current"));
        const QJsonValue maximum = progress.value(QStringLiteral("max"));
        if (!value.isObject() || !current.isDouble() || !maximum.isDouble() ||
            !std::isfinite(current.toDouble()) || !std::isfinite(maximum.toDouble()) || maximum.toDouble() <= 0)
            return invalid(QStringLiteral("Progress defaults need finite current and positive maximum values."));
        return true;
    }
    return invalid(QStringLiteral("This field type does not accept a default value."));
}

QString historyEventId()
{
    return uuid();
}

} // namespace

GameRepository::GameRepository(const QString &databasePath)
    : m_path(databasePath), m_assetRoot(QFileInfo(databasePath).dir().filePath(QStringLiteral("uploads"))),
      m_connectionName(QStringLiteral("gmst-%1").arg(uuid()))
{
}

GameRepository::~GameRepository()
{
    if (m_database.isValid()) {
        const QString name = m_connectionName;
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

bool GameRepository::open(QString *error)
{
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("Could not create the local data directory.");
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_path);
    if (!m_database.open()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QString migrationError;
    if (!MigrationRunner::apply(m_database, &migrationError)) {
        if (error) *error = migrationError;
        return false;
    }
    if (!ensureLocalProfile(error)) return false;
    QString cleanupError;
    cleanupPendingAssets(&cleanupError);
    return true;
}

bool GameRepository::isOpen() const
{
    return m_database.isOpen();
}

bool GameRepository::ensureLocalProfile(QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO users(id, username, password_hash, created_at) VALUES(?, ?, ?, ?)"));
    query.addBindValue(m_userId);
    query.addBindValue(QStringLiteral("Local Player"));
    query.addBindValue(QStringLiteral("!native-desktop-profile"));
    query.addBindValue(nowUtc());
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    return true;
}

QVector<Game> GameRepository::games(QString *error) const
{
    QVector<Game> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT g.id, g.name, g.description, g.icon, g.cover_image, g.personal_notes, "
        "(SELECT COUNT(*) FROM objects o WHERE o.game_id=g.id AND o.archived=0), "
        "(SELECT COUNT(*) FROM sections s WHERE s.game_id=g.id AND s.archived=0), "
        "(SELECT COUNT(*) FROM goals gl WHERE gl.game_id=g.id), "
        "(SELECT COUNT(*) FROM goals gl WHERE gl.game_id=g.id AND gl.completed=1) "
        "FROM games g WHERE g.user_id=? AND g.archived=0 ORDER BY g.updated_at DESC, g.name COLLATE NOCASE"));
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        result.append(Game{
            query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(),
            query.value(6).toInt(), query.value(7).toInt(), query.value(8).toInt(), query.value(9).toInt()
        });
    }
    return result;
}

QVector<Section> GameRepository::sections(const QString &gameId, QString *error) const
{
    QVector<Section> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT s.id,s.game_id,COALESCE(s.parent_id,''),s.name,s.icon,s.sort_order "
        "FROM sections s JOIN games g ON g.id=s.game_id WHERE s.game_id=? AND s.archived=0 "
        "AND g.user_id=? AND g.archived=0 ORDER BY s.sort_order,s.name COLLATE NOCASE,s.id"));
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        result.append(Section{query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(), query.value(4).toString(), query.value(5).toInt()});
    }
    return result;
}

QVector<ObjectType> GameRepository::objectTypes(const QString &gameId, QString *error) const
{
    QVector<ObjectType> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT t.id,t.game_id,t.name,t.description,t.icon,t.image FROM object_types t "
        "JOIN games g ON g.id=t.game_id WHERE t.game_id=? AND g.user_id=? AND g.archived=0 "
        "AND t.archived=0 ORDER BY t.sort_order,t.name COLLATE NOCASE"));
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        ObjectType type{query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(), {}};
        QSqlQuery fieldQuery(m_database);
        fieldQuery.prepare(QStringLiteral(
            "SELECT id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,archived "
            "FROM field_definitions WHERE object_type_id=? ORDER BY sort_order,name COLLATE NOCASE"));
        fieldQuery.addBindValue(type.id);
        if (!fieldQuery.exec()) {
            if (error) *error = fieldQuery.lastError().text();
            return {};
        }
        while (fieldQuery.next()) {
            const QVariant defaultValue = fieldQuery.value(6);
            const QJsonObject options = parseJsonValue(fieldQuery.value(7).toString()).toObject();
            type.fields.append(FieldDefinition{
                fieldQuery.value(0).toString(), fieldQuery.value(1).toString(), fieldQuery.value(2).toString(),
                fieldQuery.value(3).toString(), fieldQuery.value(4).toString(), fieldQuery.value(5).toString(),
                defaultValue.isNull() ? QJsonValue() : parseJsonValue(defaultValue.toString()), options,
                fieldQuery.value(8).toInt(), fieldQuery.value(9).toInt() != 0
            });
        }
        result.append(type);
    }
    return result;
}

QVector<ObjectRecord> GameRepository::objects(const QString &gameId, QString *error) const
{
    if (error) error->clear();
    QVector<ObjectRecord> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT o.id,o.game_id,o.object_type_id,t.name,o.name,o.description,o.image "
        "FROM objects o JOIN object_types t ON t.id=o.object_type_id JOIN games g ON g.id=o.game_id "
        "WHERE o.game_id=? AND g.user_id=? AND g.archived=0 AND o.archived=0 AND t.archived=0 "
        "ORDER BY o.updated_at DESC,o.name COLLATE NOCASE"));
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    QMap<QString, QStringList> placements;
    QSqlQuery placementQuery(m_database);
    placementQuery.prepare(QStringLiteral(
        "SELECT p.object_id,p.section_id FROM section_placements p "
        "JOIN sections s ON s.id=p.section_id JOIN games g ON g.id=p.game_id "
        "WHERE p.game_id=? AND g.user_id=? AND g.archived=0 AND s.archived=0 ORDER BY p.section_id"));
    placementQuery.addBindValue(gameId);
    placementQuery.addBindValue(m_userId);
    if (!placementQuery.exec()) {
        if (error) *error = placementQuery.lastError().text();
        return {};
    }
    while (placementQuery.next())
        placements[placementQuery.value(0).toString()].append(placementQuery.value(1).toString());
    while (query.next()) {
        ObjectRecord object;
        object.id = query.value(0).toString();
        object.gameId = query.value(1).toString();
        object.typeId = query.value(2).toString();
        object.typeName = query.value(3).toString();
        object.name = query.value(4).toString();
        object.description = query.value(5).toString();
        object.image = query.value(6).toString();
        object.sectionIds = placements.value(object.id);
        QSqlQuery valuesQuery(m_database);
        valuesQuery.prepare(QStringLiteral("SELECT field_id,value_json FROM field_values WHERE object_id=?"));
        valuesQuery.addBindValue(object.id);
        if (!valuesQuery.exec()) {
            if (error) *error = valuesQuery.lastError().text();
            return {};
        }
        while (valuesQuery.next()) object.values.insert(valuesQuery.value(0).toString(), parseJsonValue(valuesQuery.value(1).toString()));
        QSqlQuery referencesQuery(m_database);
        referencesQuery.prepare(QStringLiteral(
            "SELECT field_id,referenced_object_id FROM object_relationships WHERE object_id=? ORDER BY referenced_object_id"));
        referencesQuery.addBindValue(object.id);
        if (!referencesQuery.exec()) {
            if (error) *error = referencesQuery.lastError().text();
            return {};
        }
        while (referencesQuery.next()) object.references[referencesQuery.value(0).toString()].append(referencesQuery.value(1).toString());
        result.append(object);
    }
    const QVector<ObjectType> types = objectTypes(gameId, error);
    if (types.isEmpty() && error && !error->isEmpty()) return {};
    QMap<QString, QVector<FieldDefinition>> fieldsByType;
    for (const ObjectType &type : types) fieldsByType.insert(type.id, type.fields);
    for (ObjectRecord &object : result) {
        const QVector<FieldDefinition> fields = fieldsByType.value(object.typeId);
        for (const FieldDefinition &field : fields) {
            if (!field.archived && !object.values.contains(field.id) &&
                !field.defaultValue.isUndefined() && !field.defaultValue.isNull())
                object.values.insert(field.id, field.defaultValue);
        }
        FormulaEvaluator::evaluate(fields, object, types, result);
    }
    return result;
}

QMap<QString, QString> GameRepository::historyFields(const QString &objectId, QString *error) const
{
    if (error) error->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT c.field_id,c.field_name FROM object_history_events e JOIN games g ON g.id=e.game_id "
        "JOIN object_history_changes c ON c.event_id=e.id "
        "WHERE e.game_id=(SELECT game_id FROM objects WHERE id=e.object_id) "
        "AND e.object_id=? AND g.user_id=? AND c.field_id<>'' "
        "ORDER BY rtrim(e.created_at,'Z'),e.rowid,c.id"));
    query.addBindValue(objectId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return {};
    }
    QMap<QString, QString> fields;
    while (query.next()) fields.insert(query.value(0).toString(), query.value(1).toString());
    if (query.lastError().isValid()) {
        if (error) *error = query.lastError().text();
        return {};
    }
    return fields;
}

HistoryPage GameRepository::objectHistory(const QString &objectId, const HistoryFilter &filter, QString *error) const
{
    if (error) error->clear();
    QString where = QStringLiteral("WHERE e.game_id=(SELECT game_id FROM objects WHERE id=?) AND e.object_id=? AND g.user_id=?");
    QVariantList parameters{objectId, objectId, m_userId};
    if (!filter.eventType.isEmpty()) {
        where += QStringLiteral(" AND e.event_type=?");
        parameters.append(filter.eventType);
    }
    if (!filter.fieldId.isEmpty()) {
        where += QStringLiteral(" AND EXISTS(SELECT 1 FROM object_history_changes c WHERE c.event_id=e.id AND c.field_id=?)");
        parameters.append(filter.fieldId);
    }
    if (filter.since.isValid()) {
        where += QStringLiteral(" AND e.created_at>=?");
        parameters.append(filter.since.toString(Qt::ISODate));
    }
    if (filter.until.isValid()) {
        where += QStringLiteral(" AND substr(e.created_at,1,10)<=?");
        parameters.append(filter.until.toString(Qt::ISODate));
    }
    const int pageSize = filter.limit > 0 ? std::clamp(filter.limit, 1, 100) : HistoryFilter::pageSize;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "WITH page AS (SELECT e.id,e.event_type,e.created_at,e.rowid AS sequence "
        "FROM object_history_events e JOIN games g ON g.id=e.game_id %1 "
        "ORDER BY rtrim(e.created_at,'Z') DESC,sequence DESC LIMIT ? OFFSET ?) "
        "SELECT p.id,p.event_type,p.created_at,c.field_id,c.field_name,c.field_type,c.before_field_type,c.before_json,c.after_json "
        "FROM page p LEFT JOIN object_history_changes c ON c.event_id=p.id "
        "ORDER BY rtrim(p.created_at,'Z') DESC,p.sequence DESC,c.id").arg(where));
    for (const QVariant &parameter : std::as_const(parameters)) query.addBindValue(parameter);
    query.addBindValue(pageSize + 1);
    query.addBindValue(static_cast<qint64>(std::max(0, filter.page)) * pageSize);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return {};
    }
    HistoryPage page;
    while (query.next()) {
        const QString id = query.value(0).toString();
        if (page.events.isEmpty() || page.events.constLast().id != id) {
            if (page.events.size() == pageSize) {
                page.hasMore = true;
                break;
            }
            page.events.append(HistoryEvent{id, query.value(1).toString(), query.value(2).toString(), {}});
        }
        if (query.value(4).isNull()) continue;
        HistoryChange change;
        change.fieldId = query.value(3).toString();
        change.fieldName = query.value(4).toString();
        change.fieldType = query.value(5).toString();
        change.beforeFieldType = query.value(6).toString();
        if (!query.value(7).isNull()) change.before = parseJsonValue(query.value(7).toString());
        if (!query.value(8).isNull()) change.after = parseJsonValue(query.value(8).toString());
        page.events.last().changes.append(change);
    }
    if (query.lastError().isValid()) {
        if (error) *error = query.lastError().text();
        return {};
    }
    return page;
}

QVector<Goal> GameRepository::goals(const QString &gameId, QString *error) const
{
    if (error) error->clear();
    QVector<Goal> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT gl.id,gl.game_id,gl.title,gl.description,gl.goal_type,COALESCE(gl.object_id,''),"
        "COALESCE(gl.field_id,''),COALESCE(gl.target_json,''),gl.current_value,gl.completed,gl.created_at,gl.updated_at "
        "FROM goals gl JOIN games g ON g.id=gl.game_id "
        "WHERE gl.game_id=? AND g.user_id=? AND g.archived=0 ORDER BY gl.sort_order,gl.created_at,gl.id"));
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    QVector<ObjectRecord> fieldTargetObjects;
    bool fieldTargetObjectsLoaded = false;
    while (query.next()) {
        Goal item;
        item.id = query.value(0).toString();
        item.gameId = query.value(1).toString();
        item.title = query.value(2).toString();
        item.description = query.value(3).toString();
        item.goalType = query.value(4).toString();
        item.objectId = query.value(5).toString();
        item.fieldId = query.value(6).toString();
        item.target = parseJsonValue(query.value(7).toString());
        item.current = query.value(8).toDouble();
        item.completed = query.value(9).toBool();
        item.createdAt = query.value(10).toString();
        item.updatedAt = query.value(11).toString();

        if (item.goalType == QStringLiteral("checklist")) {
            QSqlQuery tasksQuery(m_database);
            tasksQuery.prepare(QStringLiteral("SELECT id,title,completed,sort_order FROM goal_tasks WHERE goal_id=? ORDER BY sort_order,id"));
            tasksQuery.addBindValue(item.id);
            if (!tasksQuery.exec()) {
                if (error) *error = tasksQuery.lastError().text();
                return {};
            }
            while (tasksQuery.next()) {
                item.tasks.append(GoalTask{tasksQuery.value(0).toString(), tasksQuery.value(1).toString(),
                    tasksQuery.value(2).toBool(), tasksQuery.value(3).toInt()});
            }
            int done = 0;
            for (const GoalTask &task : std::as_const(item.tasks)) if (task.completed) ++done;
            if (!item.tasks.isEmpty()) {
                item.progress = static_cast<double>(done) / static_cast<double>(item.tasks.size()) * 100.0;
                item.completed = done == item.tasks.size();
            }
        } else if (item.goalType == QStringLiteral("numeric")) {
            const double target = item.target.toDouble();
            if (target > 0 && std::isfinite(target)) {
                item.progress = std::clamp(item.current / target * 100.0, 0.0, 100.0);
                item.completed = item.current >= target;
            }
        } else if (item.goalType == QStringLiteral("field_target")) {
            const double target = item.target.toDouble();
            if (!fieldTargetObjectsLoaded) {
                fieldTargetObjects = objects(gameId, error);
                fieldTargetObjectsLoaded = true;
                if (error && !error->isEmpty()) return {};
            }
            const auto object = std::find_if(fieldTargetObjects.cbegin(), fieldTargetObjects.cend(), [&](const ObjectRecord &value) { return value.id == item.objectId; });
            if (object != fieldTargetObjects.cend()) {
                const QJsonValue raw = object->values.value(item.fieldId);
                double current = raw.toDouble(std::numeric_limits<double>::quiet_NaN());
                if (raw.isObject()) current = raw.toObject().value(QStringLiteral("current")).toDouble(std::numeric_limits<double>::quiet_NaN());
                if (std::isfinite(current)) {
                    item.current = current;
                    if (target > 0 && std::isfinite(target)) {
                        item.progress = std::clamp(current / target * 100.0, 0.0, 100.0);
                        item.completed = current >= target;
                    }
                }
            }
        } else if (item.goalType == QStringLiteral("manual")) {
            item.progress = item.completed ? 100.0 : 0.0;
        }

        QSqlQuery dependencies(m_database);
        dependencies.prepare(QStringLiteral(
            "SELECT p.id,p.title FROM goal_dependencies d JOIN goals p ON p.id=d.prerequisite_goal_id "
            "WHERE d.goal_id=? ORDER BY p.sort_order,p.created_at,p.id"));
        dependencies.addBindValue(item.id);
        if (!dependencies.exec()) {
            if (error) *error = dependencies.lastError().text();
            return {};
        }
        while (dependencies.next()) {
            item.prerequisiteIds.append(dependencies.value(0).toString());
            item.prerequisiteTitles.append(dependencies.value(1).toString());
        }
        result.append(item);
    }
    QMap<QString, bool> completedById;
    for (const Goal &item : std::as_const(result)) completedById.insert(item.id, item.completed);
    for (Goal &item : result) {
        item.ready = true;
        item.blockedBy.clear();
        for (int index = 0; index < item.prerequisiteIds.size(); ++index) {
            if (completedById.value(item.prerequisiteIds.at(index), false)) continue;
            item.ready = false;
            if (index < item.prerequisiteTitles.size()) item.blockedBy.append(item.prerequisiteTitles.at(index));
        }
    }
    return result;
}

QVector<ArchivedEntry> GameRepository::archivedEntries(QString *error) const
{
    if (error) error->clear();
    QVector<ArchivedEntry> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT g.id,'Game','',g.name FROM games g WHERE g.user_id=? AND g.archived=1 "
        "UNION ALL "
        "SELECT s.id,'Section',g.name,s.name FROM sections s JOIN games g ON g.id=s.game_id "
        "WHERE g.user_id=? AND g.archived=0 AND s.archived=1 "
        "UNION ALL "
        "SELECT o.id,'Object',g.name,o.name FROM objects o JOIN games g ON g.id=o.game_id "
        "WHERE g.user_id=? AND g.archived=0 AND o.archived=1 "
        "ORDER BY 2,3 COLLATE NOCASE,4 COLLATE NOCASE"));
    query.addBindValue(m_userId);
    query.addBindValue(m_userId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) result.append(ArchivedEntry{
        query.value(0).toString(), query.value(1).toString(), query.value(2).toString(), query.value(3).toString()
    });
    return result;
}

bool GameRepository::saveGoal(Goal &goal, const QVector<GoalTask> &tasks, QString *error)
{
    if (error) error->clear();
    goal.title = nonNullTrimmed(goal.title);
    goal.description = nonNullTrimmed(goal.description);
    if (goal.gameId.isEmpty() || goal.title.isEmpty() || goal.title.size() > 160) {
        if (error) *error = QStringLiteral("A game and a goal title of 1–160 characters are required.");
        return false;
    }
    static const QStringList supportedTypes = {QStringLiteral("manual"), QStringLiteral("checklist"),
        QStringLiteral("numeric"), QStringLiteral("field_target")};
    if (!supportedTypes.contains(goal.goalType)) {
        if (error) *error = QStringLiteral("Choose a supported goal type.");
        return false;
    }
    if (goal.goalType == QStringLiteral("numeric") && (!goal.target.isDouble() || !std::isfinite(goal.target.toDouble()) ||
        goal.target.toDouble() <= 0 || !std::isfinite(goal.current) || goal.current < 0)) {
        if (error) *error = QStringLiteral("Numeric goals need a positive target and nonnegative finite progress.");
        return false;
    }
    if (goal.goalType == QStringLiteral("checklist") && (tasks.isEmpty() || tasks.size() > 200)) {
        if (error) *error = QStringLiteral("Checklist goals need between 1 and 200 tasks.");
        return false;
    }
    QSet<QString> suppliedTaskIds;
    for (const GoalTask &task : tasks) {
        const QString title = task.title.trimmed();
        if (title.isEmpty() || title.size() > 160 || (!task.id.isEmpty() && suppliedTaskIds.contains(task.id))) {
            if (error) *error = QStringLiteral("Checklist task titles must contain 1–160 characters and task IDs must be unique.");
            return false;
        }
        if (!task.id.isEmpty()) suppliedTaskIds.insert(task.id);
    }
    if (goal.goalType == QStringLiteral("field_target")) {
        if (goal.objectId.isEmpty() || goal.fieldId.isEmpty() || !goal.target.isDouble() ||
            !std::isfinite(goal.target.toDouble()) || goal.target.toDouble() <= 0) {
            if (error) *error = QStringLiteral("Field targets need an active object, a numeric field, and a positive target.");
            return false;
        }
        const QVector<ObjectRecord> activeObjects = objects(goal.gameId, error);
        const auto object = std::find_if(activeObjects.cbegin(), activeObjects.cend(), [&](const ObjectRecord &value) { return value.id == goal.objectId; });
        if (object == activeObjects.cend()) {
            if (error && error->isEmpty()) *error = QStringLiteral("Choose an active object in this game.");
            return false;
        }
        const QVector<ObjectType> types = objectTypes(goal.gameId, error);
        const auto type = std::find_if(types.cbegin(), types.cend(), [&](const ObjectType &value) { return value.id == object->typeId; });
        const auto field = type == types.cend() ? QVector<FieldDefinition>::const_iterator() :
            std::find_if(type->fields.cbegin(), type->fields.cend(), [&](const FieldDefinition &value) { return value.id == goal.fieldId; });
        if (type == types.cend() || field == type->fields.cend() || field->archived ||
            !(field->fieldType == QStringLiteral("integer") || field->fieldType == QStringLiteral("decimal") ||
              field->fieldType == QStringLiteral("percentage") || field->fieldType == QStringLiteral("progress"))) {
            if (error && error->isEmpty()) *error = QStringLiteral("Choose an active numeric field on the selected object.");
            return false;
        }
    } else {
        goal.objectId.clear();
        goal.fieldId.clear();
    }
    if (goal.goalType != QStringLiteral("numeric")) goal.current = 0;
    if (goal.goalType != QStringLiteral("manual")) goal.completed = false;
    if (goal.goalType != QStringLiteral("checklist")) goal.tasks.clear();

    QStringList prerequisiteIds;
    QSet<QString> uniquePrerequisites;
    for (const QString &id : std::as_const(goal.prerequisiteIds)) {
        if (id.isEmpty() || uniquePrerequisites.contains(id)) continue;
        uniquePrerequisites.insert(id);
        prerequisiteIds.append(id);
    }
    std::sort(prerequisiteIds.begin(), prerequisiteIds.end());
    if (prerequisiteIds.size() > 50) {
        if (error) *error = QStringLiteral("A goal can have at most 50 prerequisites.");
        return false;
    }
    const bool isNew = goal.id.isEmpty();
    const QString goalId = isNew ? uuid() : goal.id;
    if (prerequisiteIds.contains(goalId)) {
        if (error) *error = QStringLiteral("A goal cannot depend on itself.");
        return false;
    }

    const QVector<Goal> existingGoals = goals(goal.gameId, error);
    if (error && !error->isEmpty()) return false;
    QMap<QString, Goal> existingById;
    QMap<QString, QStringList> graph;
    for (const Goal &item : existingGoals) {
        existingById.insert(item.id, item);
        graph.insert(item.id, item.prerequisiteIds);
    }
    if (!isNew && (!existingById.contains(goalId))) {
        if (error) *error = QStringLiteral("The goal is no longer available.");
        return false;
    }
    for (const QString &id : std::as_const(prerequisiteIds)) {
        if (!existingById.contains(id) || existingById.value(id).gameId != goal.gameId) {
            if (error) *error = QStringLiteral("Prerequisites must be other goals in the same game.");
            return false;
        }
        if (goal.completed && !existingById.value(id).completed) {
            if (error) *error = QStringLiteral("Complete prerequisite goals before marking this goal complete.");
            return false;
        }
    }
    graph.insert(goalId, prerequisiteIds);
    QMap<QString, int> state;
    std::function<bool(const QString &)> visitsCycle = [&](const QString &id) {
        if (state.value(id) == 1) return true;
        if (state.value(id) == 2) return false;
        state[id] = 1;
        for (const QString &dependency : graph.value(id)) if (visitsCycle(dependency)) return true;
        state[id] = 2;
        return false;
    };
    for (auto it = graph.cbegin(); it != graph.cend(); ++it) {
        if (visitsCycle(it.key())) {
            if (error) *error = QStringLiteral("These prerequisites would create a dependency cycle.");
            return false;
        }
    }

    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    const QString timestamp = nowUtc();
    const QVariant target = goal.target.isUndefined() || goal.target.isNull() ||
        (goal.goalType != QStringLiteral("numeric") && goal.goalType != QStringLiteral("field_target"))
        ? QVariant() : QVariant(QString::fromUtf8(compactJson(goal.target)));
    QSqlQuery save(m_database);
    if (isNew) {
        save.prepare(QStringLiteral(
            "INSERT INTO goals(id,game_id,title,description,goal_type,object_id,field_id,target_json,current_value,completed,sort_order,created_at,updated_at) "
            "SELECT ?,g.id,?,?,?,?,?,?,?,?,(SELECT COALESCE(MAX(sort_order),-1)+1 FROM goals WHERE game_id=g.id),?,? "
            "FROM games g WHERE g.id=? AND g.user_id=? AND g.archived=0"));
        save.addBindValue(goalId);
        save.addBindValue(goal.title);
        save.addBindValue(goal.description);
        save.addBindValue(goal.goalType);
        save.addBindValue(goal.objectId.isEmpty() ? QVariant() : QVariant(goal.objectId));
        save.addBindValue(goal.fieldId.isEmpty() ? QVariant() : QVariant(goal.fieldId));
        save.addBindValue(target);
        save.addBindValue(goal.current);
        save.addBindValue(goal.goalType == QStringLiteral("manual") && goal.completed ? 1 : 0);
        save.addBindValue(timestamp);
        save.addBindValue(timestamp);
        save.addBindValue(goal.gameId);
        save.addBindValue(m_userId);
    } else {
        save.prepare(QStringLiteral(
            "UPDATE goals SET title=?,description=?,goal_type=?,object_id=?,field_id=?,target_json=?,current_value=?,completed=?,updated_at=? "
            "WHERE id=? AND game_id=? AND EXISTS(SELECT 1 FROM games g WHERE g.id=goals.game_id AND g.user_id=? AND g.archived=0)"));
        save.addBindValue(goal.title);
        save.addBindValue(goal.description);
        save.addBindValue(goal.goalType);
        save.addBindValue(goal.objectId.isEmpty() ? QVariant() : QVariant(goal.objectId));
        save.addBindValue(goal.fieldId.isEmpty() ? QVariant() : QVariant(goal.fieldId));
        save.addBindValue(target);
        save.addBindValue(goal.current);
        save.addBindValue(goal.goalType == QStringLiteral("manual") && goal.completed ? 1 : 0);
        save.addBindValue(timestamp);
        save.addBindValue(goalId);
        save.addBindValue(goal.gameId);
        save.addBindValue(m_userId);
    }
    if (!save.exec() || save.numRowsAffected() != 1) {
        const QString detail = save.lastError().isValid() ? save.lastError().text() : QStringLiteral("The game or goal is unavailable.");
        m_database.rollback();
        if (error) *error = detail;
        return false;
    }

    QSet<QString> keptTaskIds;
    if (goal.goalType == QStringLiteral("checklist")) {
        QSet<QString> existingTaskIds;
        if (!isNew) {
            QSqlQuery existingTasks(m_database);
            existingTasks.prepare(QStringLiteral("SELECT id FROM goal_tasks WHERE goal_id=?"));
            existingTasks.addBindValue(goalId);
            if (!existingTasks.exec()) {
                m_database.rollback();
                if (error) *error = existingTasks.lastError().text();
                return false;
            }
            while (existingTasks.next()) existingTaskIds.insert(existingTasks.value(0).toString());
        }
        for (int index = 0; index < tasks.size(); ++index) {
            const GoalTask &task = tasks.at(index);
            if (!task.id.isEmpty() && !existingTaskIds.contains(task.id)) {
                m_database.rollback();
                if (error) *error = QStringLiteral("A checklist task does not belong to this goal.");
                return false;
            }
            const QString taskId = task.id.isEmpty() ? uuid() : task.id;
            QSqlQuery saveTask(m_database);
            if (task.id.isEmpty()) {
                saveTask.prepare(QStringLiteral("INSERT INTO goal_tasks(id,goal_id,title,completed,sort_order,created_at) VALUES(?,?,?,?,?,?)"));
                saveTask.addBindValue(taskId);
                saveTask.addBindValue(goalId);
                saveTask.addBindValue(task.title.trimmed());
                saveTask.addBindValue(task.completed ? 1 : 0);
                saveTask.addBindValue(index);
                saveTask.addBindValue(timestamp);
            } else {
                saveTask.prepare(QStringLiteral("UPDATE goal_tasks SET title=?,completed=?,sort_order=? WHERE id=? AND goal_id=?"));
                saveTask.addBindValue(task.title.trimmed());
                saveTask.addBindValue(task.completed ? 1 : 0);
                saveTask.addBindValue(index);
                saveTask.addBindValue(taskId);
                saveTask.addBindValue(goalId);
            }
            if (!saveTask.exec()) {
                m_database.rollback();
                if (error) *error = saveTask.lastError().text();
                return false;
            }
            if (!task.id.isEmpty()) keptTaskIds.insert(taskId);
        }
        for (const QString &oldId : std::as_const(existingTaskIds)) {
            if (keptTaskIds.contains(oldId)) continue;
            QSqlQuery removeTask(m_database);
            removeTask.prepare(QStringLiteral("DELETE FROM goal_tasks WHERE id=? AND goal_id=?"));
            removeTask.addBindValue(oldId);
            removeTask.addBindValue(goalId);
            if (!removeTask.exec()) {
                m_database.rollback();
                if (error) *error = removeTask.lastError().text();
                return false;
            }
        }
    } else {
        QSqlQuery removeTasks(m_database);
        removeTasks.prepare(QStringLiteral("DELETE FROM goal_tasks WHERE goal_id=?"));
        removeTasks.addBindValue(goalId);
        if (!removeTasks.exec()) {
            m_database.rollback();
            if (error) *error = removeTasks.lastError().text();
            return false;
        }
    }
    QSqlQuery removeDependencies(m_database);
    removeDependencies.prepare(QStringLiteral("DELETE FROM goal_dependencies WHERE goal_id=?"));
    removeDependencies.addBindValue(goalId);
    if (!removeDependencies.exec()) {
        m_database.rollback();
        if (error) *error = removeDependencies.lastError().text();
        return false;
    }
    for (const QString &prerequisiteId : std::as_const(prerequisiteIds)) {
        QSqlQuery addDependency(m_database);
        addDependency.prepare(QStringLiteral("INSERT INTO goal_dependencies(game_id,goal_id,prerequisite_goal_id,created_at) VALUES(?,?,?,?)"));
        addDependency.addBindValue(goal.gameId);
        addDependency.addBindValue(goalId);
        addDependency.addBindValue(prerequisiteId);
        addDependency.addBindValue(timestamp);
        if (!addDependency.exec()) {
            m_database.rollback();
            if (error) *error = addDependency.lastError().text();
            return false;
        }
    }
    if (!m_database.commit()) {
        if (error) *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    goal.id = goalId;
    goal.prerequisiteIds = prerequisiteIds;
    return true;
}

bool GameRepository::setGoalDependencies(const QString &goalId, const QStringList &prerequisiteIds, QString *error)
{
    if (error) error->clear();
    QVector<Goal> items;
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT game_id FROM goals WHERE id=? AND EXISTS(SELECT 1 FROM games g WHERE g.id=goals.game_id AND g.user_id=?)"));
    lookup.addBindValue(goalId);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The goal is no longer available.");
        return false;
    }
    const QString gameId = lookup.value(0).toString();
    items = goals(gameId, error);
    auto found = std::find_if(items.begin(), items.end(), [&](const Goal &item) { return item.id == goalId; });
    if (found == items.end()) {
        if (error && error->isEmpty()) *error = QStringLiteral("The goal is no longer available.");
        return false;
    }
    found->prerequisiteIds = prerequisiteIds;
    return saveGoal(*found, found->tasks, error);
}

bool GameRepository::setGoalCompleted(const QString &goalId, bool completed, QString *error)
{
    if (error) error->clear();
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT game_id,goal_type FROM goals WHERE id=? AND EXISTS(SELECT 1 FROM games g WHERE g.id=goals.game_id AND g.user_id=?)"));
    lookup.addBindValue(goalId);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The goal is no longer available.");
        return false;
    }
    if (lookup.value(1).toString() != QStringLiteral("manual")) {
        if (error) *error = QStringLiteral("Only manual goals can be marked complete directly.");
        return false;
    }
    if (completed) {
        const QVector<Goal> items = goals(lookup.value(0).toString(), error);
        const auto item = std::find_if(items.cbegin(), items.cend(), [&](const Goal &goal) { return goal.id == goalId; });
        if (item == items.cend() || !item->ready) {
            if (error && error->isEmpty()) *error = QStringLiteral("Complete prerequisite goals before completing this goal.");
            return false;
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE goals SET completed=?,updated_at=? WHERE id=? AND goal_type='manual'"));
    update.addBindValue(completed ? 1 : 0);
    update.addBindValue(nowUtc());
    update.addBindValue(goalId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (error) *error = update.lastError().text();
        return false;
    }
    return true;
}

bool GameRepository::setGoalTaskCompleted(const QString &taskId, bool completed, QString *error)
{
    if (error) error->clear();
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral(
        "SELECT t.goal_id,gl.game_id,gl.goal_type FROM goal_tasks t JOIN goals gl ON gl.id=t.goal_id "
        "JOIN games g ON g.id=gl.game_id WHERE t.id=:task_id AND g.user_id=:user_id"));
    lookup.bindValue(QStringLiteral(":task_id"), taskId);
    lookup.bindValue(QStringLiteral(":user_id"), m_userId);
    if (!lookup.exec() || !lookup.next()) {
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The checklist task is no longer available.");
        return false;
    }
    const QString goalId = lookup.value(0).toString();
    const QString gameId = lookup.value(1).toString();
    if (lookup.value(2).toString() != QStringLiteral("checklist")) {
        if (error) *error = QStringLiteral("Tasks can only be updated on checklist goals.");
        return false;
    }
    if (completed) {
        QSqlQuery remaining(m_database);
        remaining.prepare(QStringLiteral("SELECT COUNT(*) FROM goal_tasks WHERE goal_id=? AND id<>? AND completed=0"));
        remaining.addBindValue(goalId);
        remaining.addBindValue(taskId);
        if (!remaining.exec() || !remaining.next()) {
            if (error) *error = QStringLiteral("Could not check remaining checklist tasks: %1").arg(remaining.lastError().text());
            return false;
        }
        if (remaining.value(0).toInt() == 0) {
            const QVector<Goal> items = goals(gameId, error);
            const auto item = std::find_if(items.cbegin(), items.cend(), [&](const Goal &goal) { return goal.id == goalId; });
            if (item == items.cend() || !item->ready) {
                if (error && error->isEmpty()) *error = QStringLiteral("Complete prerequisite goals before completing this checklist.");
                return false;
            }
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE goal_tasks SET completed=? WHERE id=?"));
    update.addBindValue(completed ? 1 : 0);
    update.addBindValue(taskId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (error) *error = QStringLiteral("Could not update checklist task: %1").arg(update.lastError().text());
        return false;
    }
    return true;
}

bool GameRepository::setNumericGoalCurrent(const QString &goalId, double current, QString *error)
{
    if (error) error->clear();
    if (!std::isfinite(current) || current < 0) {
        if (error) *error = QStringLiteral("Current progress must be a nonnegative finite number.");
        return false;
    }
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT game_id,target_json FROM goals WHERE id=? AND goal_type='numeric' AND EXISTS(SELECT 1 FROM games g WHERE g.id=goals.game_id AND g.user_id=?)"));
    lookup.addBindValue(goalId);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The numeric goal is no longer available.");
        return false;
    }
    const double target = parseJsonValue(lookup.value(1).toString()).toDouble();
    if (current >= target) {
        const QVector<Goal> items = goals(lookup.value(0).toString(), error);
        const auto item = std::find_if(items.cbegin(), items.cend(), [&](const Goal &goal) { return goal.id == goalId; });
        if (item == items.cend() || !item->ready) {
            if (error && error->isEmpty()) *error = QStringLiteral("Complete prerequisite goals before reaching this goal's target.");
            return false;
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE goals SET current_value=?,updated_at=? WHERE id=? AND goal_type='numeric'"));
    update.addBindValue(current);
    update.addBindValue(nowUtc());
    update.addBindValue(goalId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (error) *error = update.lastError().text();
        return false;
    }
    return true;
}

bool GameRepository::deleteGoal(const QString &goalId, QString *error)
{
    if (error) error->clear();
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM goals WHERE id=? AND EXISTS(SELECT 1 FROM games g WHERE g.id=goals.game_id AND g.user_id=?)"));
    remove.addBindValue(goalId);
    remove.addBindValue(m_userId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        if (error) *error = remove.lastError().isValid() ? remove.lastError().text() : QStringLiteral("The goal is no longer available.");
        return false;
    }
    return true;
}

bool GameRepository::createGame(const QString &name, const QString &description, const QString &icon,
                                QString *createdId, QString *error, const QString &personalNotes,
                                const QString &coverImage)
{
    if (name.trimmed().isEmpty() || name.trimmed().size() > 120) {
        if (error) *error = QStringLiteral("A game name is required.");
        return false;
    }
    const QString id = uuid();
    const QString timestamp = nowUtc();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO games(id,user_id,name,description,icon,cover_image,personal_notes,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(id);
    query.addBindValue(m_userId);
    query.addBindValue(nonNullTrimmed(name));
    query.addBindValue(nonNullTrimmed(description));
    query.addBindValue(icon.isEmpty() ? QStringLiteral("🎮") : icon);
    query.addBindValue(nonNullTrimmed(coverImage));
    query.addBindValue(nonNullTrimmed(personalNotes));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    if (createdId) *createdId = id;
    return true;
}

bool GameRepository::updateGame(const Game &game, QString *error)
{
    if (game.name.trimmed().isEmpty() || game.name.trimmed().size() > 120) {
        if (error) *error = QStringLiteral("Game name must be between 1 and 120 characters.");
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE games SET name=?, description=?, icon=?, cover_image=?, personal_notes=?, updated_at=? WHERE id=? AND user_id=? AND archived=0"));
    query.addBindValue(nonNullTrimmed(game.name));
    query.addBindValue(nonNullTrimmed(game.description));
    query.addBindValue(game.icon);
    query.addBindValue(nonNullTrimmed(game.coverImage));
    query.addBindValue(nonNullTrimmed(game.personalNotes));
    query.addBindValue(nowUtc());
    query.addBindValue(game.id);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The game no longer exists.");
        return false;
    }
    return true;
}

bool GameRepository::archiveGame(const QString &gameId, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE games SET archived=1, updated_at=? WHERE id=? AND user_id=? AND archived=0"));
    query.addBindValue(nowUtc());
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool GameRepository::archiveSection(const QString &sectionId, QString *error)
{
    if (error) error->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE sections SET archived=1,updated_at=? WHERE id=? AND archived=0 "
        "AND EXISTS(SELECT 1 FROM games g WHERE g.id=sections.game_id AND g.user_id=? AND g.archived=0)"));
    query.addBindValue(nowUtc());
    query.addBindValue(sectionId);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The section is no longer available.");
        return false;
    }
    return true;
}

bool GameRepository::restoreArchivedEntry(const ArchivedEntry &entry, QString *error)
{
    if (error) error->clear();
    if (entry.id.isEmpty()) {
        if (error) *error = QStringLiteral("The archived item is unavailable.");
        return false;
    }
    if (entry.kind == QStringLiteral("Game")) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("UPDATE games SET archived=0,updated_at=? WHERE id=? AND user_id=? AND archived=1"));
        query.addBindValue(nowUtc());
        query.addBindValue(entry.id);
        query.addBindValue(m_userId);
        if (!query.exec() || query.numRowsAffected() != 1) {
            if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The archived game is no longer available.");
            return false;
        }
        return true;
    }
    if (entry.kind == QStringLiteral("Section")) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "UPDATE sections SET archived=0,updated_at=? WHERE id=? AND archived=1 "
            "AND EXISTS(SELECT 1 FROM games g WHERE g.id=sections.game_id AND g.user_id=? AND g.archived=0)"));
        query.addBindValue(nowUtc());
        query.addBindValue(entry.id);
        query.addBindValue(m_userId);
        if (!query.exec() || query.numRowsAffected() != 1) {
            if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The archived section is no longer available.");
            return false;
        }
        return true;
    }
    if (entry.kind != QStringLiteral("Object")) {
        if (error) *error = QStringLiteral("This kind of archived item cannot be restored here.");
        return false;
    }
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral(
        "SELECT o.game_id,o.name FROM objects o JOIN games g ON g.id=o.game_id "
        "WHERE o.id=? AND o.archived=1 AND g.user_id=? AND g.archived=0"));
    lookup.addBindValue(entry.id);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        m_database.rollback();
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The archived object is no longer available.");
        return false;
    }
    const QString gameId = lookup.value(0).toString();
    const QString name = lookup.value(1).toString();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE objects SET archived=0,updated_at=? WHERE id=? AND archived=1"));
    update.addBindValue(nowUtc());
    update.addBindValue(entry.id);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_database.rollback();
        if (error) *error = update.lastError().text();
        return false;
    }
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) "
        "VALUES(?,?,?,?,?,'restored','api',?)"));
    event.addBindValue(historyEventId());
    event.addBindValue(gameId);
    event.addBindValue(entry.id);
    event.addBindValue(name);
    event.addBindValue(m_userId);
    event.addBindValue(nowUtc());
    if (!event.exec() || !m_database.commit()) {
        if (error) *error = event.lastError().isValid() ? event.lastError().text() : m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

bool GameRepository::createSection(const QString &gameId, const QString &parentId, const QString &name,
                                  const QString &icon, QString *error)
{
    if (error) error->clear();
    if (name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Enter a section name.");
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO sections(id,game_id,parent_id,name,icon,sort_order,created_at,updated_at) "
        "SELECT ?,g.id,?,?,?,?,?,? FROM games g WHERE g.id=? AND g.user_id=? AND g.archived=0 "
        "AND (? IS NULL OR EXISTS(SELECT 1 FROM sections p WHERE p.id=? AND p.game_id=g.id AND p.archived=0))"));
    const QString id = uuid();
    query.addBindValue(id);
    query.addBindValue(parentId.isEmpty() ? QVariant() : QVariant(parentId));
    query.addBindValue(name.trimmed());
    query.addBindValue(icon.isEmpty() ? QStringLiteral("✦") : icon);
    query.addBindValue(0);
    query.addBindValue(nowUtc());
    query.addBindValue(nowUtc());
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    query.addBindValue(parentId.isEmpty() ? QVariant() : QVariant(parentId));
    query.addBindValue(parentId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The game or parent section is unavailable.");
        return false;
    }
    return true;
}

bool GameRepository::createObjectType(const QString &gameId, const QString &name, const QString &description,
                                      const QString &icon, QString *createdId, QString *error,
                                      const QString &image)
{
    const QString typeName = nonNullTrimmed(name);
    if (typeName.isEmpty() || typeName.size() > 80) {
        if (error) *error = QStringLiteral("Object type name must be between 1 and 80 characters.");
        return false;
    }
    const QString id = uuid();
    const QString timestamp = nowUtc();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO object_types(id,game_id,name,description,icon,image,created_at,updated_at) "
        "SELECT ?,g.id,?,?,?,?,?,? FROM games g WHERE g.id=? AND g.user_id=? AND g.archived=0"));
    query.addBindValue(id);
    query.addBindValue(typeName);
    query.addBindValue(nonNullTrimmed(description));
    query.addBindValue(icon.isEmpty() ? QStringLiteral("◈") : icon);
    query.addBindValue(nonNullTrimmed(image));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    query.addBindValue(gameId);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The selected game is unavailable.");
        return false;
    }
    if (createdId) *createdId = id;
    return true;
}

bool GameRepository::createField(const QString &typeId, const QString &name, const QString &fieldType,
                                 const QString &icon, const QJsonObject &options, QString *createdId, QString *error,
                                 const QString &description, const QJsonValue &defaultValue)
{
    static const QStringList allowed = {QStringLiteral("text"), QStringLiteral("long_text"), QStringLiteral("integer"),
        QStringLiteral("decimal"), QStringLiteral("percentage"), QStringLiteral("boolean"), QStringLiteral("select"),
        QStringLiteral("date"), QStringLiteral("progress"), QStringLiteral("reference"), QStringLiteral("references"),
        QStringLiteral("formula")};
    if (!allowed.contains(fieldType)) {
        if (error) *error = QStringLiteral("Choose one of the supported field types.");
        return false;
    }
    const QString fieldName = nonNullTrimmed(name);
    if (fieldName.isEmpty() || fieldName.size() > 80) {
        if (error) *error = QStringLiteral("Field name must be between 1 and 80 characters.");
        return false;
    }
    if (fieldType == QStringLiteral("select")) {
        const QJsonValue rawChoices = options.value(QStringLiteral("choices"));
        const QJsonArray choices = rawChoices.toArray();
        if (!rawChoices.isArray() || choices.isEmpty() || choices.size() > 200) {
            if (error) *error = QStringLiteral("Select fields need between 1 and 200 choices.");
            return false;
        }
        QSet<QString> uniqueChoices;
        for (const QJsonValue &choice : choices) {
            const QString text = choice.toString();
            if (!choice.isString() || text.trimmed().isEmpty() || text.size() > 100 || uniqueChoices.contains(text)) {
                if (error) *error = QStringLiteral("Select choices must be unique, nonempty text values up to 100 characters.");
                return false;
            }
            uniqueChoices.insert(text);
        }
    }
    if (fieldType == QStringLiteral("formula")) {
        const QString expression = options.value(QStringLiteral("expression")).toString();
        QString syntaxError;
        if (expression.trimmed().isEmpty() || !FormulaEvaluator::validateExpression(expression, &syntaxError)) {
            if (error) *error = syntaxError.isEmpty() ? QStringLiteral("Formula fields need an expression.") : syntaxError;
            return false;
        }
        const QStringList refs = FormulaEvaluator::references(expression);
        if (refs.isEmpty()) {
            if (error) *error = QStringLiteral("Formula must reference at least one numeric field.");
            return false;
        }
        QString gameId;
        QSqlQuery owner(m_database);
        owner.prepare(QStringLiteral("SELECT game_id FROM object_types WHERE id=? AND archived=0"));
        owner.addBindValue(typeId);
        if (!owner.exec() || !owner.next()) {
            if (error) *error = QStringLiteral("The selected object type is unavailable.");
            return false;
        }
        gameId = owner.value(0).toString();
        const QJsonObject aggregation = options.value(QStringLiteral("aggregation")).toObject();
        QString relationshipFieldId;
        QString aggregatedFieldId;
        QString operation;
        if (!aggregation.isEmpty()) {
            relationshipFieldId = aggregation.value(QStringLiteral("relationship_field_id")).toString();
            aggregatedFieldId = aggregation.value(QStringLiteral("field_id")).toString();
            operation = aggregation.value(QStringLiteral("operation")).toString();
            if (!QStringList{QStringLiteral("sum"), QStringLiteral("avg"), QStringLiteral("min"), QStringLiteral("max"), QStringLiteral("count")}.contains(operation)) {
                if (error) *error = QStringLiteral("Choose sum, avg, min, max, or count for the relationship aggregation.");
                return false;
            }
            QSqlQuery relation(m_database);
            relation.prepare(QStringLiteral("SELECT field_type,options_json FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0"));
            relation.addBindValue(relationshipFieldId);
            relation.addBindValue(typeId);
            if (!relation.exec() || !relation.next() ||
                (relation.value(0).toString() != QStringLiteral("reference") && relation.value(0).toString() != QStringLiteral("references"))) {
                if (error) *error = QStringLiteral("Choose an active relationship field on this type.");
                return false;
            }
            if (operation != QStringLiteral("count")) {
                QSqlQuery aggregateField(m_database);
                aggregateField.prepare(QStringLiteral(
                    "SELECT f.field_type,t.id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id "
                    "WHERE f.id=? AND f.archived=0 AND t.game_id=? AND t.archived=0"));
                aggregateField.addBindValue(aggregatedFieldId);
                aggregateField.addBindValue(gameId);
                if (!aggregateField.exec() || !aggregateField.next() ||
                    !QStringList{QStringLiteral("integer"), QStringLiteral("decimal"), QStringLiteral("percentage"), QStringLiteral("progress")}.contains(aggregateField.value(0).toString())) {
                    if (error) *error = QStringLiteral("Choose a numeric or progress field on a type in this game.");
                    return false;
                }
                const QJsonObject relationOptions = QJsonDocument::fromJson(relation.value(1).toByteArray()).object();
                const QString allowedType = relationOptions.value(QStringLiteral("allowed_type_id")).toString();
                if (!allowedType.isEmpty() && allowedType != aggregateField.value(1).toString()) {
                    if (error) *error = QStringLiteral("The aggregated field must belong to the allowed target type.");
                    return false;
                }
            }
        }
        for (const QString &ref : refs) {
            if (ref == QStringLiteral("related") && !aggregation.isEmpty()) continue;
            QSqlQuery dependency(m_database);
            dependency.prepare(QStringLiteral("SELECT field_type FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0"));
            dependency.addBindValue(ref);
            dependency.addBindValue(typeId);
            if (!dependency.exec() || !dependency.next() ||
                !QStringList{QStringLiteral("integer"), QStringLiteral("decimal"), QStringLiteral("percentage"), QStringLiteral("progress"), QStringLiteral("formula")}.contains(dependency.value(0).toString())) {
                if (error) *error = QStringLiteral("Formulas can reference numeric or formula fields on this object type only.");
                return false;
            }
        }
    }
    if (!validateDefaultValue(fieldType, options, defaultValue, error)) return false;
    QString allowedTypeId;
    if (fieldType == QStringLiteral("reference") || fieldType == QStringLiteral("references")) {
        const QJsonValue allowedType = options.value(QStringLiteral("allowed_type_id"));
        if (!allowedType.isUndefined() && !allowedType.isString()) {
            if (error) *error = QStringLiteral("Allowed reference type must be an object type ID.");
            return false;
        }
        allowedTypeId = allowedType.toString();
        if (!allowedTypeId.isEmpty()) {
            QSqlQuery targetType(m_database);
            targetType.prepare(QStringLiteral(
                "SELECT 1 FROM object_types target JOIN object_types source ON source.game_id=target.game_id "
                "JOIN games g ON g.id=source.game_id WHERE source.id=? AND target.id=? "
                "AND source.archived=0 AND target.archived=0 AND g.user_id=? AND g.archived=0"));
            targetType.addBindValue(typeId);
            targetType.addBindValue(allowedTypeId);
            targetType.addBindValue(m_userId);
            if (!targetType.exec() || !targetType.next()) {
                if (error) *error = QStringLiteral("Allowed target type must be active and belong to the same game.");
                return false;
            }
        }
    }
    const QString id = uuid();
    const QString timestamp = nowUtc();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO field_definitions(id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,created_at,updated_at) "
        "SELECT ?,t.id,?,?,?,?,?,?,(SELECT COALESCE(MAX(sort_order),-1)+1 FROM field_definitions WHERE object_type_id=t.id),?,? "
        "FROM object_types t JOIN games g ON g.id=t.game_id WHERE t.id=? AND t.archived=0 AND g.user_id=? AND g.archived=0"));
    query.addBindValue(id);
    query.addBindValue(fieldName);
    query.addBindValue(fieldType);
    query.addBindValue(icon.isEmpty() ? QStringLiteral("✦") : icon);
    query.addBindValue(nonNullTrimmed(description));
    query.addBindValue(defaultValue.isUndefined() || defaultValue.isNull()
        ? QVariant() : QVariant(QString::fromUtf8(compactJson(defaultValue))));
    query.addBindValue(QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    query.addBindValue(typeId);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The selected object type is unavailable.");
        return false;
    }
    if (createdId) *createdId = id;
    return true;
}

bool GameRepository::saveObject(ObjectRecord &object, const QVector<FieldDefinition> &fields,
                                const QString &sectionId, QString *error)
{
    if (error) error->clear();
    if (object.gameId.isEmpty() || object.typeId.isEmpty() || object.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("A game, object type, and object name are required.");
        return false;
    }
    const bool isNew = object.id.isEmpty();
    const QString objectId = isNew ? uuid() : object.id;
    const QString timestamp = nowUtc();
    QString previousName;
    QString previousDescription;
    QString previousImage;
    QMap<QString, QByteArray> beforeValues;
    QMap<QString, QStringList> beforeReferences;
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    if (!isNew) {
        QSqlQuery previous(m_database);
        previous.prepare(QStringLiteral(
            "SELECT o.name,o.description,o.image FROM objects o JOIN games g ON g.id=o.game_id "
            "WHERE o.id=? AND o.game_id=? AND g.user_id=? AND g.archived=0 AND o.archived=0"));
        previous.addBindValue(objectId);
        previous.addBindValue(object.gameId);
        previous.addBindValue(m_userId);
        if (!previous.exec() || !previous.next()) {
            if (error) *error = previous.lastError().isValid() ? previous.lastError().text() : QStringLiteral("The object is no longer available.");
            m_database.rollback();
            return false;
        }
        previousName = previous.value(0).toString();
        previousDescription = previous.value(1).toString();
        previousImage = previous.value(2).toString();
        QSqlQuery oldValues(m_database);
        oldValues.prepare(QStringLiteral("SELECT field_id,value_json FROM field_values WHERE object_id=?"));
        oldValues.addBindValue(objectId);
        if (!oldValues.exec()) {
            if (error) *error = oldValues.lastError().text();
            m_database.rollback();
            return false;
        }
        while (oldValues.next()) beforeValues.insert(oldValues.value(0).toString(), oldValues.value(1).toString().toUtf8());
        QSqlQuery oldReferences(m_database);
        oldReferences.prepare(QStringLiteral("SELECT field_id,referenced_object_id FROM object_relationships WHERE object_id=? ORDER BY referenced_object_id"));
        oldReferences.addBindValue(objectId);
        if (!oldReferences.exec()) {
            if (error) *error = oldReferences.lastError().text();
            m_database.rollback();
            return false;
        }
        while (oldReferences.next()) beforeReferences[oldReferences.value(0).toString()].append(oldReferences.value(1).toString());
    }

    QSqlQuery objectQuery(m_database);
    if (isNew) {
        objectQuery.prepare(QStringLiteral(
            "INSERT INTO objects(id,game_id,object_type_id,name,description,image,created_at,updated_at) "
            "SELECT ?,g.id,t.id,?,?,?,?,? FROM games g JOIN object_types t ON t.game_id=g.id "
            "WHERE g.id=? AND t.id=? AND g.user_id=? AND g.archived=0 AND t.archived=0"));
        objectQuery.addBindValue(objectId);
        objectQuery.addBindValue(nonNullTrimmed(object.name));
        objectQuery.addBindValue(nonNullTrimmed(object.description));
        objectQuery.addBindValue(object.image.isNull() ? QStringLiteral("") : object.image);
        objectQuery.addBindValue(timestamp);
        objectQuery.addBindValue(timestamp);
        objectQuery.addBindValue(object.gameId);
        objectQuery.addBindValue(object.typeId);
        objectQuery.addBindValue(m_userId);
    } else {
        objectQuery.prepare(QStringLiteral(
            "UPDATE objects SET name=?,description=?,image=?,updated_at=? WHERE id=? AND game_id=? AND object_type_id=? AND archived=0 "
            "AND EXISTS(SELECT 1 FROM games g WHERE g.id=objects.game_id AND g.user_id=?)"));
        objectQuery.addBindValue(nonNullTrimmed(object.name));
        objectQuery.addBindValue(nonNullTrimmed(object.description));
        objectQuery.addBindValue(object.image.isNull() ? QStringLiteral("") : object.image);
        objectQuery.addBindValue(timestamp);
        objectQuery.addBindValue(objectId);
        objectQuery.addBindValue(object.gameId);
        objectQuery.addBindValue(object.typeId);
        objectQuery.addBindValue(m_userId);
    }
    if (!objectQuery.exec() || objectQuery.numRowsAffected() != 1) {
        const QString detail = objectQuery.lastError().isValid() ? objectQuery.lastError().text() : QStringLiteral("The game or object type is unavailable.");
        m_database.rollback();
        if (error) *error = detail;
        return false;
    }

    if (!sectionId.isEmpty()) {
        if (!setObjectPlacement(objectId, sectionId, true, error)) {
            m_database.rollback();
            return false;
        }
    }

    for (const FieldDefinition &field : fields) {
        if (field.archived) continue;
        if (field.fieldType == QStringLiteral("formula")) continue;
        const bool referenceField = field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references");
        if (referenceField) {
            QSqlQuery remove(m_database);
            remove.prepare(QStringLiteral("DELETE FROM object_relationships WHERE object_id=? AND field_id=?"));
            remove.addBindValue(objectId);
            remove.addBindValue(field.id);
            if (!remove.exec()) {
                m_database.rollback();
                if (error) *error = remove.lastError().text();
                return false;
            }
            const QStringList ids = object.references.value(field.id);
            if (field.fieldType == QStringLiteral("reference") && ids.size() > 1) {
                m_database.rollback();
                if (error) *error = QStringLiteral("%1 accepts only one referenced object.").arg(field.name);
                return false;
            }
            QSet<QString> uniqueIds;
            const QString allowedType = field.options.value(QStringLiteral("allowed_type_id")).toString();
            for (const QString &referencedId : ids) {
                if (referencedId.isEmpty() || uniqueIds.contains(referencedId)) {
                    m_database.rollback();
                    if (error) *error = QStringLiteral("References must contain unique, valid objects.");
                    return false;
                }
                uniqueIds.insert(referencedId);
                QSqlQuery validate(m_database);
                validate.prepare(QStringLiteral(
                    "SELECT o.object_type_id,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id "
                    "WHERE o.id=? AND o.game_id=? AND t.archived=0"));
                validate.addBindValue(referencedId);
                validate.addBindValue(object.gameId);
                const bool foundTarget = validate.exec() && validate.next();
                const bool archivedTarget = foundTarget && validate.value(1).toBool();
                const bool wasAlreadyLinked = beforeReferences.value(field.id).contains(referencedId);
                if (!foundTarget || (archivedTarget && !wasAlreadyLinked) ||
                    (!allowedType.isEmpty() && validate.value(0).toString() != allowedType)) {
                    m_database.rollback();
                    if (error) *error = QStringLiteral("References must target an active object in this game, or preserve an existing archived reference, and match the configured target type.");
                    return false;
                }
                QSqlQuery link(m_database);
                link.prepare(QStringLiteral(
                    "INSERT INTO object_relationships(game_id,object_id,object_type_id,field_id,referenced_object_id) VALUES(?,?,?,?,?)"));
                link.addBindValue(object.gameId);
                link.addBindValue(objectId);
                link.addBindValue(object.typeId);
                link.addBindValue(field.id);
                link.addBindValue(referencedId);
                if (!link.exec()) {
                    m_database.rollback();
                    if (error) *error = link.lastError().text();
                    return false;
                }
            }
            QSqlQuery removeValue(m_database);
            removeValue.prepare(QStringLiteral("DELETE FROM field_values WHERE object_id=? AND field_id=?"));
            removeValue.addBindValue(objectId);
            removeValue.addBindValue(field.id);
            if (!removeValue.exec()) {
                m_database.rollback();
                if (error) *error = removeValue.lastError().text();
                return false;
            }
            continue;
        }
        const QJsonValue value = object.values.value(field.id);
        if (value.isUndefined() || value.isNull()) {
            QSqlQuery remove(m_database);
            remove.prepare(QStringLiteral("DELETE FROM field_values WHERE object_id=? AND field_id=?"));
            remove.addBindValue(objectId);
            remove.addBindValue(field.id);
            if (!remove.exec()) {
                m_database.rollback();
                if (error) *error = remove.lastError().text();
                return false;
            }
            continue;
        }
        auto validationFailure = [&](const QString &message) {
            m_database.rollback();
            if (error) *error = QStringLiteral("%1: %2").arg(field.name, message);
            return false;
        };
        if ((field.fieldType == QStringLiteral("text") || field.fieldType == QStringLiteral("long_text")) && !value.isString()) {
            return validationFailure(QStringLiteral("Enter text for this field."));
        }
        if (field.fieldType == QStringLiteral("text") && value.toString().size() > 2000) {
            return validationFailure(QStringLiteral("Text values must be 2000 characters or fewer."));
        }
        if (field.fieldType == QStringLiteral("long_text") && value.toString().size() > 50000) {
            return validationFailure(QStringLiteral("Long text values must be 50000 characters or fewer."));
        }
        if (field.fieldType == QStringLiteral("integer") && (!value.isDouble() || std::floor(value.toDouble()) != value.toDouble())) {
            return validationFailure(QStringLiteral("Enter a whole number."));
        }
        if ((field.fieldType == QStringLiteral("decimal") || field.fieldType == QStringLiteral("percentage")) &&
            (!value.isDouble() || !std::isfinite(value.toDouble()))) {
            return validationFailure(QStringLiteral("Enter a valid number."));
        }
        if (field.fieldType == QStringLiteral("boolean") && !value.isBool()) {
            return validationFailure(QStringLiteral("Choose true or false."));
        }
        if (field.fieldType == QStringLiteral("select") &&
            (!value.isString() || !field.options.value(QStringLiteral("choices")).toArray().contains(value))) {
            return validationFailure(QStringLiteral("Choose one of the configured options."));
        }
        if (field.fieldType == QStringLiteral("date") &&
            (!value.isString() || !QDate::fromString(value.toString(), Qt::ISODate).isValid())) {
            return validationFailure(QStringLiteral("Enter a valid ISO date."));
        }
        if (field.fieldType == QStringLiteral("percentage") && (value.toDouble() < 0 || value.toDouble() > 100)) {
            m_database.rollback();
            if (error) *error = QStringLiteral("%1: percentage must be between 0 and 100.").arg(field.name);
            return false;
        }
        if (field.fieldType == QStringLiteral("progress")) {
            const QJsonObject progress = value.toObject();
            const QJsonValue current = progress.value(QStringLiteral("current"));
            const QJsonValue maximum = progress.value(QStringLiteral("max"));
            if (!value.isObject() || !current.isDouble() || !maximum.isDouble() ||
                !std::isfinite(current.toDouble()) || !std::isfinite(maximum.toDouble()) || maximum.toDouble() <= 0) {
                m_database.rollback();
                if (error) *error = QStringLiteral("%1: progress requires finite current and positive maximum values.").arg(field.name);
                return false;
            }
        }
        QSqlQuery saveValue(m_database);
        saveValue.prepare(QStringLiteral(
            "INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) "
            "ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at"));
        saveValue.addBindValue(objectId);
        saveValue.addBindValue(object.typeId);
        saveValue.addBindValue(field.id);
        saveValue.addBindValue(QString::fromUtf8(compactJson(value)));
        saveValue.addBindValue(timestamp);
        if (!saveValue.exec()) {
            m_database.rollback();
            if (error) *error = saveValue.lastError().text();
            return false;
        }
    }

    const QString eventId = historyEventId();
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) "
        "VALUES(?,?,?,?,?,?,?,?)"));
    event.addBindValue(eventId);
    event.addBindValue(object.gameId);
    event.addBindValue(objectId);
    event.addBindValue(object.name.trimmed());
    event.addBindValue(m_userId);
    event.addBindValue(isNew ? QStringLiteral("created") : QStringLiteral("updated"));
    event.addBindValue(QStringLiteral("api"));
    event.addBindValue(timestamp);
    bool eventRecorded = isNew;
    if (isNew && !event.exec()) {
        m_database.rollback();
        if (error) *error = event.lastError().text();
        return false;
    }
    auto recordChange = [&](const QString &fieldId, const QString &fieldName, const QString &fieldType,
                            const QByteArray &before, const QByteArray &after) {
        if (!eventRecorded) {
            if (!event.exec()) {
                if (error) *error = event.lastError().text();
                return false;
            }
            eventRecorded = true;
        }
        QSqlQuery change(m_database);
        change.prepare(QStringLiteral(
            "INSERT INTO object_history_changes(event_id,field_id,field_name,field_type,before_json,after_json) VALUES(?,?,?,?,?,?)"));
        change.addBindValue(eventId);
        change.addBindValue(fieldId.isNull() ? QStringLiteral("") : fieldId);
        change.addBindValue(fieldName);
        change.addBindValue(fieldType);
        change.addBindValue(before.isEmpty() ? QVariant() : QVariant(QString::fromUtf8(before)));
        change.addBindValue(after.isEmpty() ? QVariant() : QVariant(QString::fromUtf8(after)));
        if (change.exec()) return true;
        if (error) *error = change.lastError().text();
        return false;
    };
    const auto recordMetadata = [&](const QString &name, const QString &before, const QString &after) {
        return before == after || recordChange({}, name, QStringLiteral("object"), compactJson(before), compactJson(after));
    };
    if (!isNew && (!recordMetadata(QStringLiteral("Object name"), previousName, object.name.trimmed()) ||
                   !recordMetadata(QStringLiteral("Description"), previousDescription, object.description.trimmed()) ||
                   !recordMetadata(QStringLiteral("Image"), previousImage, object.image))) {
        m_database.rollback();
        return false;
    }
    for (const FieldDefinition &field : fields) {
        if (field.archived || field.fieldType == QStringLiteral("formula")) continue;
        const QByteArray before = beforeValues.value(field.id);
        QByteArray after;
        if (field.fieldType == QStringLiteral("reference") || field.fieldType == QStringLiteral("references")) {
            QStringList refs = object.references.value(field.id);
            std::sort(refs.begin(), refs.end());
            if (!refs.isEmpty()) {
                QJsonArray ids;
                for (const QString &id : refs) ids.append(id);
                after = QJsonDocument(ids).toJson(QJsonDocument::Compact);
            }
            const QStringList oldRefs = beforeReferences.value(field.id);
            if (oldRefs != refs && !recordChange(field.id, field.name, field.fieldType, oldRefs.isEmpty() ? QByteArray() : QJsonDocument(QJsonArray::fromStringList(oldRefs)).toJson(QJsonDocument::Compact), after)) {
                m_database.rollback();
                return false;
            }
            continue;
        }
        const QJsonValue value = object.values.value(field.id);
        if (!value.isUndefined() && !value.isNull()) after = compactJson(value);
        const bool changed = before.isEmpty() != after.isEmpty() ||
            parseJsonValue(QString::fromUtf8(before)) != parseJsonValue(QString::fromUtf8(after));
        if (changed && !recordChange(field.id, field.name, field.fieldType, before, after)) {
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        if (error) *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    object.id = objectId;
    return true;
}

bool GameRepository::setObjectPlacement(const QString &objectId, const QString &sectionId, bool placed, QString *error)
{
    if (error) error->clear();
    QSqlQuery query(m_database);
    const QString eligible = QStringLiteral(
        "SELECT s.id,s.game_id,o.id FROM sections s JOIN games g ON g.id=s.game_id "
        "JOIN objects o ON o.game_id=s.game_id JOIN object_types t ON t.id=o.object_type_id "
        "WHERE o.id=? AND s.id=? AND g.user_id=? AND g.archived=0 AND s.archived=0 AND o.archived=0 AND t.archived=0");
    if (placed) {
        query.prepare(QStringLiteral("INSERT INTO section_placements(section_id,game_id,object_id,created_at) "
                                     "SELECT placement.*,? FROM (%1) placement WHERE 1 "
                                     "ON CONFLICT(section_id,object_id) DO UPDATE SET created_at=section_placements.created_at").arg(eligible));
        query.addBindValue(nowUtc());
    } else {
        query.prepare(QStringLiteral("DELETE FROM section_placements WHERE (section_id,game_id,object_id) IN (%1)").arg(eligible));
    }
    query.addBindValue(objectId);
    query.addBindValue(sectionId);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text()
            : QStringLiteral("The object, section, or placement is no longer available.");
        return false;
    }
    return true;
}

bool GameRepository::archiveObject(const QString &objectId, QString *error)
{
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral(
        "SELECT o.game_id,o.name FROM objects o JOIN games g ON g.id=o.game_id WHERE o.id=? AND g.user_id=? AND o.archived=0"));
    lookup.addBindValue(objectId);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        m_database.rollback();
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The object is no longer available.");
        return false;
    }
    const QString gameId = lookup.value(0).toString();
    const QString name = lookup.value(1).toString();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE objects SET archived=1,updated_at=? WHERE id=? AND archived=0"));
    update.addBindValue(nowUtc());
    update.addBindValue(objectId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_database.rollback();
        if (error) *error = update.lastError().text();
        return false;
    }
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) VALUES(?,?,?,?,?,'archived','api',?)"));
    event.addBindValue(historyEventId());
    event.addBindValue(gameId);
    event.addBindValue(objectId);
    event.addBindValue(name);
    event.addBindValue(m_userId);
    event.addBindValue(nowUtc());
    if (!event.exec() || !m_database.commit()) {
        m_database.rollback();
        if (error) *error = event.lastError().isValid() ? event.lastError().text() : m_database.lastError().text();
        return false;
    }
    return true;
}

QVector<UploadedAsset> GameRepository::uploadedAssets(QString *error)
{
    if (error) error->clear();
    QString cleanupError;
    const bool cleanupOkay = cleanupPendingAssets(&cleanupError);
    QVector<UploadedAsset> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,size_bytes,deleted_at FROM uploaded_assets WHERE user_id=? ORDER BY created_at DESC,id"));
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        const QString id = query.value(0).toString();
        result.append(UploadedAsset{id, QStringLiteral("/api/assets/") + id, query.value(1).toLongLong(), !query.value(2).isNull()});
    }
    if (query.lastError().isValid() && error) *error = query.lastError().text();
    else if (!cleanupOkay && error) *error = cleanupError;
    return result;
}

bool GameRepository::uploadAsset(const QByteArray &encoded, UploadedAsset *created, QString *error)
{
    if (error) error->clear();
    QString decodeError;
    const QImage decoded = ImageDecoder::decode(encoded, &decodeError);
    if (decoded.isNull()) {
        if (error) *error = decodeError.isEmpty() ? QStringLiteral("The image could not be decoded.") : decodeError;
        return false;
    }
    QByteArray normalized;
    QBuffer buffer(&normalized);
    if (!buffer.open(QIODevice::WriteOnly) || !decoded.save(&buffer, "PNG")) {
        if (error) *error = QStringLiteral("Could not normalize the image to PNG.");
        return false;
    }
    if (normalized.isEmpty() || normalized.size() > ImageDecoder::maximumBytes) {
        if (error) *error = QStringLiteral("The processed image exceeds the 5 MB storage limit.");
        return false;
    }
    QString cleanupError;
    cleanupPendingAssets(&cleanupError);
    if (!QDir().mkpath(m_assetRoot)) {
        if (error) *error = QStringLiteral("Could not create the private image storage directory.");
        return false;
    }
    QFile::setPermissions(m_assetRoot, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString id = uuid();
    const QString filename = id + QStringLiteral(".png");
    QString path;
    if (!resolveAssetPath(filename, &path, error)) return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(normalized) != normalized.size() || !file.commit()) {
        const QString detail = file.errorString();
        file.cancelWriting();
        if (error) *error = QStringLiteral("Could not write the image file: %1").arg(detail);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    if (!m_database.transaction()) {
        QFile::remove(path);
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery quota(m_database);
    quota.prepare(QStringLiteral("SELECT COUNT(*),COALESCE(SUM(size_bytes),0) FROM uploaded_assets WHERE user_id=?"));
    quota.addBindValue(m_userId);
    if (!quota.exec() || !quota.next()) {
        const QString detail = quota.lastError().text();
        m_database.rollback();
        QFile::remove(path);
        if (error) *error = detail;
        return false;
    }
    const int count = quota.value(0).toInt();
    const qint64 usedBytes = quota.value(1).toLongLong();
    if (count >= 1000 || usedBytes + normalized.size() > 250LL * 1024 * 1024) {
        m_database.rollback();
        QFile::remove(path);
        if (error) *error = QStringLiteral("The account image limit is 1000 files or 250 MB.");
        return false;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO uploaded_assets(id,user_id,game_id,filename,mime_type,size_bytes,created_at,deleted_at) VALUES(?,?,NULL,?,'image/png',?,?,NULL)"));
    insert.addBindValue(id);
    insert.addBindValue(m_userId);
    insert.addBindValue(filename);
    insert.addBindValue(normalized.size());
    insert.addBindValue(nowUtc());
    if (!insert.exec() || !m_database.commit()) {
        const QString detail = insert.lastError().isValid() ? insert.lastError().text() : m_database.lastError().text();
        m_database.rollback();
        QFile::remove(path);
        if (error) *error = detail;
        return false;
    }
    if (created) *created = UploadedAsset{id, QStringLiteral("/api/assets/") + id, normalized.size(), false};
    return true;
}

bool GameRepository::deleteAsset(const QString &assetId, bool *pendingDeletion, QString *error)
{
    if (error) error->clear();
    if (pendingDeletion) *pendingDeletion = false;
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT deleted_at FROM uploaded_assets WHERE id=? AND user_id=?"));
    lookup.addBindValue(assetId);
    lookup.addBindValue(m_userId);
    if (!lookup.exec() || !lookup.next()) {
        m_database.rollback();
        if (error) *error = lookup.lastError().isValid() ? lookup.lastError().text() : QStringLiteral("The image is unavailable.");
        return false;
    }
    const bool alreadyQueued = !lookup.value(0).isNull();
    if (!alreadyQueued) {
        QSqlQuery used(m_database);
        used.prepare(QStringLiteral("SELECT COUNT(*) FROM asset_references WHERE asset_id=?"));
        used.addBindValue(assetId);
        if (!used.exec() || !used.next()) {
            m_database.rollback();
            if (error) *error = used.lastError().text();
            return false;
        }
        if (used.value(0).toInt() > 0) {
            m_database.rollback();
            if (error) *error = QStringLiteral("This image is still used by a record, including an archived record.");
            return false;
        }
        QSqlQuery queue(m_database);
        queue.prepare(QStringLiteral("UPDATE uploaded_assets SET deleted_at=? WHERE id=? AND user_id=? AND deleted_at IS NULL"));
        queue.addBindValue(nowUtc());
        queue.addBindValue(assetId);
        queue.addBindValue(m_userId);
        if (!queue.exec() || queue.numRowsAffected() != 1) {
            m_database.rollback();
            if (error) *error = queue.lastError().isValid() ? queue.lastError().text() : QStringLiteral("Could not queue image deletion.");
            return false;
        }
    }
    if (!m_database.commit()) {
        if (error) *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    QString cleanupError;
    if (!cleanupPendingAssets(&cleanupError)) {
        if (pendingDeletion) *pendingDeletion = true;
        if (error) *error = cleanupError;
    }
    return true;
}

QImage GameRepository::loadAssetImage(const QString &reference, QString *error) const
{
    if (error) error->clear();
    static const QString prefix = QStringLiteral("/api/assets/");
    if (!reference.startsWith(prefix) || reference.size() != prefix.size() + 36) {
        if (error) *error = QStringLiteral("The image reference is invalid.");
        return {};
    }
    const QString id = reference.mid(prefix.size());
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT filename FROM uploaded_assets WHERE id=? AND user_id=? AND deleted_at IS NULL"));
    query.addBindValue(id);
    query.addBindValue(m_userId);
    if (!query.exec() || !query.next()) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The image is unavailable.");
        return {};
    }
    QString path;
    if (!resolveAssetPath(query.value(0).toString(), &path, error)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("The image file is unavailable: %1").arg(file.errorString());
        return {};
    }
    const QByteArray encoded = file.read(ImageDecoder::maximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = file.errorString();
        return {};
    }
    return ImageDecoder::decode(encoded, error);
}

bool GameRepository::retryAssetCleanup(QString *error)
{
    if (error) error->clear();
    return cleanupPendingAssets(error);
}

bool GameRepository::cleanupPendingAssets(QString *error)
{
    if (error) error->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,filename FROM uploaded_assets WHERE user_id=? AND deleted_at IS NOT NULL ORDER BY deleted_at,id LIMIT 25"));
    query.addBindValue(m_userId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    QVector<QPair<QString, QString>> pending;
    while (query.next()) pending.append({query.value(0).toString(), query.value(1).toString()});
    if (query.lastError().isValid()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    bool success = true;
    QString firstError;
    for (const auto &entry : pending) {
        QString path;
        QString pathError;
        if (!resolveAssetPath(entry.second, &path, &pathError)) {
            success = false;
            if (firstError.isEmpty()) firstError = pathError;
            continue;
        }
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            success = false;
            if (firstError.isEmpty()) firstError = QStringLiteral("Could not remove pending image file: %1").arg(path);
            continue;
        }
        QSqlQuery remove(m_database);
        remove.prepare(QStringLiteral("DELETE FROM uploaded_assets WHERE id=? AND user_id=? AND deleted_at IS NOT NULL"));
        remove.addBindValue(entry.first);
        remove.addBindValue(m_userId);
        if (!remove.exec() || remove.numRowsAffected() != 1) {
            success = false;
            if (firstError.isEmpty()) firstError = remove.lastError().isValid() ? remove.lastError().text() : QStringLiteral("Could not finish pending image deletion.");
        }
    }
    if (!success && error) *error = firstError;
    return success;
}

bool GameRepository::resolveAssetPath(const QString &filename, QString *path, QString *error) const
{
    if (filename.isEmpty() || QFileInfo(filename).fileName() != filename || filename.contains(QLatin1Char('/')) ||
        filename.contains(QLatin1Char('\\')) || !filename.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
        filename.size() != 40) {
        if (error) *error = QStringLiteral("The stored image name is invalid.");
        return false;
    }
    const QString absoluteRoot = QDir::cleanPath(QFileInfo(m_assetRoot).absoluteFilePath());
    const QString canonicalRoot = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(absoluteRoot).canonicalFilePath()));
    if (canonicalRoot.isEmpty()) {
        if (error) *error = QStringLiteral("The private image storage directory is unavailable.");
        return false;
    }
    const QString expectedRoot = QDir::cleanPath(QDir::fromNativeSeparators(absoluteRoot));
    if (canonicalRoot.compare(expectedRoot, Qt::CaseInsensitive) != 0) {
        if (error) *error = QStringLiteral("The private image storage directory resolves outside its owned location.");
        return false;
    }
    const QString candidate = QDir::cleanPath(QDir(canonicalRoot).filePath(filename));
    const QFileInfo candidateInfo(candidate);
    const QString rootPrefix = canonicalRoot.endsWith(QLatin1Char('/')) ? canonicalRoot : canonicalRoot + QLatin1Char('/');
    QString resolved;
    if (candidateInfo.exists()) resolved = QDir::cleanPath(QDir::fromNativeSeparators(candidateInfo.canonicalFilePath()));
    else {
        const QString parent = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(candidateInfo.absolutePath()).canonicalFilePath()));
        if (parent == canonicalRoot) resolved = QDir::cleanPath(parent + QLatin1Char('/') + filename);
    }
    if (resolved.isEmpty() || !resolved.startsWith(rootPrefix, Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("The resolved image path is outside the private storage directory.");
        return false;
    }
    if (path) *path = QDir::toNativeSeparators(candidate);
    return true;
}

QString GameRepository::databasePath() const
{
    return m_path;
}

} // namespace gmst
