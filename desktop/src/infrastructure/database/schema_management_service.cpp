#include "infrastructure/database/schema_management_service.h"

#include "domain/formula_evaluator.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>
#include <functional>
#include <limits>

namespace gmst {
namespace {

QString timestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QByteArray compactJson(const QJsonValue &value)
{
    if (value.isObject()) return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    if (value.isArray()) return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

bool isNumberField(const QString &type)
{
    return type == QStringLiteral("integer") || type == QStringLiteral("decimal") ||
        type == QStringLiteral("percentage") || type == QStringLiteral("progress") ||
        type == QStringLiteral("formula");
}

bool jsonHasStringOrKey(const QJsonValue &value, const QString &needle)
{
    if (value.isString()) return value.toString() == needle;
    if (value.isArray()) {
        for (const QJsonValue &child : value.toArray()) if (jsonHasStringOrKey(child, needle)) return true;
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.contains(needle)) return true;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (jsonHasStringOrKey(it.value(), needle)) return true;
    }
    return false;
}

bool validDefault(const FieldDefinition &field, const QJsonValue &value, QString *error)
{
    if (value.isUndefined() || value.isNull()) return true;
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (field.fieldType == QStringLiteral("formula") || field.fieldType == QStringLiteral("reference") ||
        field.fieldType == QStringLiteral("references"))
        return fail(QStringLiteral("Formula and object-reference fields cannot have a default value."));
    if (field.fieldType == QStringLiteral("text") || field.fieldType == QStringLiteral("long_text")) {
        const int limit = field.fieldType == QStringLiteral("text") ? 2000 : 50000;
        return value.isString() && value.toString().size() <= limit
            ? true : fail(QStringLiteral("Enter text within the field's character limit."));
    }
    if (field.fieldType == QStringLiteral("integer") || field.fieldType == QStringLiteral("decimal") ||
        field.fieldType == QStringLiteral("percentage")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble())) return fail(QStringLiteral("Enter a finite numeric default."));
        if (field.fieldType == QStringLiteral("integer") && std::floor(value.toDouble()) != value.toDouble())
            return fail(QStringLiteral("Integer defaults must be whole numbers."));
        if (field.fieldType == QStringLiteral("percentage") && (value.toDouble() < 0 || value.toDouble() > 100))
            return fail(QStringLiteral("Percentage defaults must be between 0 and 100."));
        return true;
    }
    if (field.fieldType == QStringLiteral("boolean"))
        return value.isBool() ? true : fail(QStringLiteral("Boolean defaults must be true or false."));
    if (field.fieldType == QStringLiteral("select")) {
        const QJsonArray choices = field.options.value(QStringLiteral("choices")).toArray();
        return value.isString() && choices.contains(value) ? true : fail(QStringLiteral("Choose one of the configured select options."));
    }
    if (field.fieldType == QStringLiteral("date")) {
        const QDate parsed = QDate::fromString(value.toString(), Qt::ISODate);
        return value.isString() && parsed.isValid() && parsed.toString(Qt::ISODate) == value.toString()
            ? true : fail(QStringLiteral("Date defaults must use YYYY-MM-DD."));
    }
    if (field.fieldType == QStringLiteral("progress")) {
        const QJsonObject progress = value.toObject();
        const double current = progress.value(QStringLiteral("current")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double maximum = progress.value(QStringLiteral("max")).toDouble(std::numeric_limits<double>::quiet_NaN());
        return value.isObject() && std::isfinite(current) && std::isfinite(maximum) && maximum > 0
            ? true : fail(QStringLiteral("Progress defaults need finite current and positive maximum values."));
    }
    return fail(QStringLiteral("This field type does not support a default value."));
}

bool readFieldRow(QSqlQuery &query, FieldDefinition *field)
{
    if (!query.next()) return false;
    field->id = query.value(0).toString();
    field->objectTypeId = query.value(1).toString();
    field->name = query.value(2).toString();
    field->fieldType = query.value(3).toString();
    field->icon = query.value(4).toString();
    field->description = query.value(5).toString();
    const QVariant rawDefault = query.value(6);
    if (!rawDefault.isNull()) {
        QJsonParseError parseError;
        const QByteArray wrapped = QByteArrayLiteral("[") + rawDefault.toString().toUtf8() + QByteArrayLiteral("]");
        const QJsonDocument parsed = QJsonDocument::fromJson(wrapped, &parseError);
        if (parseError.error != QJsonParseError::NoError || parsed.array().isEmpty()) return false;
        field->defaultValue = parsed.array().first();
    }
    const QJsonDocument options = QJsonDocument::fromJson(query.value(7).toString().toUtf8());
    field->options = options.object();
    field->sortOrder = query.value(8).toInt();
    field->archived = query.value(9).toInt() != 0;
    return true;
}

QStringList fieldDependencies(QSqlDatabase database, const QString &fieldId, const QString &gameId, QString *error)
{
    QStringList dependencies;
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM goals WHERE game_id=? AND field_id=?"));
    query.addBindValue(gameId);
    query.addBindValue(fieldId);
    if (!query.exec() || !query.next()) {
        if (error) *error = QStringLiteral("Could not inspect goal references: %1").arg(query.lastError().text());
        return {};
    }
    if (query.value(0).toInt() > 0)
        dependencies.append(QStringLiteral("goal targets"));

    query.clear();
    query.prepare(QStringLiteral("SELECT target_json FROM goals WHERE game_id=?"));
    query.addBindValue(gameId);
    if (!query.exec()) {
        if (error) *error = QStringLiteral("Could not inspect goal rules: %1").arg(query.lastError().text());
        return {};
    }
    while (query.next()) {
        const QJsonDocument target = QJsonDocument::fromJson(query.value(0).toString().toUtf8());
        const QJsonValue root = target.isObject() ? QJsonValue(target.object()) : QJsonValue(target.array());
        if (jsonHasStringOrKey(root, fieldId)) {
            dependencies.append(QStringLiteral("goal conditions"));
            break;
        }
    }

    QSqlQuery relationships(database);
    relationships.prepare(QStringLiteral("SELECT COUNT(*) FROM object_relationships WHERE field_id=?"));
    relationships.addBindValue(fieldId);
    if (!relationships.exec() || !relationships.next()) {
        if (error) *error = QStringLiteral("Could not inspect object relationships: %1").arg(relationships.lastError().text());
        return {};
    }
    if (relationships.value(0).toInt() > 0)
        dependencies.append(QStringLiteral("object relationships"));

    QSqlQuery primaryDisplay(database);
    primaryDisplay.prepare(QStringLiteral("SELECT COUNT(*) FROM object_types WHERE primary_display_field_id=?"));
    primaryDisplay.addBindValue(fieldId);
    if (!primaryDisplay.exec() || !primaryDisplay.next()) {
        if (error) *error = QStringLiteral("Could not inspect primary display fields: %1").arg(primaryDisplay.lastError().text());
        return {};
    }
    if (primaryDisplay.value(0).toInt() > 0)
        dependencies.append(QStringLiteral("primary display field"));

    QSqlQuery formulas(database);
    formulas.prepare(QStringLiteral("SELECT f.id,f.name,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=? AND f.field_type='formula'"));
    formulas.addBindValue(gameId);
    if (!formulas.exec()) {
        if (error) *error = QStringLiteral("Could not inspect formula dependencies: %1").arg(formulas.lastError().text());
        return {};
    }
    while (formulas.next()) {
        const QJsonObject options = QJsonDocument::fromJson(formulas.value(2).toString().toUtf8()).object();
        const QString expression = options.value(QStringLiteral("expression")).toString();
        const QJsonObject aggregation = options.value(QStringLiteral("aggregation")).toObject();
        if (FormulaEvaluator::references(expression).contains(fieldId) ||
            aggregation.value(QStringLiteral("relationship_field_id")).toString() == fieldId ||
            aggregation.value(QStringLiteral("field_id")).toString() == fieldId)
            dependencies.append(QStringLiteral("formula: %1").arg(formulas.value(1).toString()));
    }

    const auto scanJson = [&](const QString &sql, const QString &label) {
        QSqlQuery scan(database);
        scan.prepare(sql);
        scan.addBindValue(gameId);
        if (!scan.exec()) {
            if (error) *error = QStringLiteral("Could not inspect %1 dependencies: %2").arg(label, scan.lastError().text());
            return;
        }
        while (scan.next()) {
            const QJsonDocument doc = QJsonDocument::fromJson(scan.value(0).toString().toUtf8());
            const QJsonValue root = doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array());
            if (jsonHasStringOrKey(root, fieldId)) {
                const QString owner = scan.value(1).toString();
                dependencies.append(owner.isEmpty() ? label : QStringLiteral("%1: %2").arg(label, owner));
            }
        }
    };
    scanJson(QStringLiteral("SELECT display_config,name FROM sections WHERE game_id=?"), QStringLiteral("saved section view"));
    if (error && !error->isEmpty()) return {};
    scanJson(QStringLiteral("SELECT values_json,name FROM object_presets WHERE game_id=?"), QStringLiteral("object preset"));
    if (error && !error->isEmpty()) return {};
    scanJson(QStringLiteral("SELECT layout_json,game_id FROM game_dashboards WHERE game_id=?"), QStringLiteral("dashboard"));
    if (error && !error->isEmpty()) return {};
    dependencies.removeDuplicates();
    return dependencies;
}

} // namespace

