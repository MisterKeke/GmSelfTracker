#pragma once

#include <QByteArray>
#include <QString>

namespace gmst {

class PasswordHasher {
public:
    static bool validPasswordLength(const QString &password);
    static bool hash(const QString &password, QByteArray *encoded, QString *error = nullptr);
    static bool verify(const QString &password, const QByteArray &encoded);
};

} // namespace gmst
