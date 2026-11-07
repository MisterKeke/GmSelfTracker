#include "infrastructure/database/field_migration_service.h"

#include "domain/formula_evaluator.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace gmst {
namespace {

const QStringList supportedTypes = {
    QStringLiteral("text"), QStringLiteral("long_text"), QStringLiteral("integer"), QStringLiteral("decimal"),
    QStringLiteral("percentage"), QStringLiteral("boolean"), QStringLiteral("select"), QStringLiteral("date"),
    QStringLiteral("progress"), QStringLiteral("reference"), QStringLiteral("references"), QStringLiteral("formula")
};

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray compactJson(const QJsonValue &value)
{
    if (value.isObject()) return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    if (value.isArray()) return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

QString canonicalDecimal(const QString &source)
{
    static const QRegularExpression numberPattern(QStringLiteral(R"(^(-?)(0|[1-9][0-9]*)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?$)"));
    const QString text = source.trimmed();
    const QRegularExpressionMatch match = numberPattern.match(text);
    if (!match.hasMatch() || text.toUtf8().size() > 128) return {};
    bool exponentOkay = false;
    const int exponent = match.captured(4).isEmpty() ? 0 : match.captured(4).toInt(&exponentOkay);
    if (!match.captured(4).isEmpty() && (!exponentOkay || exponent < -1000 || exponent > 1000)) return {};
    QString digits = match.captured(2) + match.captured(3);
    int decimalPosition = match.captured(2).size() + exponent;
    qsizetype leading = 0;
    while (leading < digits.size() && digits.at(leading) == QLatin1Char('0')) ++leading;
    digits.remove(0, leading);
    decimalPosition -= static_cast<int>(leading);
    if (digits.isEmpty()) return QStringLiteral("0");
    while (digits.size() > 1 && digits.endsWith(QLatin1Char('0'))) digits.chop(1);
    QString result;
    if (decimalPosition <= 0) result = QStringLiteral("0.") + QString(-decimalPosition, QLatin1Char('0')) + digits;
    else if (decimalPosition >= digits.size()) result = digits + QString(decimalPosition - digits.size(), QLatin1Char('0'));
    else result = digits.left(decimalPosition) + QLatin1Char('.') + digits.mid(decimalPosition);
    if (result.startsWith(QLatin1Char('.'))) result.prepend(QLatin1Char('0'));
    if (result.startsWith(QStringLiteral("0."))) { /* already normalized */ }
    if (result.startsWith(QStringLiteral("-"))) return result;
    return match.captured(1) == QStringLiteral("-") ? QLatin1Char('-') + result : result;
}

QJsonValue parseValue(const QByteArray &raw, bool *valid = nullptr)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArrayLiteral("[") + raw + QByteArrayLiteral("]"), &parseError);
    const bool okay = parseError.error == QJsonParseError::NoError && !document.array().isEmpty();
    if (valid) *valid = okay;
    return okay ? document.array().first() : QJsonValue();
}

bool isNumeric(const QString &type)
{
    return type == QStringLiteral("integer") || type == QStringLiteral("decimal") || type == QStringLiteral("percentage");
}

bool isReference(const QString &type)
{
    return type == QStringLiteral("reference") || type == QStringLiteral("references");
}

bool sameFamily(const QString &from, const QString &to)
{
    const bool numeric = [](const QString &type) { return type == QStringLiteral("integer") || type == QStringLiteral("decimal") || type == QStringLiteral("percentage") || type == QStringLiteral("formula"); }(from) &&
        (to == QStringLiteral("integer") || to == QStringLiteral("decimal") || to == QStringLiteral("percentage") || to == QStringLiteral("formula"));
    const bool textual = (from == QStringLiteral("text") || from == QStringLiteral("long_text")) &&
        (to == QStringLiteral("text") || to == QStringLiteral("long_text"));
    return numeric || textual || (isReference(from) && isReference(to));
}

QMap<QString, QString> stringMap(const QJsonObject &object, const QString &key)
{
    QMap<QString, QString> result;
    const QJsonObject mapping = object.value(key).toObject();
    for (auto it = mapping.begin(); it != mapping.end(); ++it)
        if (it.value().isString()) result.insert(it.key(), it.value().toString());
    return result;
}

QJsonValue convertValue(const QString &from, const QString &to, const QJsonObject &targetOptions,
                        const QJsonObject &policy, const QJsonValue &value, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QJsonValue();
    };
    if (value.isUndefined() || value.isNull()) return QJsonValue(QJsonValue::Null);
    if (from == QStringLiteral("formula") || to == QStringLiteral("formula") || isReference(from))
        return fail(QStringLiteral("This conversion requires calculated or relationship values to be migrated explicitly."));

    if (isReference(to)) {
        if (from != QStringLiteral("text") && from != QStringLiteral("long_text") && from != QStringLiteral("select"))
            return fail(QStringLiteral("Only text, long text, or select values can be mapped to an object reference."));
        if (!value.isString()) return fail(QStringLiteral("Saved value is not text."));
        const QString mapped = stringMap(policy, QStringLiteral("reference_mapping")).value(value.toString());
        if (mapped.isEmpty()) return fail(QStringLiteral("Choose a target object for this text value."));
        return to == QStringLiteral("reference") ? QJsonValue(mapped) : QJsonValue(QJsonArray{mapped});
    }

    const auto parseNumber = [&](double *number) {
        if (isNumeric(from) && value.isDouble()) {
            *number = value.toDouble();
            return std::isfinite(*number);
        }
        if ((from == QStringLiteral("text") || from == QStringLiteral("long_text")) && value.isString()) {
            const QString text = value.toString().trimmed();
            static const QRegularExpression strictNumber(QStringLiteral(R"(^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$)"));
            bool okay = false;
            const double parsed = text.toDouble(&okay);
            if (text.toUtf8().size() > 128 || !strictNumber.match(text).hasMatch() || !okay || !std::isfinite(parsed)) return false;
            if (to == QStringLiteral("integer")) {
                const QString exact = canonicalDecimal(text);
                if (exact.isEmpty() || exact.contains(QLatin1Char('.'))) return false;
            }
            *number = parsed;
            return true;
        }
        if (from == QStringLiteral("progress") && value.isObject()) {
            const QJsonObject progress = value.toObject();
            const double current = progress.value(QStringLiteral("current")).toDouble(std::numeric_limits<double>::quiet_NaN());
            const double maximum = progress.value(QStringLiteral("max")).toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0) return false;
            const QString choice = policy.value(QStringLiteral("progress_value")).toString();
            if (to == QStringLiteral("percentage") && choice != QStringLiteral("percentage")) return false;
            if (choice == QStringLiteral("current")) *number = current;
            else if (choice == QStringLiteral("maximum")) *number = maximum;
            else if (choice == QStringLiteral("percentage")) *number = current / maximum * 100.0;
            else return false;
            return std::isfinite(*number);
        }
        return false;
    };

    if (to == QStringLiteral("text") || to == QStringLiteral("long_text")) {
        QString convertedText;
        if (isNumeric(from)) {
            if (!value.isDouble() || !std::isfinite(value.toDouble())) return fail(QStringLiteral("Saved value is not numeric."));
            convertedText = QString::fromLatin1(compactJson(value));
        } else if (from == QStringLiteral("boolean")) {
            if (!value.isBool()) return fail(QStringLiteral("Saved value is not boolean."));
            convertedText = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        } else if (from == QStringLiteral("text") || from == QStringLiteral("long_text") || from == QStringLiteral("select") || from == QStringLiteral("date")) {
            if (!value.isString()) return fail(QStringLiteral("Saved value is not text."));
            convertedText = value.toString();
        } else return fail(QStringLiteral("Values from %1 cannot be converted to text automatically.").arg(from));
        const int maximum = to == QStringLiteral("text") ? 2000 : 50000;
        if (convertedText.size() > maximum) return fail(QStringLiteral("Converted text exceeds the target field limit."));
        return QJsonValue(convertedText);
    }

    if (isNumeric(to)) {
        double number = 0;
        if (!parseNumber(&number)) {
            if (from == QStringLiteral("progress"))
                return fail(to == QStringLiteral("percentage")
                    ? QStringLiteral("Choose current / maximum × 100 for percentage conversion.")
                    : QStringLiteral("Choose whether progress should become current or maximum."));
            return fail(QStringLiteral("Saved value is not a valid finite number."));
        }
        if (to == QStringLiteral("percentage") && (number < 0 || number > 100)) {
            if (policy.value(QStringLiteral("percentage_strategy")).toString() != QStringLiteral("clamp"))
                return fail(QStringLiteral("Value is outside 0–100; choose the explicit clamp strategy or resolve it manually."));
            number = std::clamp(number, 0.0, 100.0);
        }
        if (to == QStringLiteral("integer")) {
            if (std::floor(number) != number) return fail(QStringLiteral("Value is not mathematically integral."));
        }
        return QJsonValue(number);
    }

    if (to == QStringLiteral("boolean")) {
        if (from != QStringLiteral("text") && from != QStringLiteral("long_text"))
            return fail(QStringLiteral("Only explicit text values true or false can become a checkbox."));
        const QString text = value.toString().trimmed().toLower();
        if (text == QStringLiteral("true")) return QJsonValue(true);
        if (text == QStringLiteral("false")) return QJsonValue(false);
        return fail(QStringLiteral("Text must be exactly true or false to become a checkbox."));
    }

    if (to == QStringLiteral("select")) {
        if (from != QStringLiteral("text") && from != QStringLiteral("long_text"))
            return fail(QStringLiteral("Only text values matching a target choice can become a select value."));
        if (!value.isString()) return fail(QStringLiteral("Saved value is not text."));
        QString text = value.toString();
        const QMap<QString, QString> replacements = stringMap(policy, QStringLiteral("select_replacements"));
        if (replacements.contains(text)) text = replacements.value(text);
        for (const QJsonValue &choice : targetOptions.value(QStringLiteral("choices")).toArray())
            if (choice.toString() == text) return QJsonValue(text);
        return fail(QStringLiteral("Value does not match a target select option."));
    }

    if (to == QStringLiteral("date")) {
        if ((from != QStringLiteral("text") && from != QStringLiteral("long_text")) || !value.isString())
            return fail(QStringLiteral("Only canonical ISO dates can become date values."));
        const QString text = value.toString();
        const QDate date = QDate::fromString(text, Qt::ISODate);
        if (!date.isValid() || date.toString(Qt::ISODate) != text)
            return fail(QStringLiteral("Value must be a canonical ISO date in YYYY-MM-DD format."));
        return QJsonValue(text);
    }

    if (to == QStringLiteral("progress")) {
        if (!isNumeric(from)) return fail(QStringLiteral("A valid numeric value and maximum are required for a progress field."));
        double number = 0;
        if (!parseNumber(&number)) return fail(QStringLiteral("Saved value is not numeric."));
        const double maximum = policy.value(QStringLiteral("maximum")).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(maximum) || maximum <= 0) return fail(QStringLiteral("Choose a progress maximum greater than zero."));
        return QJsonObject{{QStringLiteral("current"), number}, {QStringLiteral("max"), maximum}};
    }

    return fail(QStringLiteral("Conversion to %1 is not supported.").arg(to));
}