SchemaManagementService::SchemaManagementService(QSqlDatabase database, QString userId)
    : m_database(std::move(database)), m_userId(std::move(userId))
{
}

bool SchemaManagementService::updateObjectType(const ObjectType &type, QString *error) const
{
    if (error) error->clear();
    const QString name = type.name.trimmed();
    if (name.isEmpty() || name.size() > 80) {
        if (error) *error = QStringLiteral("Object type name must be between 1 and 80 characters.");
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE object_types SET name=?,description=?,icon=?,image=?,primary_display_field_id=?,updated_at=? "
        "WHERE id=? AND game_id=? AND archived=0 AND EXISTS(SELECT 1 FROM games g WHERE g.id=object_types.game_id AND g.user_id=? AND g.archived=0) "
        "AND (?='' OR EXISTS(SELECT 1 FROM field_definitions f WHERE f.id=? AND f.object_type_id=object_types.id AND f.archived=0))"));
    query.addBindValue(name);
    query.addBindValue(type.description.trimmed());
    query.addBindValue(type.icon.isEmpty() ? QStringLiteral("◈") : type.icon);
    query.addBindValue(type.image.trimmed());
    const QString primaryFieldId = type.primaryDisplayFieldId.isNull() ? QStringLiteral("") : type.primaryDisplayFieldId;
    query.addBindValue(primaryFieldId);
    query.addBindValue(timestamp());
    query.addBindValue(type.id);
    query.addBindValue(type.gameId);
    query.addBindValue(m_userId);
    query.addBindValue(primaryFieldId);
    query.addBindValue(primaryFieldId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The type or primary display field is unavailable.");
        return false;
    }
    return true;
}

