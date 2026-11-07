#pragma once

#include <QSqlDatabase>
#include <QString>

namespace gmst {

struct WebImportSummary {
    int accounts = 0;
    int games = 0;
    int objects = 0;
    int assets = 0;
};

class WebDataImportService {
public:
    WebDataImportService(QSqlDatabase targetDatabase, QString targetDatabasePath, QString targetUploadsPath);

    bool import(const QString &sourceDatabasePath, const QString &sourceUploadsPath,
                WebImportSummary *summary = nullptr, QString *error = nullptr);

private:
    QSqlDatabase m_targetDatabase;
    QString m_targetDatabasePath;
    QString m_targetUploadsPath;
};

} // namespace gmst