QByteArray convertRawValue(const QString &from, const QString &to, const QJsonObject &targetOptions,
                           const QJsonObject &policy, const QByteArray &raw, const QJsonValue &value,
                           QJsonValue *convertedValue, QString *error)
{
    QString conversionError;
    QJsonValue converted = convertValue(from, to, targetOptions, policy, value, &conversionError);
    const auto fail = [&](const QString &message) {
        if (error) *error = message;
        return QByteArray();
    };
    if (!conversionError.isEmpty()) return fail(conversionError);
    QByteArray output;
    if (isNumeric(to)) {
        QString exactNumber;
        if (isNumeric(from)) exactNumber = canonicalDecimal(QString::fromUtf8(raw));
        else if (from == QStringLiteral("text") || from == QStringLiteral("long_text")) exactNumber = canonicalDecimal(value.toString());
        else exactNumber = canonicalDecimal(QString::fromLatin1(compactJson(converted)));
        if (exactNumber.isEmpty()) return fail(QStringLiteral("Number cannot be represented as valid JSON."));
        bool okay = false;
        const double numeric = exactNumber.toDouble(&okay);
        if (!okay || !std::isfinite(numeric)) return fail(QStringLiteral("Number is outside the supported range."));
        if (to == QStringLiteral("percentage") && (numeric < 0 || numeric > 100) &&
            policy.value(QStringLiteral("percentage_strategy")).toString() == QStringLiteral("clamp"))
            exactNumber = numeric < 0 ? QStringLiteral("0") : QStringLiteral("100");
        if (to == QStringLiteral("integer")) {
            if (exactNumber.contains(QLatin1Char('.'))) return fail(QStringLiteral("Value is not mathematically integral."));
            bool integerOkay = false;
            const qlonglong integer = exactNumber.toLongLong(&integerOkay);
            if (!integerOkay) return fail(QStringLiteral("Integer is outside the signed 64-bit range."));
            exactNumber = QString::number(integer);
        }
        output = exactNumber.toLatin1();
    } else if (isNumeric(from) && (to == QStringLiteral("text") || to == QStringLiteral("long_text"))) {
        const QString exactNumber = canonicalDecimal(QString::fromUtf8(raw));
        const int maximum = to == QStringLiteral("text") ? 2000 : 50000;
        if (exactNumber.isEmpty() || exactNumber.size() > maximum)
            return fail(QStringLiteral("Converted text exceeds the target field limit or is not an exact number."));
        output = compactJson(QJsonValue(exactNumber));
    } else if (isNumeric(from) && to == QStringLiteral("progress")) {
        const QString exactNumber = canonicalDecimal(QString::fromUtf8(raw));
        const QByteArray maximum = compactJson(policy.value(QStringLiteral("maximum")));
        if (exactNumber.isEmpty() || maximum.isEmpty()) return fail(QStringLiteral("Progress value or maximum is invalid."));
        output = QByteArrayLiteral("{\"current\":") + exactNumber.toLatin1() + QByteArrayLiteral(",\"max\":") + maximum + QByteArrayLiteral("}");
    } else output = compactJson(converted);
    if (convertedValue) {
        bool valid = false;
        *convertedValue = parseValue(output, &valid);
        if (!valid) return fail(QStringLiteral("Converted value could not be encoded as JSON."));
    }
    if (error) error->clear();
    return output;
}

struct StoredEntry {
    QString id;
    QString name;
    bool archived = false;
    QByteArray raw;
    QStringList references;
};

bool readField(QSqlDatabase database, const QString &userId, const QString &fieldId,
               QString *typeId, QString *gameId, QString *name, QString *fieldType,
               QJsonValue *defaultValue, QByteArray *defaultRaw, QJsonObject *options, QString *error)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT f.object_type_id,t.game_id,f.name,f.field_type,f.default_json,f.options_json "
        "FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id JOIN games g ON g.id=t.game_id "
        "WHERE f.id=? AND f.archived=0 AND t.archived=0 AND g.user_id=? AND g.archived=0"));
    query.addBindValue(fieldId);
    query.addBindValue(userId);
    if (!query.exec() || !query.next()) {
        if (error) *error = query.lastError().isValid() ? query.lastError().text() : QStringLiteral("The field is no longer available.");
        return false;
    }
    *typeId = query.value(0).toString();
    *gameId = query.value(1).toString();
    *name = query.value(2).toString();
    *fieldType = query.value(3).toString();
    if (!query.value(4).isNull()) {
        if (defaultRaw) *defaultRaw = query.value(4).toString().toUtf8();
        bool valid = false;
        *defaultValue = parseValue(query.value(4).toString().toUtf8(), &valid);
        if (!valid) {
            if (error) *error = QStringLiteral("The field default is invalid JSON.");
            return false;
        }
    }
    *options = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).object();
    return true;
}

