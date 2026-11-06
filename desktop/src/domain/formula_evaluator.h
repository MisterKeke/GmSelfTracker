#pragma once

#include "domain/game.h"

namespace gmst {

// Evaluates the arithmetic language used by the Go implementation. Formula
// values are derived in memory and are never written to field_values.
class FormulaEvaluator {
public:
    static bool validateExpression(const QString &expression, QString *error = nullptr);
    static QStringList references(const QString &expression);
    static QMap<QString, QString> evaluate(const QVector<FieldDefinition> &fields,
                                           ObjectRecord &object,
                                           const QVector<ObjectType> &types,
                                           const QVector<ObjectRecord> &objects);
};

} // namespace gmst