bool SchemaManagementService::setObjectTypeArchived(const QString &typeId, bool archived, QString *error) const
{
    if (error) error->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE object_types SET archived=?,updated_at=? WHERE id=? AND archived=? AND EXISTS(SELECT 1 FROM games g WHERE g.id=object_types.game_id AND g.user_id=? AND g.archived=0)"));
    query.addBindValue(archived ? 1 : 0);
    query.addBindValue(timestamp());
    query.addBindValue(typeId);
    query.addBindValue(archived ? 0 : 1);
    query.addBindValue(m_userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The object type is no longer available.");
        return false;
    }
    return true;
}

bool SchemaManagementService::setPrimaryDisplayField(const QString &typeId, const QString &fieldId, QString *error) const
{
    if (error) error->clear();
    const QString normalizedFieldId = fieldId.isNull() ? QStringLiteral("") : fieldId;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE object_types SET primary_display_field_id=?,updated_at=? WHERE id=? AND archived=0 "
        "AND EXISTS(SELECT 1 FROM games g WHERE g.id=object_types.game_id AND g.user_id=? AND g.archived=0) "
        "AND (?='' OR EXISTS(SELECT 1 FROM field_definitions f WHERE f.id=? AND f.object_type_id=object_types.id AND f.archived=0))"));
    query.addBindValue(normalizedFieldId);
    query.addBindValue(timestamp());
    query.addBindValue(typeId);
    query.addBindValue(m_userId);
    query.addBindValue(normalizedFieldId);
    query.addBindValue(normalizedFieldId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("Choose an active field belonging to this type.");
        return false;
    }
    return true;
}