QStringList referenceIds(const QStringList &ids, const QString &targetType, QSqlDatabase database,
                         const QString &gameId, const QJsonObject &options, QString *error)
{
    const QString allowedType = options.value(QStringLiteral("allowed_type_id")).toString();
    for (const QString &id : ids) {
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT o.object_type_id,o.archived,t.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id "
            "WHERE o.id=? AND o.game_id=?"));
        query.addBindValue(id);
        query.addBindValue(gameId);
        if (!query.exec() || !query.next() || query.value(1).toInt() != 0 || query.value(2).toInt() != 0 ||
            (!allowedType.isEmpty() && query.value(0).toString() != allowedType)) {
            if (error) *error = QStringLiteral("Mapped object must be active, belong to this game, and match the allowed target type.");
            return {};
        }
    }
    Q_UNUSED(targetType);
    return ids;
}

bool validDefault(const QString &type, const QJsonObject &options, const QJsonValue &value, QString *error)
{
    if (value.isUndefined() || value.isNull()) return true;
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (type == QStringLiteral("text") || type == QStringLiteral("long_text")) {
        const int maximum = type == QStringLiteral("text") ? 2000 : 50000;
        return value.isString() && value.toString().size() <= maximum ? true : fail(QStringLiteral("Text default exceeds the target field limit."));
    }
    if (type == QStringLiteral("integer") || type == QStringLiteral("decimal") || type == QStringLiteral("percentage")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble())) return fail(QStringLiteral("Enter a finite numeric target default."));
        if (type == QStringLiteral("integer") && (std::floor(value.toDouble()) != value.toDouble() ||
            std::abs(value.toDouble()) > 9007199254740991.0)) return fail(QStringLiteral("Integer defaults must be exact whole numbers within the native numeric range."));
        if (type == QStringLiteral("percentage") && (value.toDouble() < 0 || value.toDouble() > 100)) return fail(QStringLiteral("Percentage defaults must be between 0 and 100."));
        return true;
    }
    if (type == QStringLiteral("boolean")) return value.isBool() ? true : fail(QStringLiteral("Boolean defaults must be true or false."));
    if (type == QStringLiteral("select")) return value.isString() && options.value(QStringLiteral("choices")).toArray().contains(value)
        ? true : fail(QStringLiteral("Choose one of the target select options for the default."));
    if (type == QStringLiteral("date")) {
        const QDate date = QDate::fromString(value.toString(), Qt::ISODate);
        return value.isString() && date.isValid() && date.toString(Qt::ISODate) == value.toString()
            ? true : fail(QStringLiteral("Date defaults must use YYYY-MM-DD."));
    }
    if (type == QStringLiteral("progress")) {
        const QJsonObject progress = value.toObject();
        const double current = progress.value(QStringLiteral("current")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double maximum = progress.value(QStringLiteral("max")).toDouble(std::numeric_limits<double>::quiet_NaN());
        return value.isObject() && std::isfinite(current) && std::isfinite(maximum) && maximum > 0
            ? true : fail(QStringLiteral("Progress defaults need finite current and positive maximum values."));
    }
    return fail(QStringLiteral("The target field type does not support a default value."));
}

