#include "app/main_window.h"
#include "app/account_dialog.h"
#include "app/theme.h"
#include "infrastructure/database/game_repository.h"
#include "infrastructure/assets/image_decoder.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextStream>

#include <algorithm>
#include <functional>

namespace {

struct RuntimeResult {
    int exitCode = 0;
    QString report;
};

void recordCheck(QStringList &messages, const QString &name, bool passed, const QString &detail = {})
{
    messages.append(QStringLiteral("%1: %2%3")
        .arg(passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"), name,
             detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail));
}

RuntimeResult checkRuntime()
{
    QStringList messages;
    const QStringList drivers = QSqlDatabase::drivers();
    const bool sqliteDriverPresent = drivers.contains(QStringLiteral("QSQLITE"));
    recordCheck(messages, QStringLiteral("QSQLITE driver"), sqliteDriverPresent,
                sqliteDriverPresent ? QString() : drivers.join(QStringLiteral(", ")));
    if (!sqliteDriverPresent) return {3, messages.join(QLatin1Char('\n'))};

    QTemporaryDir temporary;
    if (!temporary.isValid()) return {4, QStringLiteral("FAIL: temporary runtime directory could not be created")};
    {
        gmst::GameRepository repository(QDir(temporary.path()).filePath(QStringLiteral("smoke.sqlite")));
        QString error;
        const bool databaseReady = repository.open(&error);
        recordCheck(messages, QStringLiteral("SQLite migrations 001–011"), databaseReady, error);
        if (!databaseReady) return {5, messages.join(QLatin1Char('\n'))};

        const QVector<gmst::Game> before = repository.games(&error);
        QString gameId;
        const bool wrote = repository.createGame(QStringLiteral("Runtime Check"), QStringLiteral("Disposable smoke check"),
            QStringLiteral("🎮"), &gameId, &error);
        const bool writeRead = wrote && repository.games(&error).size() == before.size() + 1;
        recordCheck(messages, QStringLiteral("SQLite write/read and foreign keys"), writeRead, error);
        if (!writeRead) return {6, messages.join(QLatin1Char('\n'))};
        const bool sectionSaved = repository.createSection(gameId, {}, QStringLiteral("Disposable Section"), QStringLiteral("✦"), &error);
        recordCheck(messages, QStringLiteral("Native game and section repository"), sectionSaved, error);
        if (!sectionSaved) return {7, messages.join(QLatin1Char('\n'))};

        const QList<QByteArray> requiredFormats = {"png", "jpeg", "gif", "bmp"};
        const QList<QByteArray> availableFormats = QImageReader::supportedImageFormats();
        QStringList missingFormats;
        for (const QByteArray &format : requiredFormats) {
            if (std::none_of(availableFormats.cbegin(), availableFormats.cend(), [&](const QByteArray &available) {
                    return available.compare(format, Qt::CaseInsensitive) == 0;
                })) missingFormats.append(QString::fromLatin1(format));
        }
        QString webpError;
        const bool webpReady = gmst::ImageDecoder::verifyWebpDecoder(&webpError);
        if (!webpReady) missingFormats.append(QStringLiteral("webp"));
        const bool imagesReady = missingFormats.isEmpty();
        recordCheck(messages, QStringLiteral("Required image decoders"), imagesReady,
                    imagesReady ? QString() : QStringLiteral("missing ") + missingFormats.join(QStringLiteral(", ")) +
                        (webpError.isEmpty() ? QString() : QStringLiteral("; ") + webpError));

        gmst::MainWindow representativeUi(repository);
        representativeUi.ensurePolished();
        representativeUi.show();
        QApplication::processEvents();
        const bool widgetsReady = representativeUi.isVisible();
        recordCheck(messages, QStringLiteral("Representative Qt Widgets interface"), widgetsReady);
        if (!widgetsReady) return {9, messages.join(QLatin1Char('\n'))};
        if (!imagesReady) return {8, messages.join(QLatin1Char('\n'))};
    }

    messages.prepend(QStringLiteral("PASS: packaged runtime"));
    return {0, messages.join(QLatin1Char('\n'))};
}

QString runtimeReportPath(const QStringList &arguments)
{
    for (const QString &argument : arguments) {
        const QString prefix = QStringLiteral("--check-runtime-report=");
        if (argument.startsWith(prefix)) return argument.mid(prefix.size());
    }
    return {};
}

void writeReport(const QString &path, const QString &report)
{
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    file.write(report.toUtf8());
    file.write("\n");
}

} // namespace

