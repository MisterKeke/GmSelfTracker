#pragma once

#include "domain/game.h"

#include <QMap>
#include <QSqlDatabase>
#include <QString>

namespace gmst {

class SchemaManagementService {
public:
    SchemaManagementService(QSqlDatabase database, QString userId);

    bool updateObjectType(const ObjectType &type, QString *error = nullptr) const;
    bool setObjectTypeArchived(const QString &typeId, bool archived, QString *error = nullptr) const;
    bool setPrimaryDisplayField(const QString &typeId, const QString &fieldId, QString *error = nullptr) const;
    bool updateField(const FieldDefinition &field, const QMap<QString, QString> &choiceReplacements,
                     QString *error = nullptr);
    bool setFieldArchived(const QString &fieldId, bool archived, QString *error = nullptr) const;

private:
    QSqlDatabase m_database;
    QString m_userId;
};

} // namespace gmst