QString revision(QSqlDatabase database, const QString &typeId, const QString &fieldId, QString *error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QStringList queries = {
        QStringLiteral("SELECT id,name,field_type,COALESCE(default_json,''),options_json,archived FROM field_definitions WHERE id=?"),
        QStringLiteral("SELECT object_id,value_json FROM field_values WHERE field_id=? ORDER BY object_id"),
        QStringLiteral("SELECT object_id,referenced_object_id FROM object_relationships WHERE field_id=? ORDER BY object_id,referenced_object_id"),
        QStringLiteral("SELECT id,archived FROM objects WHERE object_type_id=? ORDER BY id"),
        QStringLiteral("SELECT object_id,field_id,value_json FROM field_values WHERE object_type_id IN (SELECT id FROM object_types WHERE game_id=(SELECT game_id FROM object_types WHERE id=?)) ORDER BY object_id,field_id"),
        QStringLiteral("SELECT object_id,field_id,referenced_object_id FROM object_relationships WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY object_id,field_id,referenced_object_id"),
        QStringLiteral("SELECT o.id,o.object_type_id,o.archived FROM objects o WHERE o.game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY o.id"),
        QStringLiteral("SELECT f.id,f.object_type_id,f.field_type,f.options_json,f.archived FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY f.id"),
        QStringLiteral("SELECT id,goal_type,field_id,object_id,COALESCE(target_json,'') FROM goals WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id"),
        QStringLiteral("SELECT id,archived,display_config FROM sections WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id"),
        QStringLiteral("SELECT id,primary_display_field_id FROM object_types WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id"),
        QStringLiteral("SELECT id,object_id,name,values_json,updated_at FROM object_presets WHERE game_id=(SELECT game_id FROM object_types WHERE id=?) ORDER BY id")
    };
    for (int index = 0; index < queries.size(); ++index) {
        QSqlQuery query(database);
        query.prepare(queries.at(index));
        query.addBindValue(index < 3 ? fieldId : typeId);
        if (!query.exec()) {
            if (error) *error = query.lastError().text();
            return {};
        }
        while (query.next()) {
            for (int column = 0; column < query.record().count(); ++column) {
                const QByteArray value = query.value(column).toString().toUtf8();
                hash.addData(QByteArray::number(value.size()));
                hash.addData(QByteArrayLiteral(":"));
                hash.addData(value);
                hash.addData(QByteArrayLiteral("\0"));
            }
            hash.addData(QByteArrayLiteral("\xff"));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool containsReference(const QJsonValue &value, const QString &id)
{
    if (value.isString()) return value.toString() == id;
    if (value.isArray()) for (const QJsonValue &item : value.toArray()) if (containsReference(item, id)) return true;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.contains(id)) return true;
        for (auto it = object.begin(); it != object.end(); ++it) if (containsReference(it.value(), id)) return true;
    }
    return false;
}

bool loadFormulaObjects(QSqlDatabase database, const QString &gameId, QVector<ObjectType> *types,
                        QVector<ObjectRecord> *objects, QString *error)
{
    QSqlQuery typeQuery(database);
    typeQuery.prepare(QStringLiteral("SELECT id,game_id,name,description,icon,image,primary_display_field_id,archived FROM object_types WHERE game_id=? ORDER BY sort_order,id"));
    typeQuery.addBindValue(gameId);
    if (!typeQuery.exec()) { if (error) *error = typeQuery.lastError().text(); return false; }
    while (typeQuery.next()) {
        ObjectType type{typeQuery.value(0).toString(), typeQuery.value(1).toString(), typeQuery.value(2).toString(),
            typeQuery.value(3).toString(), typeQuery.value(4).toString(), typeQuery.value(5).toString(), {},
            typeQuery.value(6).toString(), typeQuery.value(7).toInt() != 0};
        QSqlQuery fields(database);
        fields.prepare(QStringLiteral("SELECT id,object_type_id,name,field_type,icon,description,default_json,options_json,sort_order,archived FROM field_definitions WHERE object_type_id=? ORDER BY sort_order,id"));
        fields.addBindValue(type.id);
        if (!fields.exec()) { if (error) *error = fields.lastError().text(); return false; }
        while (fields.next()) {
            FieldDefinition field;
            field.id = fields.value(0).toString();
            field.objectTypeId = fields.value(1).toString();
            field.name = fields.value(2).toString();
            field.fieldType = fields.value(3).toString();
            field.icon = fields.value(4).toString();
            field.description = fields.value(5).toString();
            if (!fields.value(6).isNull()) field.defaultValue = parseValue(fields.value(6).toString().toUtf8());
            field.options = QJsonDocument::fromJson(fields.value(7).toString().toUtf8()).object();
            field.sortOrder = fields.value(8).toInt();
            field.archived = fields.value(9).toInt() != 0;
            type.fields.append(field);
        }
        if (fields.lastError().isValid()) { if (error) *error = fields.lastError().text(); return false; }
        types->append(type);
    }
    if (typeQuery.lastError().isValid()) { if (error) *error = typeQuery.lastError().text(); return false; }

    QSqlQuery objectQuery(database);
    objectQuery.prepare(QStringLiteral("SELECT o.id,o.game_id,o.object_type_id,t.name,o.name,o.description,o.image,o.archived FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.game_id=? ORDER BY o.id"));
    objectQuery.addBindValue(gameId);
    if (!objectQuery.exec()) { if (error) *error = objectQuery.lastError().text(); return false; }
    while (objectQuery.next()) {
        ObjectRecord object;
        object.id = objectQuery.value(0).toString();
        object.gameId = objectQuery.value(1).toString();
        object.typeId = objectQuery.value(2).toString();
        object.typeName = objectQuery.value(3).toString();
        object.name = objectQuery.value(4).toString();
        object.description = objectQuery.value(5).toString();
        object.image = objectQuery.value(6).toString();
        object.archived = objectQuery.value(7).toInt() != 0;
        QSqlQuery values(database);
        values.prepare(QStringLiteral("SELECT field_id,value_json FROM field_values WHERE object_id=?"));
        values.addBindValue(object.id);
        if (!values.exec()) { if (error) *error = values.lastError().text(); return false; }
        while (values.next()) {
            bool valid = false;
            const QJsonValue value = parseValue(values.value(1).toString().toUtf8(), &valid);
            if (!valid) { if (error) *error = QStringLiteral("Object %1 has invalid saved JSON.").arg(object.name); return false; }
            object.values.insert(values.value(0).toString(), value);
        }
        if (values.lastError().isValid()) { if (error) *error = values.lastError().text(); return false; }
        QSqlQuery relations(database);
        relations.prepare(QStringLiteral("SELECT field_id,referenced_object_id FROM object_relationships WHERE object_id=? ORDER BY field_id,referenced_object_id"));
        relations.addBindValue(object.id);
        if (!relations.exec()) { if (error) *error = relations.lastError().text(); return false; }
        while (relations.next()) object.references[relations.value(0).toString()].append(relations.value(1).toString());
        if (relations.lastError().isValid()) { if (error) *error = relations.lastError().text(); return false; }
        objects->append(object);
    }
    if (objectQuery.lastError().isValid()) { if (error) *error = objectQuery.lastError().text(); return false; }

    for (ObjectRecord &object : *objects) {
        const auto type = std::find_if(types->cbegin(), types->cend(), [&](const ObjectType &item) { return item.id == object.typeId; });
        if (type == types->cend()) continue;
        for (const FieldDefinition &field : type->fields) {
            if (!field.archived && !field.defaultValue.isUndefined() && !field.defaultValue.isNull() && !object.values.contains(field.id))
                object.values.insert(field.id, field.defaultValue);
        }
        object.formulaErrors = FormulaEvaluator::evaluate(type->fields, object, *types, *objects);
    }
    return true;
}

FieldMigrationPreview analyze(QSqlDatabase database, const QString &userId, const QString &fieldId,
                              const FieldMigrationRequest &request, QString *error)
{
    FieldMigrationPreview preview;
    preview.fieldId = fieldId;
    QString typeId;
    QString gameId;
    QString previousType;
    QJsonValue previousDefault;
    QByteArray previousDefaultRaw;
    QJsonObject previousOptions;
    if (!readField(database, userId, fieldId, &typeId, &gameId, &preview.fieldName, &previousType,
        &previousDefault, &previousDefaultRaw, &previousOptions, error)) return preview;
    preview.previousType = previousType;
    preview.targetType = request.targetType;
    if (!supportedTypes.contains(request.targetType)) preview.blockers.append(QStringLiteral("Choose a supported field type."));
    if (request.targetType == previousType) preview.blockers.append(QStringLiteral("Choose a different field type."));
    if (request.targetType == QStringLiteral("formula"))
        preview.blockers.append(QStringLiteral("Numeric-to-formula migration is not supported. Create a formula field and keep this field unchanged."));
    if (previousType == QStringLiteral("formula") && !isNumeric(request.targetType))
        preview.blockers.append(QStringLiteral("A calculated field can only be frozen into a numeric field."));
    if (previousType == QStringLiteral("formula") && isNumeric(request.targetType)) {
        if (!request.policy.value(QStringLiteral("freeze_formula")).toBool())
            preview.blockers.append(QStringLiteral("Confirm that the current formula result should be frozen into stored values."));
        preview.formulaWarning = QStringLiteral("Future changes to this formula’s inputs will no longer update these values.");
    }

    QJsonObject targetOptions = request.targetOptions;
    if (targetOptions.isEmpty() && isReference(previousType) && isReference(request.targetType)) targetOptions = previousOptions;
    if (request.targetType == QStringLiteral("select")) {
        const QJsonArray choices = targetOptions.value(QStringLiteral("choices")).toArray();
        QSet<QString> seen;
        if (choices.isEmpty() || choices.size() > 200) preview.blockers.append(QStringLiteral("Select fields need between 1 and 200 target choices."));
        for (const QJsonValue &choice : choices) {
            if (!choice.isString() || choice.toString().trimmed().isEmpty() || choice.toString().size() > 100 || seen.contains(choice.toString()))
                preview.blockers.append(QStringLiteral("Target choices must be unique, nonempty text values up to 100 characters."));
            seen.insert(choice.toString());
        }
    }
    if (isReference(request.targetType)) {
        const QString allowedType = targetOptions.value(QStringLiteral("allowed_type_id")).toString();
        if (!allowedType.isEmpty()) {
            QSqlQuery allowed(database);
            allowed.prepare(QStringLiteral("SELECT 1 FROM object_types WHERE id=? AND game_id=?"));
            allowed.addBindValue(allowedType);
            allowed.addBindValue(gameId);
            if (!allowed.exec() || !allowed.next()) preview.blockers.append(QStringLiteral("Allowed target type must belong to this game and be active."));
        }
    }

    if (request.defaultOverrideProvided) {
        if (request.defaultOverride.isNull()) preview.convertedDefault = QJsonValue(QJsonValue::Null);
        else if (request.defaultOverride.isUndefined()) {
            preview.defaultError = QStringLiteral("Enter a valid target default, or choose to clear it explicitly.");
            preview.convertedDefault = QJsonValue(QJsonValue::Null);
        }
        else {
            preview.convertedDefault = request.defaultOverride;
            validDefault(request.targetType, targetOptions, request.defaultOverride, &preview.defaultError);
            if (preview.defaultError.isEmpty()) preview.convertedDefaultJson = compactJson(request.defaultOverride);
        }
    } else if (!previousDefault.isUndefined() && !previousDefault.isNull()) {
        preview.convertedDefaultJson = convertRawValue(previousType, request.targetType, targetOptions, request.policy,
            previousDefaultRaw, previousDefault, &preview.convertedDefault, &preview.defaultError);
    } else preview.convertedDefault = QJsonValue(QJsonValue::Null);
    if (!preview.defaultError.isEmpty()) preview.blockers.append(QStringLiteral("Default value: %1").arg(preview.defaultError));

    QMap<QString, StoredEntry> entries;
    QSqlQuery values(database);
    values.prepare(QStringLiteral("SELECT o.id,o.name,o.archived,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? ORDER BY o.id"));
    values.addBindValue(fieldId);
    if (!values.exec()) {
        if (error) *error = values.lastError().text();
        return preview;
    }
    while (values.next()) {
        StoredEntry entry;
        entry.id = values.value(0).toString();
        entry.name = values.value(1).toString();
        entry.archived = values.value(2).toInt() != 0;
        entry.raw = values.value(3).toString().toUtf8();
        entries.insert(entry.id, entry);
    }
    QSqlQuery relations(database);
    relations.prepare(QStringLiteral("SELECT o.id,o.name,o.archived,rel.referenced_object_id FROM object_relationships rel JOIN objects o ON o.id=rel.object_id WHERE rel.field_id=? ORDER BY o.id,rel.referenced_object_id"));
    relations.addBindValue(fieldId);
    if (!relations.exec()) {
        if (error) *error = relations.lastError().text();
        return preview;
    }
    while (relations.next()) {
        StoredEntry &entry = entries[relations.value(0).toString()];
        entry.id = relations.value(0).toString();
        entry.name = relations.value(1).toString();
        entry.archived = relations.value(2).toInt() != 0;
        entry.references.append(relations.value(3).toString());
    }

    preview.savedValues = entries.size();
    for (const StoredEntry &entry : std::as_const(entries)) {
        if (entry.archived) ++preview.archivedObjectsAffected;
        QString conversionError;
        bool valid = true;
        if (isReference(previousType)) {
            if (!entry.raw.isEmpty()) {
                conversionError = QStringLiteral("Unexpected saved values on a reference field must be resolved before conversion.");
                valid = false;
            }
            if (isReference(request.targetType)) {
                if (request.targetType == QStringLiteral("reference") && entry.references.size() > 1) {
                    conversionError = QStringLiteral("This object has more than one reference; select one explicitly before converting to a single reference.");
                    valid = false;
                }
                const QString allowedType = targetOptions.value(QStringLiteral("allowed_type_id")).toString();
                if (!allowedType.isEmpty()) {
                    for (const QString &reference : std::as_const(entry.references)) {
                        QSqlQuery target(database);
                        target.prepare(QStringLiteral("SELECT object_type_id FROM objects WHERE id=? AND game_id=?"));
                        target.addBindValue(reference);
                        target.addBindValue(gameId);
                        if (!target.exec() || !target.next() || target.value(0).toString() != allowedType) {
                            conversionError = QStringLiteral("One or more linked objects do not match the target allowed type.");
                            valid = false;
                            break;
                        }
                    }
                }
            } else if (request.targetType == QStringLiteral("text") || request.targetType == QStringLiteral("long_text")) {
                if (!entry.raw.isEmpty()) {
                    conversionError = QStringLiteral("Unexpected saved values on a reference field must be resolved before conversion.");
                    valid = false;
                }
                if (request.policy.value(QStringLiteral("relationship_strategy")).toString() != QStringLiteral("serialize_ids")) {
                    conversionError = QStringLiteral("Choose the explicit serialize linked IDs strategy to preserve these relationships as text.");
                    valid = false;
                }
            } else if (!entry.references.isEmpty() || !entry.raw.isEmpty()) {
                conversionError = QStringLiteral("Object relationships need an explicit mapping before changing to this field type.");
                valid = false;
            }
        } else {
            bool parsed = false;
            const QJsonValue value = parseValue(entry.raw, &parsed);
            if (!parsed) {
                conversionError = QStringLiteral("Saved value is not valid JSON.");
                valid = false;
            } else {
                convertRawValue(previousType, request.targetType, targetOptions, request.policy,
                    entry.raw, value, nullptr, &conversionError);
                valid = conversionError.isEmpty();
                if (valid && isReference(request.targetType)) {
                    const QString id = stringMap(request.policy, QStringLiteral("reference_mapping")).value(value.toString());
                    QString referenceError;
                    referenceIds({id}, request.targetType, database, gameId, targetOptions, &referenceError);
                    if (!referenceError.isEmpty()) { valid = false; conversionError = referenceError; }
                }
            }
        }
        if (!valid) {
            ++preview.incompatibleValues;
            if (preview.examples.size() < 10) {
                FieldMigrationExample example;
                example.objectId = entry.id;
                example.objectName = entry.name;
                example.value = entry.raw.isEmpty() ? QJsonValue(QJsonArray::fromStringList(entry.references)) : parseValue(entry.raw);
                example.reason = conversionError;
                preview.examples.append(example);
            }
        }
    }
    preview.compatibleValues = preview.savedValues - preview.incompatibleValues;

    QSqlQuery affected(database);
    affected.prepare(QStringLiteral("SELECT COUNT(DISTINCT o.id),COUNT(DISTINCT CASE WHEN o.archived=1 THEN o.id END) FROM objects o WHERE o.object_type_id=? AND (EXISTS(SELECT 1 FROM field_values v WHERE v.object_id=o.id AND v.field_id=?) OR EXISTS(SELECT 1 FROM object_relationships r WHERE r.object_id=o.id AND r.field_id=?) OR (?=1) )"));
    affected.addBindValue(typeId);
    affected.addBindValue(fieldId);
    affected.addBindValue(fieldId);
    const bool defaultExists = (!previousDefault.isUndefined() && !previousDefault.isNull()) ||
        (!preview.convertedDefault.isUndefined() && !preview.convertedDefault.isNull());
    affected.addBindValue(defaultExists ? 1 : 0);
    if (affected.exec() && affected.next()) {
        preview.affectedObjects = affected.value(0).toInt();
        preview.archivedObjectsAffected = std::max(preview.archivedObjectsAffected, affected.value(1).toInt());
    }

    const bool differentFamily = !sameFamily(previousType, request.targetType);
    auto countDependency = [&](const QString &sql, const QVariantList &args, const QString &label) {
        QSqlQuery count(database);
        count.prepare(sql);
        for (const QVariant &arg : args) count.addBindValue(arg);
        if (!count.exec() || !count.next()) {
            if (error) *error = count.lastError().text();
            return;
        }
        if (count.value(0).toInt() > 0) preview.dependencies.append(label);
    };
    countDependency(QStringLiteral("SELECT COUNT(*) FROM goals WHERE game_id=? AND (field_id=? OR EXISTS(SELECT 1 FROM json_each(goals.target_json,'$.conditions') c WHERE json_extract(c.value,'$.field_id')=?))"), {gameId, fieldId, fieldId}, QStringLiteral("goal conditions"));
    countDependency(QStringLiteral("SELECT COUNT(*) FROM object_relationships WHERE field_id=?"), {fieldId}, QStringLiteral("object relationships"));
    countDependency(QStringLiteral("SELECT COUNT(*) FROM object_types WHERE primary_display_field_id=?"), {fieldId}, QStringLiteral("primary display field"));
    QSqlQuery presets(database);
    presets.prepare(QStringLiteral("SELECT values_json FROM object_presets WHERE game_id=? ORDER BY id"));
    presets.addBindValue(gameId);
    if (!presets.exec()) { if (error) *error = presets.lastError().text(); return preview; }
    while (presets.next()) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(presets.value(0).toString().toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) *error = QStringLiteral("A saved preset contains invalid JSON and must be repaired before migration.");
            return preview;
        }
        const QJsonObject overrides = document.object();
        if (overrides.contains(fieldId)) ++preview.presetOverrides;
    }
    if (presets.lastError().isValid()) { if (error) *error = presets.lastError().text(); return preview; }
    if (preview.presetOverrides > 0) {
        preview.dependencies.append(QStringLiteral("preset overrides"));
        preview.blockers.append(QStringLiteral("Remove or safely convert preset overrides before changing this field type."));
    }
    QSqlQuery formulas(database);
    formulas.prepare(QStringLiteral("SELECT f.id,f.name,f.options_json FROM field_definitions f JOIN object_types t ON t.id=f.object_type_id WHERE t.game_id=? AND f.field_type='formula'"));
    formulas.addBindValue(gameId);
    if (formulas.exec()) {
        QMap<QString, QString> formulaNames;
        QMap<QString, QStringList> formulaReferences;
        while (formulas.next()) {
            const QString formulaId = formulas.value(0).toString();
            formulaNames.insert(formulaId, formulas.value(1).toString());
            const QJsonObject options = QJsonDocument::fromJson(formulas.value(2).toString().toUtf8()).object();
            const QJsonObject aggregation = options.value(QStringLiteral("aggregation")).toObject();
            QStringList references = FormulaEvaluator::references(options.value(QStringLiteral("expression")).toString());
            const QString relationField = aggregation.value(QStringLiteral("relationship_field_id")).toString();
            const QString aggregateField = aggregation.value(QStringLiteral("field_id")).toString();
            if (!relationField.isEmpty()) references.append(relationField);
            if (!aggregateField.isEmpty()) references.append(aggregateField);
            formulaReferences.insert(formulaId, references);
        }
        for (auto it = formulaReferences.cbegin(); it != formulaReferences.cend(); ++it) {
            QSet<QString> visited;
            std::function<bool(const QString &)> dependsOnField = [&](const QString &candidateId) {
                if (candidateId == fieldId) return true;
                if (visited.contains(candidateId)) return false;
                visited.insert(candidateId);
                for (const QString &reference : formulaReferences.value(candidateId))
                    if (dependsOnField(reference)) return true;
                return false;
            };
            if (dependsOnField(it.key()))
                preview.dependencies.append(QStringLiteral("formula: %1").arg(formulaNames.value(it.key())));
        }
        if (formulas.lastError().isValid()) { if (error) *error = formulas.lastError().text(); return preview; }
    } else {
        if (error) *error = formulas.lastError().text();
        return preview;
    }
    QSqlQuery sections(database);
    sections.prepare(QStringLiteral("SELECT display_config FROM sections WHERE game_id=?"));
    sections.addBindValue(gameId);
    if (sections.exec()) {
        while (sections.next()) {
            const QJsonDocument config = QJsonDocument::fromJson(sections.value(0).toString().toUtf8());
            if (containsReference(config.isObject() ? QJsonValue(config.object()) : QJsonValue(config.array()), fieldId)) {
                preview.dependencies.append(QStringLiteral("saved section view"));
                break;
            }
        }
    }
    preview.dependencies.removeDuplicates();
    if (differentFamily && !preview.dependencies.isEmpty()) {
        if (request.policy.value(QStringLiteral("relationship_strategy")).toString() == QStringLiteral("serialize_ids") &&
            (request.targetType == QStringLiteral("text") || request.targetType == QStringLiteral("long_text")))
            preview.dependencyConflicts = preview.dependencies;
        else preview.dependencyConflicts = preview.dependencies;
        if (request.policy.value(QStringLiteral("relationship_strategy")).toString() == QStringLiteral("serialize_ids") &&
            (request.targetType == QStringLiteral("text") || request.targetType == QStringLiteral("long_text")))
            preview.dependencyConflicts.removeAll(QStringLiteral("object relationships"));
        if (!preview.dependencyConflicts.isEmpty())
            preview.blockers.append(QStringLiteral("Resolve or remove dependent formulas, goals, object views, or display settings before changing to a different value family."));
    }
    if (previousType == QStringLiteral("formula") && isNumeric(request.targetType)) {
        preview.incompatibleValues = 0;
        int staleCount = 0;
        QSqlQuery stale(database);
        stale.prepare(QStringLiteral("SELECT COUNT(*) FROM field_values WHERE field_id=?"));
        stale.addBindValue(fieldId);
        if (!stale.exec() || !stale.next()) { if (error) *error = stale.lastError().text(); return preview; }
        staleCount = stale.value(0).toInt();
        if (staleCount > 0) {
            preview.blockers.append(QStringLiteral("This formula has unexpected stored values. Resolve them before freezing calculated results."));
            QSqlQuery first(database);
            first.prepare(QStringLiteral("SELECT o.id,o.name,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? ORDER BY o.id LIMIT 1"));
            first.addBindValue(fieldId);
            if (first.exec() && first.next() && preview.examples.size() < 10)
                preview.examples.append({first.value(0).toString(), first.value(1).toString(), parseValue(first.value(2).toString().toUtf8()), QStringLiteral("Unexpected stored data cannot be overwritten by a formula freeze.")});
        }
        QVector<ObjectType> formulaTypes;
        QVector<ObjectRecord> formulaObjects;
        if (!loadFormulaObjects(database, gameId, &formulaTypes, &formulaObjects, error)) return preview;
        const auto sourceType = std::find_if(formulaTypes.cbegin(), formulaTypes.cend(), [&](const ObjectType &type) { return type.id == typeId; });
        if (sourceType == formulaTypes.cend()) { if (error) *error = QStringLiteral("Formula type is no longer available."); return preview; }
        preview.savedValues = 0;
        for (const ObjectRecord &object : std::as_const(formulaObjects)) {
            if (object.typeId != typeId) continue;
            ++preview.savedValues;
            QString reason;
            const QJsonValue result = object.values.value(fieldId);
            if (object.formulaErrors.contains(fieldId)) reason = QStringLiteral("Formula result is unavailable: %1").arg(object.formulaErrors.value(fieldId));
            else if (result.isUndefined() || result.isNull()) reason = QStringLiteral("Formula result is missing.");
            else {
                QString conversionError;
                convertRawValue(QStringLiteral("decimal"), request.targetType, targetOptions, request.policy,
                    compactJson(result), result, nullptr, &conversionError);
                reason = conversionError;
            }
            if (!reason.isEmpty()) {
                ++preview.incompatibleValues;
                if (preview.examples.size() < 10) preview.examples.append({object.id, object.name, result, reason});
            }
        }
        preview.affectedObjects = preview.savedValues;
        QSqlQuery archived(database);
        archived.prepare(QStringLiteral("SELECT COUNT(*) FROM objects WHERE object_type_id=? AND archived=1"));
        archived.addBindValue(typeId);
        if (!archived.exec() || !archived.next()) { if (error) *error = archived.lastError().text(); return preview; }
        preview.archivedObjectsAffected = archived.value(0).toInt();
    }
    if (!error || error->isEmpty()) {
        QString revisionError;
        const QString currentRevision = revision(database, typeId, fieldId, &revisionError);
        if (!revisionError.isEmpty()) {
            if (error) *error = revisionError;
            return preview;
        }
        QJsonObject tokenData{{QStringLiteral("field_id"), fieldId}, {QStringLiteral("revision"), currentRevision},
            {QStringLiteral("target_type"), request.targetType},
            {QStringLiteral("target_options"), request.targetOptions.isEmpty() ? targetOptions : request.targetOptions},
            {QStringLiteral("policy"), request.policy},
            {QStringLiteral("default_override_provided"), request.defaultOverrideProvided},
            {QStringLiteral("default_override"), request.defaultOverrideProvided ? request.defaultOverride : QJsonValue(QJsonValue::Null)}};
        preview.previewToken = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(tokenData).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    }
    preview.canApply = preview.targetType != preview.previousType && preview.blockers.isEmpty() && preview.incompatibleValues == 0;
    return preview;
}