bool SchemaManagementService::updateField(const FieldDefinition &input, const QMap<QString, QString> &choiceReplacements,
                                         QString *error)
{
    if (error) error->clear();
    if (input.name.trimmed().isEmpty() || input.name.trimmed().size() > 80) {
        if (error) *error = QStringLiteral("Field name must be between 1 and 80 characters.");
        return false;
    }
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    const auto rollback = [this] { m_database.rollback(); };
    FieldDefinition current;
    QSqlQuery load(m_database);
    load.prepare(QStringLiteral(
        "SELECT f.id,f.object_type_id,f.name,f.field_type,f.icon,f.description,f.default_json,f.options_json,f.sort_order,f.archived "
        "FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id JOIN games g ON g.id=t.game_id "
        "WHERE f.id=? AND f.object_type_id=? AND f.archived=0 AND t.archived=0 AND g.user_id=? AND g.archived=0"));
    load.addBindValue(input.id);
    load.addBindValue(input.objectTypeId);
    load.addBindValue(m_userId);
    if (!load.exec() || !readFieldRow(load, &current)) {
        rollback();
        if (error) *error = load.lastError().isValid() ? load.lastError().text() : QStringLiteral("The field is no longer available.");
        return false;
    }
    if (current.fieldType != input.fieldType) {
        rollback();
        if (error) *error = QStringLiteral("Field types can only be changed through the safe field migration workflow.");
        return false;
    }
    FieldDefinition candidate = input;
    candidate.name = input.name.trimmed();
    if (candidate.fieldType == QStringLiteral("select")) {
        const QJsonValue rawChoices = candidate.options.value(QStringLiteral("choices"));
        const QJsonArray choices = rawChoices.toArray();
        QSet<QString> unique;
        if (!rawChoices.isArray() || choices.isEmpty() || choices.size() > 200) {
            rollback();
            if (error) *error = QStringLiteral("Select fields need between 1 and 200 choices.");
            return false;
        }
        for (const QJsonValue &choice : choices) {
            const QString text = choice.toString();
            if (!choice.isString() || text.trimmed().isEmpty() || text.size() > 100 || unique.contains(text)) {
                rollback();
                if (error) *error = QStringLiteral("Select choices must be unique, nonempty text values up to 100 characters.");
                return false;
            }
            unique.insert(text);
        }
        for (auto it = choiceReplacements.cbegin(); it != choiceReplacements.cend(); ++it) {
            if (!unique.contains(it.value())) {
                rollback();
                if (error) *error = QStringLiteral("Every select replacement must be one of the new choices.");
                return false;
            }
        }
        const auto remap = [&](const QJsonValue &raw, QJsonValue *converted) {
            if (raw.isNull() || raw.isUndefined()) { *converted = raw; return true; }
            if (!raw.isString()) return false;
            const QString oldChoice = raw.toString();
            if (unique.contains(oldChoice)) { *converted = oldChoice; return true; }
            const auto replacement = choiceReplacements.constFind(oldChoice);
            if (replacement == choiceReplacements.cend()) return false;
            *converted = replacement.value();
            return true;
        };
        QJsonValue mappedDefault;
        if (!remap(candidate.defaultValue, &mappedDefault)) {
            rollback();
            if (error) *error = QStringLiteral("A removed choice is still used by the default or saved values. Choose a replacement for every removed value.");
            return false;
        }
        candidate.defaultValue = mappedDefault;
        QSqlQuery values(m_database);
        values.prepare(QStringLiteral("SELECT object_id,value_json FROM field_values WHERE field_id=? ORDER BY object_id"));
        values.addBindValue(candidate.id);
        if (!values.exec()) {
            rollback();
            if (error) *error = values.lastError().text();
            return false;
        }
        QVector<QPair<QString, QJsonValue>> updates;
        while (values.next()) {
            QJsonParseError parseError;
            const QJsonDocument parsed = QJsonDocument::fromJson(QByteArrayLiteral("[") + values.value(1).toString().toUtf8() + QByteArrayLiteral("]"), &parseError);
            if (parseError.error != QJsonParseError::NoError || parsed.array().isEmpty()) {
                rollback();
                if (error) *error = QStringLiteral("A saved select value is invalid JSON; no changes were applied.");
                return false;
            }
            const QJsonValue raw = parsed.array().first();
            QJsonValue converted;
            if (!remap(raw, &converted)) {
                rollback();
                if (error) *error = QStringLiteral("A removed choice is still used by an object. Choose a replacement for every removed value.");
                return false;
            }
            if (converted != raw) updates.append({values.value(0).toString(), converted});
        }
        for (const auto &entry : updates) {
            QSqlQuery updateValue(m_database);
            updateValue.prepare(QStringLiteral("UPDATE field_values SET value_json=?,updated_at=? WHERE object_id=? AND field_id=?"));
            updateValue.addBindValue(QString::fromUtf8(compactJson(entry.second)));
            updateValue.addBindValue(timestamp());
            updateValue.addBindValue(entry.first);
            updateValue.addBindValue(candidate.id);
            if (!updateValue.exec()) {
                rollback();
                if (error) *error = updateValue.lastError().text();
                return false;
            }
        }
    }

    if (!validDefault(candidate, candidate.defaultValue, error)) {
        rollback();
        return false;
    }

    if (candidate.fieldType == QStringLiteral("reference") || candidate.fieldType == QStringLiteral("references")) {
        const QString allowedType = candidate.options.value(QStringLiteral("allowed_type_id")).toString();
        if (!allowedType.isEmpty()) {
            QSqlQuery valid(m_database);
            valid.prepare(QStringLiteral("SELECT 1 FROM object_types source JOIN games g ON g.id=source.game_id JOIN object_types target ON target.game_id=source.game_id WHERE source.id=? AND target.id=? AND source.archived=0 AND target.archived=0 AND g.user_id=? AND g.archived=0"));
            valid.addBindValue(candidate.objectTypeId);
            valid.addBindValue(allowedType);
            valid.addBindValue(m_userId);
            if (!valid.exec() || !valid.next()) {
                rollback();
                if (error) *error = QStringLiteral("Allowed reference type must be active and belong to the same game.");
                return false;
            }
            QSqlQuery mismatched(m_database);
            mismatched.prepare(QStringLiteral("SELECT COUNT(*) FROM object_relationships rel JOIN objects target ON target.id=rel.referenced_object_id WHERE rel.field_id=? AND target.object_type_id<>?"));
            mismatched.addBindValue(candidate.id);
            mismatched.addBindValue(allowedType);
            if (!mismatched.exec() || !mismatched.next() || mismatched.value(0).toInt() > 0) {
                rollback();
                if (error) *error = QStringLiteral("Existing references use another type. Update those links before changing the allowed target type.");
                return false;
            }
        }
    }

    if (candidate.fieldType == QStringLiteral("formula")) {
        const QString expression = candidate.options.value(QStringLiteral("expression")).toString();
        QString syntaxError;
        const QStringList refs = FormulaEvaluator::references(expression);
        if (expression.trimmed().isEmpty() || !FormulaEvaluator::validateExpression(expression, &syntaxError) || refs.isEmpty()) {
            rollback();
            if (error) *error = syntaxError.isEmpty() ? QStringLiteral("Formula fields need an expression that references at least one numeric field.") : syntaxError;
            return false;
        }
        QSqlQuery gameQuery(m_database);
        gameQuery.prepare(QStringLiteral("SELECT game_id FROM object_types WHERE id=? AND archived=0"));
        gameQuery.addBindValue(candidate.objectTypeId);
        if (!gameQuery.exec() || !gameQuery.next()) {
            rollback();
            if (error) *error = QStringLiteral("The selected object type is unavailable.");
            return false;
        }
        const QString gameId = gameQuery.value(0).toString();
        QMap<QString, QStringList> formulaDependencies;
        QSqlQuery formulas(m_database);
        formulas.prepare(QStringLiteral("SELECT f.id,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=? AND f.field_type='formula' AND f.archived=0"));
        formulas.addBindValue(gameId);
        if (!formulas.exec()) {
            rollback();
            if (error) *error = formulas.lastError().text();
            return false;
        }
        while (formulas.next()) {
            const QJsonObject options = QJsonDocument::fromJson(formulas.value(1).toString().toUtf8()).object();
            formulaDependencies.insert(formulas.value(0).toString(), FormulaEvaluator::references(options.value(QStringLiteral("expression")).toString()));
        }
        formulaDependencies.insert(candidate.id, refs);
        QSet<QString> seen;
        std::function<bool(const QString &)> reachesSelf = [&](const QString &id) {
            if (id == candidate.id) return true;
            if (seen.contains(id)) return false;
            seen.insert(id);
            for (const QString &next : formulaDependencies.value(id)) if (reachesSelf(next)) return true;
            return false;
        };
        for (const QString &ref : refs) {
            if (ref == QStringLiteral("related") && !candidate.options.value(QStringLiteral("aggregation")).toObject().isEmpty()) continue;
            QSqlQuery dependency(m_database);
            dependency.prepare(QStringLiteral("SELECT field_type FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0"));
            dependency.addBindValue(ref);
            dependency.addBindValue(candidate.objectTypeId);
            if (!dependency.exec() || !dependency.next() || !isNumberField(dependency.value(0).toString())) {
                rollback();
                if (error) *error = QStringLiteral("Formulas can reference numeric or formula fields on this object type only.");
                return false;
            }
            seen.clear();
            if (reachesSelf(ref)) {
                rollback();
                if (error) *error = QStringLiteral("This formula would create a circular dependency.");
                return false;
            }
        }
        const QJsonObject aggregation = candidate.options.value(QStringLiteral("aggregation")).toObject();
        if (!aggregation.isEmpty()) {
            const QString relationId = aggregation.value(QStringLiteral("relationship_field_id")).toString();
            const QString operation = aggregation.value(QStringLiteral("operation")).toString();
            QSqlQuery relation(m_database);
            relation.prepare(QStringLiteral("SELECT field_type,options_json FROM field_definitions WHERE id=? AND object_type_id=? AND archived=0"));
            relation.addBindValue(relationId);
            relation.addBindValue(candidate.objectTypeId);
            if (!relation.exec() || !relation.next() ||
                (relation.value(0).toString() != QStringLiteral("reference") && relation.value(0).toString() != QStringLiteral("references")) ||
                !QStringList{QStringLiteral("sum"),QStringLiteral("avg"),QStringLiteral("min"),QStringLiteral("max"),QStringLiteral("count")}.contains(operation)) {
                rollback();
                if (error) *error = QStringLiteral("Choose a valid relationship and aggregation operation.");
                return false;
            }
            if (operation != QStringLiteral("count")) {
                QSqlQuery target(m_database);
                target.prepare(QStringLiteral("SELECT f.field_type FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE f.id=? AND t.game_id=? AND f.archived=0 AND t.archived=0"));
                target.addBindValue(aggregation.value(QStringLiteral("field_id")).toString());
                target.addBindValue(gameId);
                if (!target.exec() || !target.next() || !QStringList{QStringLiteral("integer"), QStringLiteral("decimal"),
                    QStringLiteral("percentage"), QStringLiteral("progress")}.contains(target.value(0).toString())) {
                    rollback();
                    if (error) *error = QStringLiteral("Choose a numeric field on an active type in this game.");
                    return false;
                }
                const QString allowedType = QJsonDocument::fromJson(relation.value(1).toString().toUtf8()).object()
                    .value(QStringLiteral("allowed_type_id")).toString();
                if (!allowedType.isEmpty()) {
                    QSqlQuery targetOwner(m_database);
                    targetOwner.prepare(QStringLiteral("SELECT object_type_id FROM field_definitions WHERE id=?"));
                    targetOwner.addBindValue(aggregation.value(QStringLiteral("field_id")).toString());
                    if (!targetOwner.exec() || !targetOwner.next() || targetOwner.value(0).toString() != allowedType) {
                        rollback();
                        if (error) *error = QStringLiteral("The aggregated field must belong to the relationship's allowed target type.");
                        return false;
                    }
                }
            }
        }
    }

    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE field_definitions SET name=?,icon=?,description=?,default_json=?,options_json=?,sort_order=?,updated_at=? WHERE id=? AND object_type_id=? AND field_type=? AND archived=0"));
    update.addBindValue(candidate.name);
    update.addBindValue(candidate.icon.isEmpty() ? QStringLiteral("✦") : candidate.icon);
    update.addBindValue(candidate.description.trimmed());
    update.addBindValue(candidate.defaultValue.isUndefined() || candidate.defaultValue.isNull() ? QVariant() : QVariant(QString::fromUtf8(compactJson(candidate.defaultValue))));
    update.addBindValue(QString::fromUtf8(QJsonDocument(candidate.options).toJson(QJsonDocument::Compact)));
    update.addBindValue(candidate.sortOrder);
    update.addBindValue(timestamp());
    update.addBindValue(candidate.id);
    update.addBindValue(candidate.objectTypeId);
    update.addBindValue(candidate.fieldType);
    if (!update.exec() || update.numRowsAffected() != 1) {
        rollback();
        if (error) *error = update.lastError().isValid() ? update.lastError().text() : QStringLiteral("The field changed while it was being edited.");
        return false;
    }
    if (!m_database.commit()) {
        if (error) *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

bool SchemaManagementService::setFieldArchived(const QString &fieldId, bool archived, QString *error) const
{
    if (error) error->clear();
    QSqlQuery owner(m_database);
    owner.prepare(QStringLiteral("SELECT f.archived,t.game_id FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id JOIN games g ON g.id=t.game_id WHERE f.id=? AND g.user_id=? AND g.archived=0 AND t.archived=0"));
    owner.addBindValue(fieldId);
    owner.addBindValue(m_userId);
    if (!owner.exec() || !owner.next() || owner.value(0).toInt() == (archived ? 1 : 0)) {
            if (error) *error = owner.lastError().isValid() ? QStringLiteral("Could not locate field: %1").arg(owner.lastError().text()) : QStringLiteral("The field is no longer available.");
        return false;
    }
    if (archived) {
        QString dependencyError;
        const QStringList dependencies = fieldDependencies(m_database, fieldId, owner.value(1).toString(), &dependencyError);
        if (!dependencyError.isEmpty()) {
            if (error) *error = dependencyError;
            return false;
        }
        if (!dependencies.isEmpty()) {
            if (error) *error = QStringLiteral("Cannot archive this field while it is used by: %1. Update those dependencies first.").arg(dependencies.join(QStringLiteral(", ")));
            return false;
        }
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE field_definitions SET archived=?,updated_at=? WHERE id=? AND archived=?"));
    update.addBindValue(archived ? 1 : 0);
    update.addBindValue(timestamp());
    update.addBindValue(fieldId);
    update.addBindValue(archived ? 0 : 1);
    if (!update.exec() || update.numRowsAffected() != 1) {
            if (error) *error = update.lastError().isValid() ? QStringLiteral("Could not update archived state: %1").arg(update.lastError().text()) : QStringLiteral("The field changed while it was being updated.");
        return false;
    }
    return true;
}

} // namespace gmst
