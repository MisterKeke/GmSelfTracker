#include "domain/formula_evaluator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>

namespace gmst {
namespace {

class Parser {
public:
    Parser(const QString &input, const QMap<QString, double> &values, bool syntaxOnly)
        : m_input(input), m_values(values), m_syntaxOnly(syntaxOnly) {}

    bool parse(double *result, QString *error)
    {
        double value = 0;
        if (!expression(&value, error)) return false;
        space();
        if (m_position != m_input.size()) return fail(error, QStringLiteral("unexpected character at position %1").arg(m_position + 1));
        if (!m_syntaxOnly && (!std::isfinite(value))) return fail(error, QStringLiteral("formula result is not finite"));
        if (result) *result = value;
        return true;
    }

private:
    bool expression(double *value, QString *error)
    {
        if (!term(value, error)) return false;
        while (true) {
            space();
            if (take(QLatin1Char('+'))) {
                double right = 0;
                if (!term(&right, error)) return false;
                *value += right;
            } else if (take(QLatin1Char('-'))) {
                double right = 0;
                if (!term(&right, error)) return false;
                *value -= right;
            } else return true;
        }
    }

    bool term(double *value, QString *error)
    {
        if (!factor(value, error)) return false;
        while (true) {
            space();
            if (take(QLatin1Char('*'))) {
                double right = 0;
                if (!factor(&right, error)) return false;
                *value *= right;
            } else if (take(QLatin1Char('/'))) {
                double right = 0;
                if (!factor(&right, error)) return false;
                if (!m_syntaxOnly && right == 0) return fail(error, QStringLiteral("division by zero"));
                if (!m_syntaxOnly) *value /= right;
            } else return true;
        }
    }

    bool factor(double *value, QString *error)
    {
        space();
        if (take(QLatin1Char('+'))) return factor(value, error);
        if (take(QLatin1Char('-'))) {
            if (!factor(value, error)) return false;
            *value = -*value;
            return true;
        }
        if (take(QLatin1Char('('))) {
            if (!expression(value, error)) return false;
            space();
            return take(QLatin1Char(')')) || fail(error, QStringLiteral("expected closing parenthesis"));
        }
        if (m_position >= m_input.size()) return fail(error, QStringLiteral("expected a number or field reference"));
        if (m_input.at(m_position) == QLatin1Char('@')) {
            ++m_position;
            bool braced = take(QLatin1Char('{'));
            const qsizetype start = m_position;
            if (braced) {
                while (m_position < m_input.size() && m_input.at(m_position) != QLatin1Char('}')) ++m_position;
            } else {
                while (m_position < m_input.size() && !QStringLiteral(" \t\r\n+-*/()").contains(m_input.at(m_position))) ++m_position;
            }
            if (m_position == start) return fail(error, QStringLiteral("field reference is missing an ID"));
            const QString id = m_input.mid(start, m_position - start);
            if (braced) {
                if (!take(QLatin1Char('}'))) return fail(error, QStringLiteral("field reference is missing a closing brace"));
            }
            if (m_syntaxOnly) { *value = 1; return true; }
            const auto found = m_values.constFind(id);
            if (found == m_values.cend()) return fail(error, QStringLiteral("field %1 has no numeric value").arg(id));
            *value = found.value();
            return true;
        }
        const qsizetype start = m_position;
        while (m_position < m_input.size()) {
            const QChar ch = m_input.at(m_position);
            if (!(ch.isDigit() || ch == QLatin1Char('.'))) break;
            ++m_position;
        }
        if (m_position == start) return fail(error, QStringLiteral("unexpected character at position %1").arg(m_position + 1));
        bool okay = false;
        const double parsed = m_input.mid(start, m_position - start).toDouble(&okay);
        if (!okay) return fail(error, QStringLiteral("invalid number"));
        *value = parsed;
        return true;
    }

    void space()
    {
        while (m_position < m_input.size() && QStringLiteral(" \t\r\n").contains(m_input.at(m_position))) ++m_position;
    }
    bool take(QChar ch)
    {
        if (m_position < m_input.size() && m_input.at(m_position) == ch) { ++m_position; return true; }
        return false;
    }
    bool fail(QString *error, const QString &message)
    {
        if (error) *error = message;
        return false;
    }