void recordHistory(QSqlDatabase database, const QString &userId, const QString &fieldId, const QString &fieldName,
                   const QString &oldType, const QString &newType, const QString &objectId, const QByteArray &before,
                   const QByteArray &after, const QString &now, QString *error)
{
    QSqlQuery object(database);
    object.prepare(QStringLiteral("SELECT game_id,name FROM objects WHERE id=?"));
    object.addBindValue(objectId);
    if (!object.exec() || !object.next()) { if (error) *error = object.lastError().text(); return; }
    const QString gameId = object.value(0).toString();
    const QString objectName = object.value(1).toString();
    const QString eventId = uuid();
    QSqlQuery event(database);
    event.prepare(QStringLiteral("INSERT INTO object_history_events(id,game_id,object_id,object_name,user_id,event_type,source,created_at) VALUES(?,?,?,?,?,'field_type_migrated','internal_migration',?)"));
    event.addBindValue(eventId);
    event.addBindValue(gameId);
    event.addBindValue(objectId);
    event.addBindValue(objectName);
    event.addBindValue(userId);
    event.addBindValue(now);
    if (!event.exec()) { if (error) *error = event.lastError().text(); return; }
    QSqlQuery change(database);
    change.prepare(QStringLiteral("INSERT INTO object_history_changes(event_id,field_id,field_name,field_type,before_field_type,before_json,after_json) VALUES(?,?,?,?,?,?,?)"));
    change.addBindValue(eventId);
    change.addBindValue(fieldId);
    change.addBindValue(fieldName);
    change.addBindValue(newType);
    change.addBindValue(oldType);
    change.addBindValue(before.isEmpty() ? QVariant() : QVariant(QString::fromUtf8(before)));
    change.addBindValue(after.isEmpty() ? QVariant() : QVariant(QString::fromUtf8(after)));
    if (!change.exec() && error) *error = change.lastError().text();
}

} // namespace

