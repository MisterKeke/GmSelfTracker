#include "infrastructure/database/migration_runner.h"

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

static void initializeGmSelfTrackerResources()
{
    Q_INIT_RESOURCE(resources);
}

namespace gmst {
namespace {

QString migrationError(const QString &name, const QString &detail)
{
    return QStringLiteral("Migration %1 failed: %2").arg(name, detail);
}

} // namespace

QStringList MigrationRunner::splitSqlStatements(const QString &script)
{
    enum class Quote { None, Single, Double, Backtick, Bracket };
    Quote quote = Quote::None;
    bool lineComment = false;
    bool blockComment = false;
    bool isTrigger = false;
    bool triggerBodyStarted = false;
    int triggerBlockDepth = 0;
    int caseDepth = 0;
    QString token;
    QString statement;
    QStringList statements;

    auto flushToken = [&] {
        if (token.isEmpty()) return;
        const QString word = token.toUpper();
        if (word == QStringLiteral("TRIGGER")) isTrigger = true;
        if (isTrigger && word == QStringLiteral("BEGIN")) {
            triggerBodyStarted = true;
            ++triggerBlockDepth;
        } else if (isTrigger && triggerBodyStarted && word == QStringLiteral("CASE")) {
            ++caseDepth;
        } else if (isTrigger && triggerBodyStarted && word == QStringLiteral("END")) {
            if (caseDepth > 0) --caseDepth;
            else if (triggerBlockDepth > 0) --triggerBlockDepth;
        }
        token.clear();
    };

    for (qsizetype i = 0; i < script.size(); ++i) {
        const QChar ch = script.at(i);
        const QChar next = i + 1 < script.size() ? script.at(i + 1) : QChar();

        if (lineComment) {
            statement += ch;
            if (ch == QLatin1Char('\n')) lineComment = false;
            continue;
        }
        if (blockComment) {
            statement += ch;
            if (ch == QLatin1Char('*') && next == QLatin1Char('/')) {
                statement += next;
                ++i;
                blockComment = false;
            }
            continue;
        }
        if (quote != Quote::None) {
            statement += ch;
            const QChar close = quote == Quote::Single ? QLatin1Char('\'')
                : quote == Quote::Double ? QLatin1Char('"')
                : quote == Quote::Backtick ? QLatin1Char('`') : QLatin1Char(']');
            if (ch == close) {
                if (quote != Quote::Bracket && next == close) {
                    statement += next;
                    ++i;
                } else {
                    quote = Quote::None;
                }
            }
            continue;
        }

        if (ch == QLatin1Char('-') && next == QLatin1Char('-')) {
            flushToken();
            statement += ch;
            statement += next;
            ++i;
            lineComment = true;
            continue;
        }
        if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            flushToken();
            statement += ch;
            statement += next;
            ++i;
            blockComment = true;
            continue;
        }
        if (ch == QLatin1Char('\'')) { flushToken(); quote = Quote::Single; statement += ch; continue; }
        if (ch == QLatin1Char('"')) { flushToken(); quote = Quote::Double; statement += ch; continue; }
        if (ch == QLatin1Char('`')) { flushToken(); quote = Quote::Backtick; statement += ch; continue; }
        if (ch == QLatin1Char('[')) { flushToken(); quote = Quote::Bracket; statement += ch; continue; }

        if (ch.isLetterOrNumber() || ch == QLatin1Char('_')) {
            token += ch;
            statement += ch;
            continue;
        }
        flushToken();
        statement += ch;
        if (ch == QLatin1Char(';') && (!isTrigger || !triggerBodyStarted || triggerBlockDepth == 0)) {
            if (!statement.trimmed().isEmpty()) statements.append(statement.trimmed());
            statement.clear();
            isTrigger = false;
            triggerBodyStarted = false;
            triggerBlockDepth = 0;
            caseDepth = 0;
        }
    }
    flushToken();
    if (!statement.trimmed().isEmpty()) statements.append(statement.trimmed());
    return statements;
}

bool MigrationRunner::apply(QSqlDatabase &database, QString *error)
{
    initializeGmSelfTrackerResources();
    QSqlQuery setup(database);
    if (!setup.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
        if (error) *error = setup.lastError().text();
        return false;
    }
    if (!setup.exec(QStringLiteral("PRAGMA busy_timeout = 5000"))) {
        if (error) *error = setup.lastError().text();
        return false;
    }
    if (!setup.exec(QStringLiteral("PRAGMA journal_mode = WAL"))) {
        if (error) *error = setup.lastError().text();
        return false;
    }
    if (!setup.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"))) {
        if (error) *error = setup.lastError().text();
        return false;
    }

    const QStringList names = {
        QStringLiteral("001_initial.sql"), QStringLiteral("002_local_features.sql"),
        QStringLiteral("003_asset_lifecycle.sql"), QStringLiteral("004_primary_display.sql"),
        QStringLiteral("005_object_history.sql"), QStringLiteral("006_field_type_migrations.sql"),
        QStringLiteral("007_game_templates.sql"), QStringLiteral("008_object_presets.sql"),
        QStringLiteral("009_goal_dependencies.sql"), QStringLiteral("010_game_dashboards.sql"),
        QStringLiteral("011_object_gallery.sql")
    };

    for (qsizetype index = 0; index < names.size(); ++index) {
        QSqlQuery check(database);
        check.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version = ?"));
        check.addBindValue(index + 1);
        if (!check.exec()) {
            if (error) *error = check.lastError().text();
            return false;
        }
        if (check.next()) continue;

        const QString name = names.at(index);
        QFile file(QStringLiteral(":/migrations/") + name);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (error) *error = migrationError(name, file.errorString());
            return false;
        }
        const QStringList statements = splitSqlStatements(QString::fromUtf8(file.readAll()));
        if (!database.transaction()) {
            if (error) *error = migrationError(name, database.lastError().text());
            return false;
        }
        for (const QString &sql : statements) {
            QSqlQuery query(database);
            if (!query.exec(sql)) {
                const QString detail = query.lastError().text() + QStringLiteral("\nSQL: ") + sql;
                database.rollback();
                if (error) *error = migrationError(name, detail);
                return false;
            }
        }
        QSqlQuery record(database);
        record.prepare(QStringLiteral("INSERT INTO schema_migrations(version) VALUES (?)"));
        record.addBindValue(index + 1);
        if (!record.exec() || !database.commit()) {
            const QString detail = record.lastError().isValid() ? record.lastError().text() : database.lastError().text();
            database.rollback();
            if (error) *error = migrationError(name, detail);
            return false;
        }
    }

    return true;
}

} // namespace gmst
