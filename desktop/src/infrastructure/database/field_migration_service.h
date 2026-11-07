#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

namespace gmst {

struct FieldMigrationRequest {
    QString targetType;
    QJsonObject targetOptions;
    QJsonObject policy;
    bool defaultOverrideProvided = false;
    QJsonValue defaultOverride;
};

struct FieldMigrationExample {
    QString objectId;
    QString objectName;
    QJsonValue value;
    QString reason;
};

struct FieldMigrationPreview {
    QString fieldId;
    QString fieldName;
    QString previousType;
    QString targetType;
    QString previewToken;
    int affectedObjects = 0;
    int archivedObjectsAffected = 0;
    int savedValues = 0;
    int compatibleValues = 0;
    int incompatibleValues = 0;
    int presetOverrides = 0;
    QJsonValue convertedDefault;
    QByteArray convertedDefaultJson;
    QString defaultError;
    QString formulaWarning;
    QStringList dependencies;
    QStringList dependencyConflicts;
    QStringList blockers;
    QVector<FieldMigrationExample> examples;
    bool canApply = false;
};

class FieldMigrationService {
public:
    FieldMigrationService(QSqlDatabase database, QString userId);

    FieldMigrationPreview preview(const QString &fieldId, const FieldMigrationRequest &request,
                                  QString *error = nullptr) const;
    bool apply(const QString &fieldId, const FieldMigrationRequest &request, const QString &previewToken,
               FieldMigrationPreview *result = nullptr, QString *error = nullptr);

private:
    QSqlDatabase m_database;
    QString m_userId;
};

} // namespace gmst
