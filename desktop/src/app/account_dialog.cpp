#include "app/account_dialog.h"

#include "infrastructure/database/game_repository.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace gmst {
namespace {

QLabel *accountLabel(const QString &text, QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    return label;
}

QPushButton *accountButton(const QString &text, const QString &name, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    return button;
}

} // namespace

AccountDialog::AccountDialog(GameRepository &repository, QWidget *parent)
    : QDialog(parent), m_repository(repository)
{
    setObjectName(QStringLiteral("accountDialog"));
    setWindowTitle(QStringLiteral("GmSelfTracker account"));
    setMinimumWidth(460);
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(12);
    auto *title = accountLabel(QStringLiteral("Welcome back, adventurer"), this);
    title->setObjectName(QStringLiteral("accountTitle"));
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:800;color:#49334e;"));
    layout->addWidget(title);
    layout->addWidget(accountLabel(QStringLiteral("Sign in to open your local game library, or create a separate account on this computer."), this));

    auto *form = new QFormLayout;
    form->setSpacing(8);
    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("accountUsername"));
    m_username->setMaxLength(32);
    m_username->setPlaceholderText(QStringLiteral("3–32 letters, numbers, _ or -"));
    m_username->setText(QSettings().value(QStringLiteral("account/lastUsername")).toString());
    form->addRow(QStringLiteral("Username"), m_username);
    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("accountPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setMaxLength(72);
    m_password->setPlaceholderText(QStringLiteral("At least 10 UTF-8 bytes"));
    form->addRow(QStringLiteral("Password"), m_password);
    m_confirmation = new QLineEdit(this);
    m_confirmation->setObjectName(QStringLiteral("accountConfirmPassword"));
    m_confirmation->setEchoMode(QLineEdit::Password);
    m_confirmation->setMaxLength(72);
    m_confirmation->setPlaceholderText(QStringLiteral("Repeat password for account creation"));
    form->addRow(QStringLiteral("Confirm password"), m_confirmation);
    layout->addLayout(form);

    m_notice = accountLabel(QString(), this);
    m_notice->setObjectName(QStringLiteral("accountNotice"));
    m_notice->setStyleSheet(QStringLiteral("color:#a23b3b;font-weight:700;"));
    layout->addWidget(m_notice);

    auto *login = accountButton(QStringLiteral("Log In"), QStringLiteral("accountLoginButton"), this);
    login->setDefault(true);
    auto *registerButton = accountButton(QStringLiteral("Create Account"), QStringLiteral("accountRegisterButton"), this);
    auto *secure = accountButton(QStringLiteral("Secure Existing Local Profile"), QStringLiteral("accountSecureLocalButton"), this);
    secure->setVisible(repository.legacyProfileAvailable());
    secure->setToolTip(QStringLiteral("Set a password while preserving the existing local profile ID and its game data."));
    auto *cancel = accountButton(QStringLiteral("Exit"), QStringLiteral("accountCancelButton"), this);
    auto *actions = new QVBoxLayout;
    actions->addWidget(login);
    actions->addWidget(registerButton);
    if (secure->isVisible()) actions->addWidget(secure);
    actions->addWidget(cancel);
    layout->addLayout(actions);
    connect(login, &QPushButton::clicked, this, &AccountDialog::finishLogin);
    connect(registerButton, &QPushButton::clicked, this, &AccountDialog::finishRegistration);
    connect(secure, &QPushButton::clicked, this, &AccountDialog::finishLegacyProfile);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_password, &QLineEdit::returnPressed, this, &AccountDialog::finishLogin);
}

bool AccountDialog::validateConfirmation()
{
    if (m_password->text() != m_confirmation->text()) {
        showError(QStringLiteral("The password confirmation does not match."));
        return false;
    }
    return true;
}

void AccountDialog::showError(const QString &message)
{
    m_notice->setText(message);
}

void AccountDialog::finishLogin()
{
    QString error;
    if (!m_repository.authenticate(m_username->text(), m_password->text(), nullptr, &error)) {
        showError(error);
        return;
    }
    QSettings().setValue(QStringLiteral("account/lastUsername"), m_repository.activeUsername());
    accept();
}

void AccountDialog::finishRegistration()
{
    if (!validateConfirmation()) return;
    QString error;
    if (!m_repository.registerAccount(m_username->text(), m_password->text(), nullptr, &error)) {
        showError(error);
        return;
    }
    QSettings().setValue(QStringLiteral("account/lastUsername"), m_repository.activeUsername());
    accept();
}

void AccountDialog::finishLegacyProfile()
{
    if (!validateConfirmation()) return;
    QString error;
    if (!m_repository.secureLegacyProfile(m_username->text(), m_password->text(), nullptr, &error)) {
        showError(error);
        return;
    }
    QSettings().setValue(QStringLiteral("account/lastUsername"), m_repository.activeUsername());
    accept();
}

} // namespace gmst
