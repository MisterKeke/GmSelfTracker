#pragma once

#include <QSqlDatabase>
#include <QString>

namespace gmst {

struct AccountRecord {
    QString id;
    QString username;
};

class AccountService {
public:
    explicit AccountService(QSqlDatabase database);

    bool registerAccount(const QString &username, const QString &password, AccountRecord *created = nullptr,
                         QString *error = nullptr) const;
    bool authenticate(const QString &username, const QString &password, AccountRecord *account = nullptr,
                      QString *error = nullptr) const;
    bool secureLegacyProfile(const QString &username, const QString &password, AccountRecord *account = nullptr,
                             QString *error = nullptr) const;
    bool changePassword(const QString &userId, const QString &currentPassword, const QString &newPassword,
                        QString *error = nullptr);
    bool legacyProfileAvailable() const;

private:
    QSqlDatabase m_database;
};

} // namespace gmst