    const QString &m_input;
    const QMap<QString, double> &m_values;
    qsizetype m_position = 0;
    bool m_syntaxOnly = false;
};

bool numericValue(const QJsonValue &value, double *number)
{
    if (value.isDouble()) { *number = value.toDouble(); return std::isfinite(*number); }
    if (value.isObject()) {
        const QJsonValue current = value.toObject().value(QStringLiteral("current"));
        if (current.isDouble()) { *number = current.toDouble(); return std::isfinite(*number); }
    }
    return false;
}

} // namespace

bool FormulaEvaluator::validateExpression(const QString &expression, QString *error)
{
    if (expression.toUtf8().size() > 4096) {
        if (error) *error = QStringLiteral("formula exceeds 4096 bytes");
        return false;
    }
    static const QMap<QString, double> empty;
    return Parser(expression, empty, true).parse(nullptr, error);
}

QStringList FormulaEvaluator::references(const QString &expression)
{
    QStringList result;
    QSet<QString> seen;
    for (qsizetype i = 0; i < expression.size(); ++i) {
        if (expression.at(i) != QLatin1Char('@')) continue;
        qsizetype start = i + 1;
        if (start < expression.size() && expression.at(start) == QLatin1Char('{')) {
            ++start;
            i = start;
            while (i < expression.size() && expression.at(i) != QLatin1Char('}')) ++i;
            if (i < expression.size() && i > start) {
                const QString id = expression.mid(start, i - start);
                if (!seen.contains(id)) { seen.insert(id); result.append(id); }
            }
        } else {
            i = start;
            while (i < expression.size() && !QStringLiteral(" \t\r\n+-*/()").contains(expression.at(i))) ++i;
            if (i > start) {
                const QString id = expression.mid(start, i - start);
                if (!seen.contains(id)) { seen.insert(id); result.append(id); }
            }
        }
        --i;
    }
    return result;
}

QMap<QString, QString> FormulaEvaluator::evaluate(const QVector<FieldDefinition> &fields,
                                                   ObjectRecord &object,
                                                   const QVector<ObjectType> &types,
                                                   const QVector<ObjectRecord> &objects)
{
    QMap<QString, FieldDefinition> ownFields;
    QMap<QString, ObjectType> typeById;
    QMap<QString, ObjectRecord> objectById;
    for (const FieldDefinition &field : fields) {
        ownFields.insert(field.id, field);
        if (!field.archived && !field.defaultValue.isUndefined() && !object.values.contains(field.id))
            object.values.insert(field.id, field.defaultValue);
    }
    for (const ObjectType &type : types) {
        typeById.insert(type.id, type);
    }
    for (const ObjectRecord &item : objects) {
        objectById.insert(item.id, item);
        const auto type = typeById.constFind(item.typeId);
        if (type == typeById.cend()) continue;
        for (const FieldDefinition &field : type->fields) {
            if (!field.archived && !field.defaultValue.isUndefined() && !objectById[item.id].values.contains(field.id))
                objectById[item.id].values.insert(field.id, field.defaultValue);
        }
    }

    QMap<QString, double> memo;
    QSet<QString> visiting;
    QSet<QString> done;
    QMap<QString, QString> errors;
    std::function<bool(const QString &, double *, QString *)> resolve;
    resolve = [&](const QString &id, double *result, QString *error) -> bool {
        if (done.contains(id)) { *result = memo.value(id); return true; }
        const auto fieldIt = ownFields.constFind(id);
        if (fieldIt == ownFields.cend() || fieldIt->archived) {
            if (error) *error = QStringLiteral("field %1 is unavailable or archived").arg(id);
            return false;
        }
        if (fieldIt->fieldType != QStringLiteral("formula")) {
            double value = 0;
            const QJsonValue raw = object.values.value(id);
            if (raw.isUndefined() || raw.isNull()) {
                if (error) *error = QStringLiteral("enter a value for %1").arg(fieldIt->name);
                return false;
            }
            if (!numericValue(raw, &value)) {
                if (error) *error = QStringLiteral("%1 has no numeric value").arg(fieldIt->name);
                return false;
            }
            memo.insert(id, value);
            done.insert(id);
            *result = value;
            return true;
        }
        if (visiting.contains(id)) {
            if (error) *error = QStringLiteral("circular formula dependency");
            return false;
        }
        if (visiting.size() >= 128) {
            if (error) *error = QStringLiteral("formula dependency limit exceeded");
            return false;
        }
        visiting.insert(id);
        const QJsonObject options = fieldIt->options;
        const QString expression = options.value(QStringLiteral("expression")).toString();
        if (expression.isEmpty()) {
            visiting.remove(id);
            if (error) *error = QStringLiteral("formula configuration is invalid");
            return false;
        }
        QMap<QString, double> inputs;
        for (const QString &ref : references(expression)) {
            if (ref == QStringLiteral("related")) continue;
            double value = 0;
            QString detail;
            if (!resolve(ref, &value, &detail)) {
                visiting.remove(id);
                if (error) *error = detail;
                return false;
            }
            inputs.insert(ref, value);
        }

        const QJsonObject aggregation = options.value(QStringLiteral("aggregation")).toObject();
        if (references(expression).contains(QStringLiteral("related")) && !aggregation.isEmpty()) {
            const QString relationshipId = aggregation.value(QStringLiteral("relationship_field_id")).toString();
            const QString targetFieldId = aggregation.value(QStringLiteral("field_id")).toString();
            const QString operation = aggregation.value(QStringLiteral("operation")).toString();
            const auto relation = ownFields.constFind(relationshipId);
            if (relation == ownFields.cend() || relation->archived ||
                (relation->fieldType != QStringLiteral("reference") && relation->fieldType != QStringLiteral("references"))) {
                visiting.remove(id);
                if (error) *error = QStringLiteral("relationship aggregation field is unavailable");
                return false;
            }
            int count = 0;
            double aggregate = 0;
            for (const QString &targetId : object.references.value(relationshipId)) {
                const auto target = objectById.constFind(targetId);
                if (target == objectById.cend() || target->gameId != object.gameId || target->archived) continue;
                const auto targetType = typeById.constFind(target->typeId);
                if (targetType == typeById.cend() || targetType->archived) continue;
                const QString allowedType = relation->options.value(QStringLiteral("allowed_type_id")).toString();
                if (!allowedType.isEmpty() && target->typeId != allowedType) continue;
                if (operation == QStringLiteral("count")) { ++count; continue; }
                const auto targetField = std::find_if(targetType->fields.cbegin(), targetType->fields.cend(), [&](const FieldDefinition &item) { return item.id == targetFieldId; });
                if (targetField == targetType->fields.cend() || targetField->archived) continue;
                if (targetField->fieldType != QStringLiteral("integer") && targetField->fieldType != QStringLiteral("decimal") &&
                    targetField->fieldType != QStringLiteral("percentage") && targetField->fieldType != QStringLiteral("progress")) continue;
                const QJsonValue raw = target->values.contains(targetFieldId) ? target->values.value(targetFieldId) : targetField->defaultValue;
                double value = 0;
                if (!numericValue(raw, &value)) continue;
                if (count == 0 && (operation == QStringLiteral("min") || operation == QStringLiteral("max"))) aggregate = value;
                else if (operation == QStringLiteral("min")) aggregate = std::min(aggregate, value);
                else if (operation == QStringLiteral("max")) aggregate = std::max(aggregate, value);
                else aggregate += value;
                ++count;
            }
            if (operation == QStringLiteral("count")) aggregate = count;
            else if (operation == QStringLiteral("avg") || operation == QStringLiteral("min") || operation == QStringLiteral("max")) {
                if (count == 0) {
                    visiting.remove(id);
                    if (error) *error = QStringLiteral("no linked numeric values");
                    return false;
                }
                if (operation == QStringLiteral("avg")) aggregate /= count;
            }
            if (!std::isfinite(aggregate)) {
                visiting.remove(id);
                if (error) *error = QStringLiteral("aggregation result is not finite");
                return false;
            }
            inputs.insert(QStringLiteral("related"), aggregate);
        }
        double value = 0;
        QString detail;
        const QByteArray expressionBytes = expression.toUtf8();
        if (!Parser(QString::fromUtf8(expressionBytes), inputs, false).parse(&value, &detail)) {
            visiting.remove(id);
            if (error) *error = detail;
            return false;
        }
        visiting.remove(id);
        done.insert(id);
        memo.insert(id, value);
        object.values.insert(id, value);
        *result = value;
        return true;
    };

    for (const FieldDefinition &field : fields) {
        if (field.archived || field.fieldType != QStringLiteral("formula")) continue;
        double value = 0;
        QString error;
        if (!resolve(field.id, &value, &error)) errors.insert(field.id, error);
    }
    object.formulaErrors = errors;
    return errors;
}

} // namespace gmst