int main(int argc, char *argv[])
{
    bool runtimeMode = false;
    bool previewMode = false;
    bool interactivePreview = false;
    bool goalsPreview = false;
    QString previewPath;
    QSize previewSize(1600, 950);
    for (int index = 1; index < argc; ++index) {
        const QByteArray argument(argv[index]);
        if (argument == QByteArrayLiteral("--check-runtime")) runtimeMode = true;
        if (argument.startsWith("--render-preview=")) {
            previewMode = true;
            previewPath = QString::fromLocal8Bit(argument.mid(17));
        }
        if (argument.startsWith("--preview-size=")) {
            const QString size = QString::fromLatin1(argument.mid(15));
            const QStringList dimensions = size.split(QLatin1Char('x'));
            if (dimensions.size() == 2) previewSize = QSize(dimensions.at(0).toInt(), dimensions.at(1).toInt());
        }
        if (argument == QByteArrayLiteral("--preview-page=goals")) goalsPreview = true;
        if (argument == QByteArrayLiteral("--preview-interactive=goals")) {
            previewMode = true;
            goalsPreview = true;
            interactivePreview = true;
        }
    }
    if ((runtimeMode || previewMode) && !interactivePreview && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    QCoreApplication::setOrganizationName(QStringLiteral(GMST_ORGANIZATION));
    QCoreApplication::setApplicationName(QStringLiteral(GMST_APPLICATION_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(GMST_VERSION));
    gmst::theme::apply(application);

    if (runtimeMode) {
        const RuntimeResult result = checkRuntime();
        QTextStream stream(result.exitCode == 0 ? stdout : stderr);
        stream << result.report << Qt::endl;
        writeReport(runtimeReportPath(arguments), result.report);
        return result.exitCode;
    }

    if (previewMode) {
        QTemporaryDir temporary;
        gmst::GameRepository repository(QDir(temporary.path()).filePath(QStringLiteral("preview.sqlite")));
        QString error;
        if (!temporary.isValid() || !repository.open(&error)) {
            QTextStream(stderr) << QStringLiteral("Preview setup failed: %1").arg(error) << Qt::endl;
            return 10;
        }
        QString gameId;
        if (!repository.createGame(QStringLiteral("Genshin"), QStringLiteral("Testing"), QStringLiteral("✧"), &gameId, &error) ||
            !repository.createSection(gameId, {}, QStringLiteral("test"), QStringLiteral("✦"), &error)) {
            QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
            return 11;
        }
        QString characterTypeId;
        if (!repository.createObjectType(gameId, QStringLiteral("Character"), {}, QStringLiteral("◈"), &characterTypeId, &error)) {
            QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
            return 11;
        }
        const QVector<QPair<QString, QString>> fieldSeeds = {
            {QStringLiteral("Level"), QStringLiteral("integer")}, {QStringLiteral("Hp"), QStringLiteral("integer")},
            {QStringLiteral("Attack"), QStringLiteral("integer")}, {QStringLiteral("Defense"), QStringLiteral("integer")}
        };
        for (const auto &fieldSeed : fieldSeeds) {
            if (!repository.createField(characterTypeId, fieldSeed.first, fieldSeed.second, QStringLiteral("✦"), {}, nullptr, &error)) {
                QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
                return 11;
            }
        }
        const QVector<gmst::ObjectType> previewTypes = repository.objectTypes(gameId, &error);
        if (previewTypes.isEmpty()) {
            QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
            return 11;
        }
        const auto makeCharacter = [&](const QString &name, const QVector<int> &values) {
            gmst::ObjectRecord object;
            object.gameId = gameId;
            object.typeId = characterTypeId;
            object.name = name;
            for (int index = 0; index < previewTypes.first().fields.size() && index < values.size(); ++index) {
                object.values.insert(previewTypes.first().fields.at(index).id, values.at(index));
            }
            return repository.saveObject(object, previewTypes.first().fields, {}, &error);
        };
        if (!makeCharacter(QStringLiteral("Furina"), {90, 30000, 1000, 1000}) ||
            !makeCharacter(QStringLiteral("Flins"), {90, 10000, 1500, 1000})) {
            QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
            return 11;
        }
        if (goalsPreview) {
            gmst::Goal prerequisite;
            prerequisite.gameId = gameId;
            prerequisite.title = QStringLiteral("Complete the story chapter");
            prerequisite.description = QStringLiteral("Finish the latest Archon Quest before moving on.");
            prerequisite.goalType = QStringLiteral("manual");
            if (!repository.saveGoal(prerequisite, {}, &error)) {
                QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
                return 11;
            }
            gmst::Goal checklist;
            checklist.gameId = gameId;
            checklist.title = QStringLiteral("Daily routine");
            checklist.goalType = QStringLiteral("checklist");
            checklist.prerequisiteIds = {prerequisite.id};
            if (!repository.saveGoal(checklist, {
                    gmst::GoalTask{{}, QStringLiteral("Claim daily commissions"), true, 0},
                    gmst::GoalTask{{}, QStringLiteral("Spend resin"), false, 1}
                }, &error)) {
                QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
                return 11;
            }
            gmst::Goal numeric;
            numeric.gameId = gameId;
            numeric.title = QStringLiteral("Collect boss materials");
            numeric.goalType = QStringLiteral("numeric");
            numeric.current = 18;
            numeric.target = 30;
            if (!repository.saveGoal(numeric, {}, &error)) {
                QTextStream(stderr) << QStringLiteral("Preview sample failed: %1").arg(error) << Qt::endl;
                return 11;
            }
        }
        gmst::MainWindow window(repository);
        window.resize(previewSize);
        window.show();
        if (goalsPreview) {
            for (QPushButton *tab : window.findChildren<QPushButton *>(QStringLiteral("tabButton"))) {
                if (tab->text().contains(QStringLiteral("Goals"))) {
                    tab->click();
                    break;
                }
            }
        }
        if (interactivePreview) return application.exec();
        QApplication::processEvents();
        if (!window.grab().save(previewPath)) {
            QTextStream(stderr) << QStringLiteral("Could not write preview image: %1").arg(previewPath) << Qt::endl;
            return 12;
        }
        QTextStream(stdout) << QStringLiteral("Native preview written: %1").arg(previewPath) << Qt::endl;
        return 0;
    }

    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString databasePath = QDir(dataDirectory).filePath(QStringLiteral("gametracker.sqlite"));
    gmst::GameRepository repository(databasePath);
    QString error;
    if (!repository.open(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Could not open local data"), error);
        return 2;
    }
    gmst::AccountDialog signIn(repository);
    if (signIn.exec() != QDialog::Accepted) return 0;
    std::function<void()> showTracker;
    showTracker = [&] {
        auto *window = new gmst::MainWindow(repository);
        QObject::connect(window, &gmst::MainWindow::accountLockRequested, &application, [&, window] {
            window->hide();
            QTimer::singleShot(0, &application, [&, window] {
                gmst::AccountDialog accountDialog(repository);
                if (accountDialog.exec() == QDialog::Accepted) {
                    window->deleteLater();
                    showTracker();
                } else {
                    window->deleteLater();
                    application.quit();
                }
            });
        });
        window->show();
    };
    showTracker();
    return application.exec();
}