FieldMigrationService::FieldMigrationService(QSqlDatabase database, QString userId)
    : m_database(std::move(database)), m_userId(std::move(userId))
{
}

FieldMigrationPreview FieldMigrationService::preview(const QString &fieldId, const FieldMigrationRequest &request, QString *error) const
{
    if (error) error->clear();
    return analyze(m_database, m_userId, fieldId, request, error);
}

bool FieldMigrationService::apply(const QString &fieldId, const FieldMigrationRequest &request, const QString &previewToken,
                                  FieldMigrationPreview *result, QString *error)
{
    if (error) error->clear();
    if (previewToken.isEmpty()) {
        if (error) *error = QStringLiteral("Preview the conversion before applying it.");
        return false;
    }
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    FieldMigrationPreview checked = analyze(m_database, m_userId, fieldId, request, error);
    if (error && !error->isEmpty()) { m_database.rollback(); return false; }
    if (previewToken != checked.previewToken) {
        m_database.rollback();
        if (error) *error = QStringLiteral("Field values or dependencies changed after preview. Review a fresh preview before applying.");
        if (result) *result = checked;
        return false;
    }
    if (!checked.canApply) {
        m_database.rollback();
        if (error) *error = checked.blockers.isEmpty() ? QStringLiteral("Resolve incompatible values before applying this migration.") : checked.blockers.join(QLatin1Char('\n'));
        if (result) *result = checked;
        return false;
    }
    QString typeId, gameId, name, oldType;
    QJsonValue oldDefault;
    QByteArray oldDefaultRaw;
    QJsonObject oldOptions;
    if (!readField(m_database, m_userId, fieldId, &typeId, &gameId, &name, &oldType, &oldDefault, &oldDefaultRaw, &oldOptions, error)) {
        m_database.rollback();
        return false;
    }
    const QJsonObject newOptions = request.targetOptions.isEmpty() && isReference(oldType) && isReference(request.targetType)
        ? oldOptions : request.targetOptions;
    QJsonValue newDefault = checked.convertedDefault;
    const QString migrationId = uuid();
    QSqlQuery migration(m_database);
    migration.prepare(QStringLiteral("INSERT INTO field_type_migrations(id,game_id,field_id,field_name,previous_type,new_type,policy_json,previous_default_json,new_default_json,converted_values,affected_objects,archived_objects_affected,incompatible_values,outcome,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,'applying',?)"));
    migration.addBindValue(migrationId);
    migration.addBindValue(gameId);
    migration.addBindValue(fieldId);
    migration.addBindValue(name);
    migration.addBindValue(oldType);
    migration.addBindValue(request.targetType);
    migration.addBindValue(QString::fromUtf8(QJsonDocument(request.policy).toJson(QJsonDocument::Compact)));
    migration.addBindValue(oldDefault.isUndefined() || oldDefault.isNull() ? QVariant() : QVariant(QString::fromUtf8(oldDefaultRaw)));
    migration.addBindValue(newDefault.isNull() || newDefault.isUndefined() ? QVariant() : QVariant(QString::fromUtf8(checked.convertedDefaultJson)));
    migration.addBindValue(checked.savedValues);
    migration.addBindValue(checked.affectedObjects);
    migration.addBindValue(checked.archivedObjectsAffected);
    migration.addBindValue(checked.incompatibleValues);
    migration.addBindValue(nowUtc());
    if (!migration.exec()) {
        m_database.rollback();
        if (error) *error = migration.lastError().text();
        return false;
    }

    QMap<QString, StoredEntry> entries;
    QSqlQuery values(m_database);
    values.prepare(QStringLiteral("SELECT o.id,o.name,o.archived,v.value_json FROM field_values v JOIN objects o ON o.id=v.object_id WHERE v.field_id=? ORDER BY o.id"));
    values.addBindValue(fieldId);
    if (!values.exec()) { m_database.rollback(); if (error) *error = values.lastError().text(); return false; }
    while (values.next()) entries.insert(values.value(0).toString(), StoredEntry{values.value(0).toString(), values.value(1).toString(), values.value(2).toInt()!=0, values.value(3).toString().toUtf8(), {}});
    QSqlQuery relations(m_database);
    relations.prepare(QStringLiteral("SELECT o.id,o.name,o.archived,rel.referenced_object_id FROM object_relationships rel JOIN objects o ON o.id=rel.object_id WHERE rel.field_id=? ORDER BY o.id,rel.referenced_object_id"));
    relations.addBindValue(fieldId);
    if (!relations.exec()) { m_database.rollback(); if (error) *error = relations.lastError().text(); return false; }
    while (relations.next()) {
        StoredEntry &entry = entries[relations.value(0).toString()];
        entry.id = relations.value(0).toString(); entry.name = relations.value(1).toString(); entry.archived = relations.value(2).toInt()!=0;
        entry.references.append(relations.value(3).toString());
    }

    const QString at = nowUtc();
    const QMap<QString, QString> referenceMap = stringMap(request.policy, QStringLiteral("reference_mapping"));
    if (oldType == QStringLiteral("formula")) {
        QVector<ObjectType> formulaTypes;
        QVector<ObjectRecord> formulaObjects;
        if (!loadFormulaObjects(m_database, gameId, &formulaTypes, &formulaObjects, error)) {
            m_database.rollback();
            return false;
        }
        for (const ObjectRecord &object : std::as_const(formulaObjects)) {
            if (object.typeId != typeId) continue;
            if (object.formulaErrors.contains(fieldId) || !object.values.contains(fieldId) || object.values.value(fieldId).isNull()) {
                m_database.rollback();
                if (error) *error = QStringLiteral("A formula result changed or became unavailable after preview. The migration was rolled back.");
                return false;
            }
            QString conversionError;
            const QJsonValue formulaValue = object.values.value(fieldId);
            QJsonValue converted;
            const QByteArray after = convertRawValue(QStringLiteral("decimal"), request.targetType, newOptions,
                request.policy, compactJson(formulaValue), formulaValue, &converted, &conversionError);
            if (!conversionError.isEmpty() || after.isEmpty()) {
                m_database.rollback();
                if (error) *error = conversionError.isEmpty() ? QStringLiteral("Formula value could not be encoded.") : conversionError;
                return false;
            }
            QSqlQuery save(m_database);
            save.prepare(QStringLiteral("INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at"));
            save.addBindValue(object.id);
            save.addBindValue(typeId);
            save.addBindValue(fieldId);
            save.addBindValue(QString::fromUtf8(after));
            save.addBindValue(at);
            if (!save.exec()) { m_database.rollback(); if (error) *error = save.lastError().text(); return false; }
            QString historyError;
            recordHistory(m_database, m_userId, fieldId, name, oldType, request.targetType, object.id, {}, after, at, &historyError);
            if (!historyError.isEmpty()) { m_database.rollback(); if (error) *error = historyError; return false; }
            QSqlQuery touch(m_database);
            touch.prepare(QStringLiteral("UPDATE objects SET updated_at=? WHERE id=?"));
            touch.addBindValue(at);
            touch.addBindValue(object.id);
            if (!touch.exec()) { m_database.rollback(); if (error) *error = touch.lastError().text(); return false; }
        }
    }
    for (const StoredEntry &entry : std::as_const(entries)) {
        QByteArray after;
        QByteArray before = entry.raw;
        QString conversionError;
        if (isReference(oldType) && isReference(request.targetType)) {
            QJsonValue beforeValue;
            if (oldType == QStringLiteral("reference")) beforeValue = entry.references.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(entry.references.first());
            else { QJsonArray refs; for (const QString &ref : entry.references) refs.append(ref); beforeValue = refs; }
            QJsonValue afterValue;
            if (request.targetType == QStringLiteral("reference")) afterValue = entry.references.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(entry.references.first());
            else { QJsonArray refs; for (const QString &ref : entry.references) refs.append(ref); afterValue = refs; }
            before = compactJson(beforeValue);
            after = compactJson(afterValue);
        } else if (isReference(oldType) && (request.targetType == QStringLiteral("text") || request.targetType == QStringLiteral("long_text"))) {
            QJsonArray refs;
            for (const QString &ref : entry.references) refs.append(ref);
            const QString serialized = QString::fromUtf8(QJsonDocument(refs).toJson(QJsonDocument::Compact));
            const QJsonValue converted = serialized;
            after = compactJson(converted);
            QSqlQuery save(m_database);
            save.prepare(QStringLiteral("INSERT INTO field_values(object_id,object_type_id,field_id,value_json,updated_at) VALUES(?,?,?,?,?) ON CONFLICT(object_id,field_id) DO UPDATE SET value_json=excluded.value_json,updated_at=excluded.updated_at"));
            save.addBindValue(entry.id); save.addBindValue(typeId); save.addBindValue(fieldId); save.addBindValue(QString::fromUtf8(after)); save.addBindValue(at);
            if (!save.exec()) { m_database.rollback(); if (error) *error = save.lastError().text(); return false; }
            QSqlQuery remove(m_database); remove.prepare(QStringLiteral("DELETE FROM object_relationships WHERE object_id=? AND field_id=?")); remove.addBindValue(entry.id); remove.addBindValue(fieldId);
            if (!remove.exec()) { m_database.rollback(); if (error) *error = remove.lastError().text(); return false; }
        } else if (isReference(request.targetType) && !isReference(oldType)) {
            const QJsonValue oldValue = parseValue(entry.raw);
            const QString mapped = referenceMap.value(oldValue.toString());
            const QStringList ids{mapped};
            referenceIds(ids, request.targetType, m_database, gameId, newOptions, &conversionError);
            if (!conversionError.isEmpty()) { m_database.rollback(); if (error) *error = conversionError; return false; }
            QSqlQuery link(m_database);
            link.prepare(QStringLiteral("INSERT INTO object_relationships(game_id,object_id,object_type_id,field_id,referenced_object_id) VALUES(?,?,?,?,?)"));
            link.addBindValue(gameId); link.addBindValue(entry.id); link.addBindValue(typeId); link.addBindValue(fieldId); link.addBindValue(mapped);
            if (!link.exec()) { m_database.rollback(); if (error) *error = link.lastError().text(); return false; }
            after = request.targetType == QStringLiteral("reference") ? compactJson(QJsonValue(mapped)) : QJsonDocument(QJsonArray{mapped}).toJson(QJsonDocument::Compact);
            QSqlQuery remove(m_database); remove.prepare(QStringLiteral("DELETE FROM field_values WHERE object_id=? AND field_id=?")); remove.addBindValue(entry.id); remove.addBindValue(fieldId);
            if (!remove.exec()) { m_database.rollback(); if (error) *error = remove.lastError().text(); return false; }
        } else {
            bool parsed = false;
            const QJsonValue oldValue = parseValue(entry.raw, &parsed);
            QJsonValue converted;
            after = convertRawValue(oldType, request.targetType, newOptions, request.policy,
                entry.raw, oldValue, &converted, &conversionError);
            if (!parsed || !conversionError.isEmpty() || after.isEmpty()) { m_database.rollback(); if (error) *error = conversionError.isEmpty() ? QStringLiteral("Saved value is invalid JSON.") : conversionError; return false; }
            QSqlQuery update(m_database);
            update.prepare(QStringLiteral("UPDATE field_values SET value_json=?,updated_at=? WHERE object_id=? AND field_id=?"));
            update.addBindValue(QString::fromUtf8(after)); update.addBindValue(at); update.addBindValue(entry.id); update.addBindValue(fieldId);
            if (!update.exec()) { m_database.rollback(); if (error) *error = update.lastError().text(); return false; }
        }
        if (isReference(oldType) && isReference(request.targetType)) {
            Q_UNUSED(referenceMap);
        }
        QString historyError;
        if (entry.raw != after || isReference(oldType) || isReference(request.targetType))
            recordHistory(m_database, m_userId, fieldId, name, oldType, request.targetType, entry.id, before, after, at, &historyError);
        if (!historyError.isEmpty()) { m_database.rollback(); if (error) *error = historyError; return false; }
        QSqlQuery touch(m_database); touch.prepare(QStringLiteral("UPDATE objects SET updated_at=? WHERE id=?")); touch.addBindValue(at); touch.addBindValue(entry.id);
        if (!touch.exec()) { m_database.rollback(); if (error) *error = touch.lastError().text(); return false; }
    }

    QSqlQuery updateField(m_database);
    updateField.prepare(QStringLiteral("UPDATE field_definitions SET field_type=?,options_json=?,default_json=?,updated_at=? WHERE id=? AND field_type=?"));
    updateField.addBindValue(request.targetType);
    updateField.addBindValue(QString::fromUtf8(QJsonDocument(newOptions).toJson(QJsonDocument::Compact)));
    updateField.addBindValue(newDefault.isNull() || newDefault.isUndefined() ? QVariant() : QVariant(QString::fromUtf8(checked.convertedDefaultJson)));
    updateField.addBindValue(at); updateField.addBindValue(fieldId); updateField.addBindValue(oldType);
    if (!updateField.exec() || updateField.numRowsAffected() != 1) { m_database.rollback(); if (error) *error = updateField.lastError().isValid() ? updateField.lastError().text() : QStringLiteral("Field type changed during migration."); return false; }
    QSqlQuery audit(m_database);
    audit.prepare(QStringLiteral("UPDATE field_type_migrations SET outcome='applied' WHERE id=? AND outcome='applying'"));
    audit.addBindValue(migrationId);
    if (!audit.exec()) { m_database.rollback(); if (error) *error = audit.lastError().text(); return false; }
    if (!m_database.commit()) { if (error) *error = m_database.lastError().text(); m_database.rollback(); return false; }
    checked.targetType = request.targetType;
    if (result) *result = checked;
    return true;
}

} // namespace gmst
