#include "infrastructure/database/account_service.h"

#include "infrastructure/security/password_hasher.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace gmst {
namespace {

const QString legacyUserId = QStringLiteral("native-desktop-local");
const QString legacyPasswordHash = QStringLiteral("!native-desktop-profile");
const QByteArray dummyBcryptHash = QByteArrayLiteral("$2a$10$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy");

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString normalizeUsername(const QString &username)
{
    return username.trimmed();
}

bool validUsername(const QString &username)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{3,32}$"));
    return pattern.match(username).hasMatch();
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

AccountService::AccountService(QSqlDatabase database) : m_database(std::move(database))
{
}

bool AccountService::registerAccount(const QString &usernameValue, const QString &password, AccountRecord *created,
                                     QString *error) const
{
    if (error) error->clear();
    const QString username = normalizeUsername(usernameValue);
    if (!validUsername(username)) {
        if (error) *error = QStringLiteral("Use 3–32 letters, numbers, underscores, or hyphens for the username.");
        return false;
    }
    QByteArray hash;
    if (!PasswordHasher::hash(password, &hash, error)) return false;
    const AccountRecord account{newId(), username};
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral("INSERT INTO users(id,username,password_hash,created_at) VALUES(?,?,?,?)"));
    insert.addBindValue(account.id);
    insert.addBindValue(account.username);
    insert.addBindValue(QString::fromLatin1(hash));
    insert.addBindValue(nowUtc());
    if (!insert.exec()) {
        if (error) *error = insert.lastError().text().contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
            ? QStringLiteral("That username is already in use.") : insert.lastError().text();
        return false;
    }
    if (created) *created = account;
    return true;
}

bool AccountService::authenticate(const QString &usernameValue, const QString &password, AccountRecord *account,
                                  QString *error) const
{
    if (error) error->clear();
    const QString username = normalizeUsername(usernameValue);
    QString id;
    QString storedUsername;
    QByteArray passwordHash;
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT id,username,password_hash FROM users WHERE username=? COLLATE NOCASE"));
    lookup.addBindValue(username);
    const bool found = lookup.exec() && lookup.next();
    if (found) {
        id = lookup.value(0).toString();
        storedUsername = lookup.value(1).toString();
        passwordHash = lookup.value(2).toString().toLatin1();
    }
    const QByteArray candidate = found ? passwordHash : dummyBcryptHash;
    const bool passwordMatches = PasswordHasher::validPasswordLength(password) && PasswordHasher::verify(password, candidate);
    if (!validUsername(username) || !found || !passwordMatches) {
        if (error) *error = QStringLiteral("Username or password is incorrect.");
        return false;
    }
    if (account) *account = AccountRecord{id, storedUsername};
    return true;
}

bool AccountService::secureLegacyProfile(const QString &usernameValue, const QString &password, AccountRecord *account,
                                        QString *error) const
{
    if (error) error->clear();
    const QString username = normalizeUsername(usernameValue);
    if (!validUsername(username)) {
        if (error) *error = QStringLiteral("Use 3–32 letters, numbers, underscores, or hyphens for the username.");
        return false;
    }
    QByteArray hash;
    if (!PasswordHasher::hash(password, &hash, error)) return false;
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE users SET username=?,password_hash=? WHERE id=? AND password_hash=?"));
    update.addBindValue(username);
    update.addBindValue(QString::fromLatin1(hash));
    update.addBindValue(legacyUserId);
    update.addBindValue(legacyPasswordHash);
    if (!update.exec() || update.numRowsAffected() != 1) {
        if (error) *error = update.lastError().isValid() ? update.lastError().text() : QStringLiteral("The legacy local profile has already been secured.");
        return false;
    }
    if (account) *account = AccountRecord{legacyUserId, username};
    return true;
}

bool AccountService::changePassword(const QString &userId, const QString &currentPassword, const QString &newPassword,
                                    QString *error)
{
    if (error) error->clear();
    QSqlQuery lookup(m_database);
    lookup.prepare(QStringLiteral("SELECT password_hash FROM users WHERE id=?"));
    lookup.addBindValue(userId);
    if (!lookup.exec() || !lookup.next()) {
        if (error) *error = QStringLiteral("The selected account is unavailable.");
        return false;
    }
    const QString currentHash = lookup.value(0).toString();
    if (!PasswordHasher::verify(currentPassword, currentHash.toLatin1())) {
        if (error) *error = QStringLiteral("Current password is incorrect.");
        return false;
    }
    QByteArray newHash;
    if (!PasswordHasher::hash(newPassword, &newHash, error)) return false;
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE users SET password_hash=? WHERE id=? AND password_hash=?"));
    update.addBindValue(QString::fromLatin1(newHash));
    update.addBindValue(userId);
    update.addBindValue(currentHash);
    if (!update.exec() || update.numRowsAffected() != 1) {
        m_database.rollback();
        if (error) *error = QStringLiteral("The password changed in another session. Sign in again.");
        return false;
    }
    QSqlQuery sessions(m_database);
    sessions.prepare(QStringLiteral("DELETE FROM sessions WHERE user_id=?"));
    sessions.addBindValue(userId);
    if (!sessions.exec() || !m_database.commit()) {
        m_database.rollback();
        if (error) *error = sessions.lastError().isValid() ? sessions.lastError().text() : m_database.lastError().text();
        return false;
    }
    return true;
}

bool AccountService::legacyProfileAvailable() const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM users WHERE id=? AND password_hash=?"));
    query.addBindValue(legacyUserId);
    query.addBindValue(legacyPasswordHash);
    return query.exec() && query.next();
}

} // namespace gmst
