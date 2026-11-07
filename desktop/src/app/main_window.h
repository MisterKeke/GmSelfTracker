#pragma once

#include "domain/game.h"

#include <QMainWindow>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;
class QWidget;

namespace gmst {
class GameRepository;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(GameRepository &repository, QWidget *parent = nullptr);

signals:
    void accountLockRequested();

private:
    enum class Page { Overview, Objects, Goals, Types, Templates };

    void buildUi();
    QWidget *makeSidebar();
    QWidget *makeGameWorkspace();
    QWidget *makePage(const QString &title, const QString &description, const QString &icon,
                      const QString &emptyTitle, const QString &emptyDescription);
    void refreshGames(const QString &preferredId = {});
    void refreshSections();
    void refreshObjectPage();
    void refreshGoalPage();
    void refreshFeaturePages();
    void selectGame(const QString &gameId);
    void selectPage(Page page);
    void openCreateGame();
    void openEditGame();
    void openCreateSection();
    void openCreateType();
    void openEditType(const QString &typeId);
    void openCreateField(const QString &typeId);
    void openEditField(const QString &typeId, const QString &fieldId);
    void openFieldTypeMigration(const QString &typeId, const QString &fieldId);
    void openObjectEditor(const QString &objectId = {});
    void openGoalEditor(const QString &goalId = {});
    void openProfile();
    QString openAssetLibrary(QWidget *parent, const QString &currentReference = {}, bool selectionMode = true);
    Game currentGame() const;

    GameRepository &m_repository;
    QVector<Game> m_games;
    QVector<Section> m_gameSections;
    QVector<ObjectType> m_types;
    QVector<ObjectRecord> m_objects;
    QVector<Goal> m_goals;
    QString m_selectedGameId;
    QString m_selectedSectionId;
    Page m_currentPage = Page::Overview;

    QLineEdit *m_gameSearch = nullptr;
    QWidget *m_gameList = nullptr;
    QVBoxLayout *m_gameListLayout = nullptr;
    QLabel *m_gameCount = nullptr;
    QLabel *m_heroTitle = nullptr;
    QLabel *m_heroDescription = nullptr;
    QLabel *m_heroSymbol = nullptr;
    QLabel *m_gameTotalStat = nullptr;
    QLabel *m_objectTotalStat = nullptr;
    QLabel *m_sectionTotalStat = nullptr;
    QLabel *m_goalDoneStat = nullptr;
    QTreeWidget *m_sections = nullptr;
    QStackedWidget *m_pages = nullptr;
    QButtonGroup *m_tabGroup = nullptr;
    QVector<QVBoxLayout *> m_pageLayouts;
    QVector<QWidget *> m_emptyStates;
    QVector<QWidget *> m_featureViews;
    QPushButton *m_newObjectButton = nullptr;
    QPushButton *m_addSectionObjectButton = nullptr;
    QPushButton *m_newGoalButton = nullptr;
    QPushButton *m_newTypeButton = nullptr;
    QPushButton *m_editGameButton = nullptr;
    QWidget *m_goalFilters = nullptr;
    QLineEdit *m_goalSearch = nullptr;
    QComboBox *m_goalStatus = nullptr;
    QComboBox *m_goalType = nullptr;
    QComboBox *m_goalSort = nullptr;
    int m_goalPage = 0;
};

} // namespace gmst
