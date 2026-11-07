#include "app/web_import_dialog.h"

#include "infrastructure/database/game_repository.h"

#include <QDialogButtonBox>
#include <QAbstractButton>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace gmst {

WebImportDialog::WebImportDialog(GameRepository &repository, QWidget *parent)
    : QDialog(parent), m_repository(repository)
{
    setObjectName(QStringLiteral("webImportDialog"));
    setWindowTitle(QStringLiteral("Import Existing GmSelfTracker Data"));
    setMinimumWidth(620);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *intro = new QLabel(QStringLiteral("Import every account and game from a Web database. The source database and uploads are read only. Existing desktop accounts and records are never replaced; any ID or username collision stops the whole import."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    m_databasePath = new QLineEdit(this);
    m_databasePath->setObjectName(QStringLiteral("webImportDatabasePath"));
    m_databasePath->setReadOnly(true);
    auto *databaseRow = new QWidget(this);
    auto *databaseLayout = new QHBoxLayout(databaseRow);
    databaseLayout->setContentsMargins(0, 0, 0, 0);
    databaseLayout->addWidget(m_databasePath, 1);
    auto *databaseButton = new QPushButton(QStringLiteral("Choose SQLite…"), databaseRow);
    databaseButton->setObjectName(QStringLiteral("chooseWebDatabaseButton"));
    databaseLayout->addWidget(databaseButton);
    form->addRow(QStringLiteral("Web database"), databaseRow);

    m_uploadsPath = new QLineEdit(this);
    m_uploadsPath->setObjectName(QStringLiteral("webImportUploadsPath"));
    m_uploadsPath->setReadOnly(true);
    auto *uploadsRow = new QWidget(this);
    auto *uploadsLayout = new QHBoxLayout(uploadsRow);
    uploadsLayout->setContentsMargins(0, 0, 0, 0);
    uploadsLayout->addWidget(m_uploadsPath, 1);
    auto *uploadsButton = new QPushButton(QStringLiteral("Choose Folder…"), uploadsRow);
    uploadsButton->setObjectName(QStringLiteral("chooseWebUploadsButton"));
    uploadsLayout->addWidget(uploadsButton);
    form->addRow(QStringLiteral("Web uploads"), uploadsRow);
    layout->addLayout(form);

    auto *note = new QLabel(QStringLiteral("The import requires Web schema version 11. It preserves account IDs, bcrypt hashes, archived data, history, templates, presets, dashboards, gallery records, and shared assets. Browser sessions are not imported. Active WAL data is read through a consistent SQLite snapshot."), this);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("webImportRequirements"));
    layout->addWidget(note);
    m_notice = new QLabel(QString(), this);
    m_notice->setObjectName(QStringLiteral("webImportNotice"));
    m_notice->setWordWrap(true);
    layout->addWidget(m_notice);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *import = buttons->addButton(QStringLiteral("Import Data"), QDialogButtonBox::AcceptRole);
    import->setObjectName(QStringLiteral("runWebImportButton"));
    import->setEnabled(false);
    layout->addWidget(buttons);
    const auto updateEnabled = [this, import] {
        import->setEnabled(!m_databasePath->text().isEmpty() && !m_uploadsPath->text().isEmpty());
    };
    connect(databaseButton, &QPushButton::clicked, this, &WebImportDialog::chooseDatabase);
    connect(uploadsButton, &QPushButton::clicked, this, &WebImportDialog::chooseUploads);
    connect(m_databasePath, &QLineEdit::textChanged, this, updateEnabled);
    connect(m_uploadsPath, &QLineEdit::textChanged, this, updateEnabled);
    connect(import, &QPushButton::clicked, this, &WebImportDialog::runImport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void WebImportDialog::chooseDatabase()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select Web SQLite database"), {},
        QStringLiteral("SQLite databases (*.db *.sqlite *.sqlite3);;All files (*.*)"));
    if (!path.isEmpty()) m_databasePath->setText(path);
}

void WebImportDialog::chooseUploads()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Select Web uploads directory"));
    if (!path.isEmpty()) m_uploadsPath->setText(path);
}

void WebImportDialog::runImport()
{
    if (QMessageBox::question(this, QStringLiteral("Import Web data?"),
        QStringLiteral("Add all accounts and their records to this desktop database? Existing records will not be overwritten. You can retry after resolving any reported collision."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    WebImportSummary summary;
    QString error;
    if (!m_repository.importWebData(m_databasePath->text(), m_uploadsPath->text(), &summary, &error)) {
        m_notice->setText(error);
        return;
    }
    m_notice->setStyleSheet(QStringLiteral("color:#276c3c;font-weight:700;"));
    m_notice->setText(QStringLiteral("Imported %1 account(s), %2 game(s), %3 object(s), and %4 asset record(s). Use Lock / Switch Account to sign in to an imported account.")
        .arg(summary.accounts).arg(summary.games).arg(summary.objects).arg(summary.assets));
    QDialogButtonBox *buttons = findChild<QDialogButtonBox *>();
    if (buttons) {
        for (QAbstractButton *button : buttons->buttons()) {
            if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) button->setText(QStringLiteral("Done"));
        }
    }
    disconnect(findChild<QPushButton *>(QStringLiteral("runWebImportButton")), nullptr, this, nullptr);
    connect(findChild<QPushButton *>(QStringLiteral("runWebImportButton")), &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace gmst
