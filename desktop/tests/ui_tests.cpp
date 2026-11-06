#include "app/main_window.h"
#include "infrastructure/database/game_repository.h"

#include <QFrame>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest>

using namespace gmst;

class UiTests final : public QObject {
    Q_OBJECT

private slots:
    void goalFiltersRunBeforePagination()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("goals-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QVERIFY(repository.createGame(QStringLiteral("Goal fixture"), {}, {}, &gameId, &error));
        for (int index = 0; index < 22; ++index) {
            Goal goal;
            goal.gameId = gameId;
            goal.title = QStringLiteral("Milestone %1").arg(index);
            goal.goalType = QStringLiteral("manual");
            goal.completed = index == 21;
            QVERIFY2(repository.saveGoal(goal, {}, &error), qPrintable(error));
        }
        MainWindow window(repository);
        const auto cards = [&] { return window.findChildren<QFrame *>(QStringLiteral("goalCard")).size(); };
        QCOMPARE(cards(), 20);
        auto *next = window.findChild<QPushButton *>(QStringLiteral("goalNextPage"));
        QVERIFY(next);
        next->click();
        QCOMPARE(cards(), 2);
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("goalSearch"));
        QVERIFY(search);
        search->setText(QStringLiteral("mILESTONE 21"));
        QCOMPARE(cards(), 1);
        auto *status = window.findChild<QComboBox *>(QStringLiteral("goalStatusFilter"));
        QVERIFY(status);
        status->setCurrentIndex(1);
        QCOMPARE(cards(), 0);
        status->setCurrentIndex(2);
        QCOMPARE(cards(), 1);
    }

    void sectionSelectionShowsPlacementsAndObjectHistory()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("section-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        ObjectRecord object;
        object.name = QStringLiteral("Placed");
        QVERIFY(repository.createGame(QStringLiteral("UI fixture"), {}, {}, &object.gameId, &error));
        QVERIFY(repository.createObjectType(object.gameId, QStringLiteral("Item"), {}, {}, &object.typeId, &error));
        QVERIFY(repository.createSection(object.gameId, {}, QStringLiteral("Root"), {}, &error));
        const auto sections = repository.sections(object.gameId);
        QCOMPARE(sections.size(), 1);
        const QString rootId = sections.first().id;
        QVERIFY(repository.createSection(object.gameId, rootId, QStringLiteral("Child"), {}, &error));
        QVERIFY2(repository.saveObject(object, {}, rootId, &error), qPrintable(error));
        object.id.clear();
        object.name = QStringLiteral("Unplaced");
        QVERIFY2(repository.saveObject(object, {}, {}, &error), qPrintable(error));

        MainWindow window(repository);
        auto *tree = window.findChild<QTreeWidget *>();
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 2);
        auto *root = tree->topLevelItem(1);
        QCOMPARE(root->childCount(), 1);
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("objectCard")).size(), 2);
        tree->setCurrentItem(root);
        const auto cards = window.findChildren<QFrame *>(QStringLiteral("objectCard"));
        QCOMPARE(cards.size(), 1);
        auto *history = cards.first()->findChild<QPushButton *>(QStringLiteral("objectHistoryButton"));
        QVERIFY(history);
        bool opened = false;
        int rows = -1;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            opened = dialog->objectName() == QStringLiteral("objectHistoryDialog");
            if (const auto *timeline = dialog->findChild<QTableWidget *>(QStringLiteral("historyTimeline"))) rows = timeline->rowCount();
            dialog->reject();
        });
        history->click();
        QVERIFY(opened);
        QCOMPARE(rows, 1);
        tree->setCurrentItem(root->child(0));
        QVERIFY(window.findChildren<QFrame *>(QStringLiteral("objectCard")).isEmpty());
        tree->setCurrentItem(tree->topLevelItem(0));
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("objectCard")).size(), 2);
    }

    void selectedGameAndPrimaryActionAreRendered()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QVERIFY2(repository.createGame(QStringLiteral("UI fixture"), QStringLiteral("Disposable"), QStringLiteral("🎮"), &gameId, &error), qPrintable(error));
        Goal goal;
        goal.gameId = gameId;
        goal.title = QStringLiteral("Finish the opening quest");
        goal.goalType = QStringLiteral("manual");
        QVERIFY2(repository.saveGoal(goal, {}, &error), qPrintable(error));

        MainWindow window(repository);
        const QList<QPushButton *> gameButtons = window.findChildren<QPushButton *>(QStringLiteral("gameButton"));
        QCOMPARE(gameButtons.size(), 1);
        QCOMPARE(gameButtons.first()->text(), QStringLiteral("🎮   UI fixture"));
        QVERIFY(gameButtons.first()->isChecked());
        auto *newObject = window.findChild<QPushButton *>(QStringLiteral("newObjectButton"));
        auto *newGoal = window.findChild<QPushButton *>(QStringLiteral("newGoalButton"));
        auto *newType = window.findChild<QPushButton *>(QStringLiteral("newTypeButton"));
        QVERIFY(newObject);
        QVERIFY(newGoal);
        QVERIFY(newType);
        QVERIFY(newObject->isEnabled());
        QVERIFY(newGoal->isEnabled());
        QVERIFY(newType->isEnabled());
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("goalCard")).size(), 1);
        QCOMPARE(window.findChildren<QProgressBar *>(QStringLiteral("goalProgressBar")).size(), 1);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *title = dialog->findChild<QLineEdit *>(QStringLiteral("goalTitleInput"));
            if (!title) return;
            title->setText(QStringLiteral("Created in the native editor"));
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>()) {
                if (candidate->text() == QStringLiteral("Create Goal")) {
                    candidate->click();
                    return;
                }
            }
        });
        newGoal->click();
        QCOMPARE(repository.goals(gameId, &error).size(), 2);
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("goalCard")).size(), 2);

        QString archivedGameId;
        QVERIFY2(repository.createGame(QStringLiteral("Restorable game"), {}, {}, &archivedGameId, &error), qPrintable(error));
        QVERIFY2(repository.archiveGame(archivedGameId, &error), qPrintable(error));
        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            const auto restoreButtons = dialog->findChildren<QPushButton *>(QStringLiteral("restoreArchivedButton"));
            if (!restoreButtons.isEmpty()) restoreButtons.first()->click();
        });
        auto *settings = window.findChild<QPushButton *>(QStringLiteral("settingsButton"));
        QVERIFY(settings);
        settings->click();
        QCOMPARE(repository.games(&error).size(), 2);
        QVERIFY(repository.archivedEntries(&error).isEmpty());
    }
};

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
