#pragma once

#include <QDialog>

class QLineEdit;
class QLabel;

namespace gmst {
class GameRepository;

class WebImportDialog final : public QDialog {
    Q_OBJECT
public:
    explicit WebImportDialog(GameRepository &repository, QWidget *parent = nullptr);

private:
    void chooseDatabase();
    void chooseUploads();
    void runImport();

    GameRepository &m_repository;
    QLineEdit *m_databasePath = nullptr;
    QLineEdit *m_uploadsPath = nullptr;
    QLabel *m_notice = nullptr;
};

} // namespace gmst
