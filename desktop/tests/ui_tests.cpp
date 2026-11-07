#include "app/main_window.h"
#include "app/account_dialog.h"
#include "infrastructure/database/game_repository.h"

#include <QFrame>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest>

using namespace gmst;

class UiTests final : public QObject {
    Q_OBJECT

private slots:
    void accountDialogRegistersLogsInAndKeepsAccountsIsolated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QCoreApplication::setOrganizationName(QStringLiteral("GmSelfTrackerUiTests"));
        QCoreApplication::setApplicationName(QStringLiteral("AccountFlow"));
        GameRepository repository(directory.filePath(QStringLiteral("accounts-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));

        AccountDialog registration(repository);
        registration.findChild<QLineEdit *>(QStringLiteral("accountUsername"))->setText(QStringLiteral("first_local"));
        registration.findChild<QLineEdit *>(QStringLiteral("accountPassword"))->setText(QStringLiteral("first-password-123"));
        registration.findChild<QLineEdit *>(QStringLiteral("accountConfirmPassword"))->setText(QStringLiteral("first-password-123"));
        registration.findChild<QPushButton *>(QStringLiteral("accountRegisterButton"))->click();
        QCOMPARE(registration.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(repository.activeUsername(), QStringLiteral("first_local"));
        QString firstGame;
        QVERIFY2(repository.createGame(QStringLiteral("First account game"), {}, {}, &firstGame, &error), qPrintable(error));

        AccountDialog secondRegistration(repository);
        secondRegistration.findChild<QLineEdit *>(QStringLiteral("accountUsername"))->setText(QStringLiteral("second_local"));
        secondRegistration.findChild<QLineEdit *>(QStringLiteral("accountPassword"))->setText(QStringLiteral("second-password-456"));
        secondRegistration.findChild<QLineEdit *>(QStringLiteral("accountConfirmPassword"))->setText(QStringLiteral("second-password-456"));
        secondRegistration.findChild<QPushButton *>(QStringLiteral("accountRegisterButton"))->click();
        QCOMPARE(secondRegistration.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(repository.games(&error).isEmpty());

        AccountDialog signIn(repository);
        signIn.findChild<QLineEdit *>(QStringLiteral("accountUsername"))->setText(QStringLiteral("first_local"));
        signIn.findChild<QLineEdit *>(QStringLiteral("accountPassword"))->setText(QStringLiteral("incorrect-password"));
        signIn.findChild<QPushButton *>(QStringLiteral("accountLoginButton"))->click();
        QCOMPARE(signIn.result(), 0);
        QVERIFY(!signIn.findChild<QLabel *>(QStringLiteral("accountNotice"))->text().isEmpty());
        signIn.findChild<QLineEdit *>(QStringLiteral("accountPassword"))->setText(QStringLiteral("first-password-123"));
        signIn.findChild<QPushButton *>(QStringLiteral("accountLoginButton"))->click();
        QCOMPARE(signIn.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(repository.activeUsername(), QStringLiteral("first_local"));
        QCOMPARE(repository.games(&error).size(), 1);
        QCOMPARE(repository.games(&error).first().id, firstGame);
    }

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

    void formulaFieldCanBeCreatedWithAnUnsavedPreview()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("formula-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        ObjectRecord object;
        QVERIFY(repository.createGame(QStringLiteral("Formula UI"), {}, {}, &object.gameId, &error));
        QVERIFY(repository.createObjectType(object.gameId, QStringLiteral("Item"), {}, {}, &object.typeId, &error));
        QString scoreId;
        QVERIFY(repository.createField(object.typeId, QStringLiteral("Score"), QStringLiteral("integer"), {}, {}, &scoreId, &error));
        const auto types = repository.objectTypes(object.gameId);
        object.name = QStringLiteral("Example");
        object.values.insert(scoreId, 7);
        QVERIFY(repository.saveObject(object, types.first().fields, {}, &error));
        MainWindow window(repository);
        for (QPushButton *tab : window.findChildren<QPushButton *>(QStringLiteral("tabButton")))
            if (tab->text().contains(QStringLiteral("Types"))) tab->click();
        auto *addField = window.findChild<QPushButton *>(QStringLiteral("addFieldButton"));
        if (!addField) {
            for (QPushButton *candidate : window.findChildren<QPushButton *>())
                if (candidate->text().contains(QStringLiteral("Add Field"))) { addField = candidate; break; }
        }
        QVERIFY(addField);
        QString previewText;
        bool fieldSaved = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *kind = dialog->findChild<QComboBox *>(QStringLiteral("fieldTypeChoice"));
            auto *expression = dialog->findChild<QTextEdit *>(QStringLiteral("formulaExpression"));
            auto *example = dialog->findChild<QComboBox *>(QStringLiteral("formulaPreviewObject"));
            auto *preview = dialog->findChild<QLabel *>(QStringLiteral("formulaPreview"));
            if (!kind || !expression || !example || !preview) { dialog->reject(); return; }
            kind->setCurrentIndex(kind->findData(QStringLiteral("formula")));
            expression->setPlainText(QStringLiteral("@{%1} * 2").arg(scoreId));
            example->setCurrentIndex(example->findData(object.id));
            QApplication::processEvents();
            previewText = preview->text();
            QLineEdit *name = dialog->findChild<QLineEdit *>();
            if (!name) { dialog->reject(); return; }
            name->setText(QStringLiteral("Double score"));
            QPushButton *createFormula = nullptr;
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Add Field")) { createFormula = candidate; break; }
            if (!createFormula) { dialog->reject(); return; }
            createFormula->click();
            fieldSaved = !dialog->isVisible();
        });
        addField->click();
        QVERIFY(fieldSaved);
        QVERIFY(previewText.contains(QStringLiteral("14")));
        const auto after = repository.objectTypes(object.gameId);
        const auto formulaField = std::find_if(after.first().fields.cbegin(), after.first().fields.cend(),
            [](const FieldDefinition &field) { return field.name == QStringLiteral("Double score"); });
        QVERIFY(formulaField != after.first().fields.cend());
        const auto objects = repository.objects(object.gameId);
        QCOMPARE(objects.first().values.value(formulaField->id).toDouble(), 14.0);

        QLabel *formulaLabel = nullptr;
        for (QLabel *candidate : window.findChildren<QLabel *>(QStringLiteral("typeFieldRowTitle")))
            if (candidate->property("fieldId").toString() == formulaField->id) formulaLabel = candidate;
        QVERIFY(formulaLabel);
        auto *editFormula = formulaLabel->parentWidget()->findChild<QPushButton *>(QStringLiteral("editFieldButton"));
        QVERIFY(editFormula);
        bool formulaEdited = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *expression = dialog->findChild<QTextEdit *>(QStringLiteral("fieldEditFormulaExpression"));
            if (!expression) { dialog->reject(); return; }
            expression->setPlainText(QStringLiteral("@{%1} * 3").arg(scoreId));
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Save Field")) { candidate->click(); formulaEdited = true; break; }
        });
        editFormula->click();
        QVERIFY(formulaEdited);
        const auto editedTypes = repository.objectTypes(object.gameId);
        const auto editedFormula = std::find_if(editedTypes.first().fields.cbegin(), editedTypes.first().fields.cend(),
            [&](const FieldDefinition &field) { return field.id == formulaField->id; });
        QVERIFY(editedFormula != editedTypes.first().fields.cend());
        QCOMPARE(editedFormula->options.value(QStringLiteral("expression")).toString(), QStringLiteral("@{%1} * 3").arg(scoreId));
        QCOMPARE(repository.objects(object.gameId).first().values.value(formulaField->id).toDouble(), 21.0);
    }

    void gameEditorSelectsAnUploadedCoverAndKeepsPersonalNotes()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("asset-picker-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QImage source(8, 8, QImage::Format_ARGB32);
        source.fill(QColor(20, 140, 220));
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        UploadedAsset asset;
        QVERIFY2(repository.uploadAsset(encoded, &asset, &error), qPrintable(error));
        QString gameId;
        QVERIFY2(repository.createGame(QStringLiteral("Image game"), QStringLiteral("Description"), QStringLiteral("🎮"),
            &gameId, &error, QStringLiteral("Initial notes")), qPrintable(error));

        MainWindow window(repository);
        QPushButton *edit = nullptr;
        for (QPushButton *candidate : window.findChildren<QPushButton *>())
            if (candidate->text().contains(QStringLiteral("Edit"))) { edit = candidate; break; }
        QVERIFY(edit);
        bool coverSelected = false;
        bool saved = false;
        QTimer::singleShot(0, [&] {
            auto *gameDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!gameDialog) return;
            auto *cover = gameDialog->findChild<QLineEdit *>(QStringLiteral("gameCoverImageInput"));
            auto *notes = gameDialog->findChild<QTextEdit *>(QStringLiteral("gamePersonalNotesInput"));
            QPushButton *choose = nullptr;
            for (QPushButton *candidate : gameDialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Choose…")) { choose = candidate; break; }
            if (!cover || !notes || !choose) { gameDialog->reject(); return; }
            QTimer::singleShot(0, [&] {
                auto *assetDialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!assetDialog) return;
                auto *list = assetDialog->findChild<QListWidget *>(QStringLiteral("uploadedAssetsList"));
                auto *use = assetDialog->findChild<QPushButton *>(QStringLiteral("useSelectedAssetButton"));
                if (!list || !use) { assetDialog->reject(); return; }
                for (int index = 0; index < list->count(); ++index) {
                    if (list->item(index)->data(Qt::UserRole).toString() == asset.reference) {
                        list->setCurrentItem(list->item(index));
                        break;
                    }
                }
                use->click();
            });
            choose->click();
            coverSelected = cover->text() == asset.reference;
            notes->setPlainText(QStringLiteral("Updated through the editor"));
            for (QPushButton *candidate : gameDialog->findChildren<QPushButton *>()) {
                if (candidate->text() == QStringLiteral("Save")) {
                    candidate->click();
                    saved = true;
                    break;
                }
            }
        });
        edit->click();
        QVERIFY(coverSelected);
        QVERIFY(saved);
        const QVector<Game> games = repository.games(&error);
        QCOMPARE(games.size(), 1);
        QCOMPARE(games.first().coverImage, asset.reference);
        QCOMPARE(games.first().personalNotes, QStringLiteral("Updated through the editor"));
    }

