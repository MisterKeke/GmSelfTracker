#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace gmst {

struct Game {
    QString id;
    QString name;
    QString description;
    QString icon;
    QString coverImage;
    int objectCount = 0;
    int sectionCount = 0;
    int goalCount = 0;
    int completedGoals = 0;
};

struct Section {
    QString id;
    QString gameId;
    QString parentId;
    QString name;
    QString icon;
    int sortOrder = 0;
};

struct FieldDefinition {
    QString id;
    QString objectTypeId;
    QString name;
    QString fieldType;
    QString icon;
    QString description;
    QJsonValue defaultValue;
    QJsonObject options;
    int sortOrder = 0;
    bool archived = false;
};

struct ObjectType {
    QString id;
    QString gameId;
    QString name;
    QString description;
    QString icon;
    QVector<FieldDefinition> fields;
};

struct ObjectRecord {
    QString id;
    QString gameId;
    QString typeId;
    QString typeName;
    QString name;
    QString description;
    QString image;
    QMap<QString, QJsonValue> values;
    QMap<QString, QStringList> references;
    QStringList sectionIds;
};

struct GoalTask {
    QString id;
    QString title;
    bool completed = false;
    int sortOrder = 0;
};

struct Goal {
    QString id;
    QString gameId;
    QString title;
    QString description;
    QString goalType;
    QString objectId;
    QString fieldId;
    QJsonValue target;
    double current = 0;
    bool completed = false;
    double progress = 0;
    QVector<GoalTask> tasks;
    QStringList prerequisiteIds;
    QStringList prerequisiteTitles;
    QStringList blockedBy;
    bool ready = true;
    QString createdAt;
    QString updatedAt;
};

struct ArchivedEntry {
    QString id;
    QString kind;
    QString gameName;
    QString name;
};

} // namespace gmst
