#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;

namespace gmst {
class GameRepository;

class AccountDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AccountDialog(GameRepository &repository, QWidget *parent = nullptr);

private:
    bool validateConfirmation();
    void showError(const QString &message);
    void finishLogin();
    void finishRegistration();
    void finishLegacyProfile();

    GameRepository &m_repository;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_confirmation = nullptr;
    QLabel *m_notice = nullptr;
};

} // namespace gmst
