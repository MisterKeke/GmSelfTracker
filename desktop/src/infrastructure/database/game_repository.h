#pragma once

#include "domain/game.h"
#include "domain/object_history.h"

#include <QSqlDatabase>
#include <QImage>
#include <QString>
#include <QVector>

namespace gmst {

class GameRepository {
public:
    explicit GameRepository(const QString &databasePath);
    ~GameRepository();
    GameRepository(const GameRepository &) = delete;
    GameRepository &operator=(const GameRepository &) = delete;

    bool open(QString *error = nullptr);
    bool isOpen() const;
    QVector<Game> games(QString *error = nullptr) const;
    QVector<Section> sections(const QString &gameId, QString *error = nullptr) const;
    QVector<ObjectType> objectTypes(const QString &gameId, QString *error = nullptr) const;
    QVector<ObjectRecord> objects(const QString &gameId, QString *error = nullptr) const;
    QMap<QString, QString> historyFields(const QString &objectId, QString *error = nullptr) const;
    HistoryPage objectHistory(const QString &objectId, const HistoryFilter &filter, QString *error = nullptr) const;
    QVector<Goal> goals(const QString &gameId, QString *error = nullptr) const;
    QVector<ArchivedEntry> archivedEntries(QString *error = nullptr) const;
    bool createGame(const QString &name, const QString &description, const QString &icon, QString *createdId = nullptr,
                    QString *error = nullptr, const QString &personalNotes = {}, const QString &coverImage = {});
    bool updateGame(const Game &game, QString *error = nullptr);
    bool archiveGame(const QString &gameId, QString *error = nullptr);
    bool archiveSection(const QString &sectionId, QString *error = nullptr);
    bool restoreArchivedEntry(const ArchivedEntry &entry, QString *error = nullptr);
    bool createSection(const QString &gameId, const QString &parentId, const QString &name, const QString &icon, QString *error = nullptr);
    bool createObjectType(const QString &gameId, const QString &name, const QString &description,
                          const QString &icon, QString *createdId = nullptr, QString *error = nullptr, const QString &image = {});
    bool createField(const QString &typeId, const QString &name, const QString &fieldType,
                     const QString &icon, const QJsonObject &options, QString *createdId = nullptr, QString *error = nullptr,
                     const QString &description = {}, const QJsonValue &defaultValue = {});
    bool saveObject(ObjectRecord &object, const QVector<FieldDefinition> &fields,
                    const QString &sectionId = {}, QString *error = nullptr);
    bool archiveObject(const QString &objectId, QString *error = nullptr);
    bool setObjectPlacement(const QString &objectId, const QString &sectionId, bool placed,
                            QString *error = nullptr);
    bool saveGoal(Goal &goal, const QVector<GoalTask> &tasks, QString *error = nullptr);
    bool setGoalDependencies(const QString &goalId, const QStringList &prerequisiteIds, QString *error = nullptr);
    bool setGoalCompleted(const QString &goalId, bool completed, QString *error = nullptr);
    bool setGoalTaskCompleted(const QString &taskId, bool completed, QString *error = nullptr);
    bool setNumericGoalCurrent(const QString &goalId, double current, QString *error = nullptr);
    bool deleteGoal(const QString &goalId, QString *error = nullptr);
    QVector<UploadedAsset> uploadedAssets(QString *error = nullptr);
    bool uploadAsset(const QByteArray &encoded, UploadedAsset *created = nullptr, QString *error = nullptr);
    bool deleteAsset(const QString &assetId, bool *pendingDeletion = nullptr, QString *error = nullptr);
    QImage loadAssetImage(const QString &reference, QString *error = nullptr) const;
    bool retryAssetCleanup(QString *error = nullptr);
    QString databasePath() const;

private:
    bool ensureLocalProfile(QString *error);
    bool cleanupPendingAssets(QString *error = nullptr);
    bool resolveAssetPath(const QString &filename, QString *path, QString *error = nullptr) const;
    QString m_path;
    QString m_assetRoot;
    QString m_connectionName;
    QSqlDatabase m_database;
    QString m_userId = QStringLiteral("native-desktop-local");
};

} // namespace gmst