    void fieldEditorPersistsDescriptionAndZeroDefault()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("field-default-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QVERIFY(repository.createGame(QStringLiteral("Default field"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Item"), {}, {}, &typeId, &error));
        MainWindow window(repository);
        for (QPushButton *tab : window.findChildren<QPushButton *>(QStringLiteral("tabButton")))
            if (tab->text().contains(QStringLiteral("Types"))) tab->click();
        QPushButton *addField = nullptr;
        for (QPushButton *candidate : window.findChildren<QPushButton *>())
            if (candidate->text().contains(QStringLiteral("Add Field"))) { addField = candidate; break; }
        QVERIFY(addField);
        bool saved = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fieldNameInput"));
            auto *kind = dialog->findChild<QComboBox *>(QStringLiteral("fieldTypeChoice"));
            auto *description = dialog->findChild<QLineEdit *>(QStringLiteral("fieldDescriptionInput"));
            auto *enabled = dialog->findChild<QCheckBox *>(QStringLiteral("fieldDefaultEnabled"));
            auto *defaultInput = dialog->findChild<QLineEdit *>(QStringLiteral("fieldDefaultScalar"));
            if (!name || !kind || !enabled || !defaultInput) { dialog->reject(); return; }
            name->setText(QStringLiteral("Points"));
            kind->setCurrentIndex(kind->findData(QStringLiteral("integer")));
            if (description) description->setText(QStringLiteral("Score begins at zero"));
            enabled->setChecked(true);
            defaultInput->setText(QStringLiteral("0"));
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Add Field")) { candidate->click(); saved = true; break; }
        });
        addField->click();
        QVERIFY(saved);
        const QVector<ObjectType> types = repository.objectTypes(gameId, &error);
        QCOMPARE(types.size(), 1);
        QCOMPARE(types.first().fields.size(), 1);
        const FieldDefinition field = types.first().fields.first();
        QCOMPARE(field.description, QStringLiteral("Score begins at zero"));
        QCOMPARE(field.defaultValue.toDouble(), 0.0);
        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Defaulted item");
        QVERIFY2(repository.saveObject(object, types.first().fields, {}, &error), qPrintable(error));
        const auto objects = repository.objects(gameId, &error);
        QCOMPARE(objects.size(), 1);
        QVERIFY(objects.first().values.contains(field.id));
        QCOMPARE(objects.first().values.value(field.id).toDouble(), 0.0);
    }

    void objectTypeAndFieldEditorsAreReachableAndPersistChanges()
    {
        QTemporaryDir directory;
        GameRepository repository(directory.filePath(QStringLiteral("schema-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QString fieldId;
        QVERIFY(repository.createGame(QStringLiteral("Schema UI"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Entry"), {}, {}, &typeId, &error));
        QVERIFY(repository.createField(typeId, QStringLiteral("Score"), QStringLiteral("integer"), {}, {}, &fieldId, &error));

        MainWindow window(repository);
        QPushButton *typesTab = nullptr;
        for (QPushButton *candidate : window.findChildren<QPushButton *>(QStringLiteral("tabButton")))
            if (candidate->text().contains(QStringLiteral("Types"))) { typesTab = candidate; break; }
        QVERIFY(typesTab);
        typesTab->click();
        auto cards = window.findChildren<QFrame *>(QStringLiteral("typeCard"));
        QCOMPARE(cards.size(), 1);
        auto *primary = cards.first()->findChild<QComboBox *>(QStringLiteral("primaryDisplayFieldChoice"));
        QVERIFY(primary);
        QCOMPARE(primary->findData(fieldId) >= 0, true);
        primary->setCurrentIndex(primary->findData(fieldId));
        QPushButton *savePrimary = nullptr;
        for (QPushButton *candidate : cards.first()->findChildren<QPushButton *>())
            if (candidate->text() == QStringLiteral("Save")) { savePrimary = candidate; break; }
        QVERIFY(savePrimary);
        savePrimary->click();
        QCOMPARE(repository.objectTypes(gameId).first().primaryDisplayFieldId, fieldId);
        cards = window.findChildren<QFrame *>(QStringLiteral("typeCard"));

        QPushButton *editType = nullptr;
        for (QPushButton *candidate : cards.first()->findChildren<QPushButton *>())
            if (candidate->text() == QStringLiteral("Edit Type")) { editType = candidate; break; }
        QVERIFY(editType);
        bool typeSaved = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("typeNameInput"));
            if (!name) { dialog->reject(); return; }
            name->setText(QStringLiteral("Edited Entry"));
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Save Type")) { candidate->click(); typeSaved = true; break; }
        });
        editType->click();
        QVERIFY(typeSaved);
        QCOMPARE(repository.objectTypes(gameId).first().name, QStringLiteral("Edited Entry"));

        cards = window.findChildren<QFrame *>(QStringLiteral("typeCard"));
        QPushButton *editField = nullptr;
        for (QPushButton *candidate : cards.first()->findChildren<QPushButton *>())
            if (candidate->text() == QStringLiteral("Edit")) { editField = candidate; break; }
        QVERIFY(editField);
        bool fieldSaved = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fieldEditNameInput"));
            if (!name) { dialog->reject(); return; }
            name->setText(QStringLiteral("Power"));
            for (QPushButton *candidate : dialog->findChildren<QPushButton *>())
                if (candidate->text() == QStringLiteral("Save Field")) { candidate->click(); fieldSaved = true; break; }
        });
        editField->click();
        QVERIFY(fieldSaved);
        QCOMPARE(repository.objectTypes(gameId).first().fields.first().id, fieldId);
        QCOMPARE(repository.objectTypes(gameId).first().fields.first().name, QStringLiteral("Power"));
    }

    void fieldTypeMigrationDialogPreviewsAndPersistsConversion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        GameRepository repository(directory.filePath(QStringLiteral("migration-ui.sqlite")));
        QString error;
        QVERIFY2(repository.open(&error), qPrintable(error));
        QString gameId;
        QString typeId;
        QString fieldId;
        QVERIFY(repository.createGame(QStringLiteral("Migration UI"), {}, {}, &gameId, &error));
        QVERIFY(repository.createObjectType(gameId, QStringLiteral("Entry"), {}, {}, &typeId, &error));
        QVERIFY(repository.createField(typeId, QStringLiteral("Score text"), QStringLiteral("text"), {}, {}, &fieldId, &error));
        ObjectRecord object;
        object.gameId = gameId;
        object.typeId = typeId;
        object.name = QStringLiteral("Score record");
        object.values.insert(fieldId, QStringLiteral("37"));
        QVERIFY2(repository.saveObject(object, repository.objectTypes(gameId, &error).first().fields, {}, &error), qPrintable(error));

        MainWindow window(repository);
        for (QPushButton *candidate : window.findChildren<QPushButton *>(QStringLiteral("tabButton")))
            if (candidate->text().contains(QStringLiteral("Types"))) candidate->click();
        QPushButton *migrate = window.findChild<QPushButton *>(QStringLiteral("migrateFieldTypeButton"));
        QVERIFY(migrate);
        bool previewed = false;
        bool applied = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("fieldTypeMigrationDialog")) return;
            auto *target = dialog->findChild<QComboBox *>(QStringLiteral("migrationTargetType"));
            auto *previewButton = dialog->findChild<QPushButton *>(QStringLiteral("previewFieldMigrationButton"));
            auto *applyButton = dialog->findChild<QPushButton *>(QStringLiteral("applyFieldMigrationButton"));
            if (!target || !previewButton || !applyButton) { dialog->reject(); return; }
            target->setCurrentIndex(target->findData(QStringLiteral("integer")));
            previewButton->click();
            previewed = applyButton->isEnabled();
            if (previewed) {
                QTimer::singleShot(0, [] {
                    if (QWidget *modal = QApplication::activeModalWidget()) modal->close();
                });
                applyButton->click();
                applied = true;
            } else dialog->reject();
        });
        migrate->click();
        QVERIFY(previewed);
        QVERIFY(applied);
        const auto updatedTypes = repository.objectTypes(gameId, &error);
        const auto updatedField = std::find_if(updatedTypes.first().fields.cbegin(), updatedTypes.first().fields.cend(),
            [&](const FieldDefinition &item) { return item.id == fieldId; });
        QVERIFY(updatedField != updatedTypes.first().fields.cend());
        QCOMPARE(updatedField->fieldType, QStringLiteral("integer"));
        const auto migratedObjects = repository.objects(gameId, &error);
        QCOMPARE(migratedObjects.first().values.value(fieldId).toDouble(), 37.0);
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
