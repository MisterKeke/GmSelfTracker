#pragma once

#include <QSqlDatabase>
#include <QStringList>

namespace gmst {

class MigrationRunner {
public:
    static bool apply(QSqlDatabase &database, QString *error = nullptr);
    static QStringList splitSqlStatements(const QString &script);
};

} // namespace gmst
