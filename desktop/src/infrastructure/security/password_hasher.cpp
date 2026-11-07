#include "infrastructure/security/password_hasher.h"

extern "C" {
#include "crypt_blowfish.h"
}

#include <QRandomGenerator>
#include <array>
#include <cstring>

namespace gmst {
namespace {

constexpr int bcryptCost = 10;
constexpr char bcryptPrefix[] = "$2a$";
constexpr int bcryptHashLength = 60;
constexpr int bcryptSaltLength = 29;

bool validBcryptHash(const QByteArray &hash)
{
    if (hash.size() != bcryptHashLength || hash.at(0) != '$' || hash.at(1) != '2' ||
        (hash.at(2) != 'a' && hash.at(2) != 'b' && hash.at(2) != 'y') || hash.at(3) != '$' ||
        hash.at(6) != '$') return false;
    const char firstCostDigit = hash.at(4);
    const char secondCostDigit = hash.at(5);
    if (firstCostDigit < '0' || firstCostDigit > '3' || secondCostDigit < '0' || secondCostDigit > '9') return false;
    const int cost = (firstCostDigit - '0') * 10 + (secondCostDigit - '0');
    return cost >= 4 && cost <= 31;
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference |= static_cast<unsigned char>(left.at(index) ^ right.at(index));
    return difference == 0;
}

} // namespace

bool PasswordHasher::validPasswordLength(const QString &password)
{
    const QByteArray bytes = password.toUtf8();
    return bytes.size() >= 10 && bytes.size() <= 72;
}

bool PasswordHasher::hash(const QString &password, QByteArray *encoded, QString *error)
{
    if (error) error->clear();
    if (!encoded || !validPasswordLength(password)) {
        if (error) *error = QStringLiteral("Passwords must be 10–72 UTF-8 bytes long.");
        return false;
    }
    QByteArray passwordBytes = password.toUtf8();
    std::array<unsigned char, 16> saltBytes{};
    for (qsizetype index = 0; index < static_cast<qsizetype>(saltBytes.size()); index += 4) {
        const quint32 random = QRandomGenerator::system()->generate();
        std::memcpy(saltBytes.data() + index, &random, sizeof(random));
    }
    std::array<char, 30> setting{};
    std::array<char, 128> output{};
    if (!_crypt_gensalt_blowfish_rn(bcryptPrefix, bcryptCost, reinterpret_cast<const char *>(saltBytes.data()),
                                    static_cast<int>(saltBytes.size()), setting.data(), static_cast<int>(setting.size())) ||
        !_crypt_blowfish_rn(passwordBytes.constData(), setting.data(), output.data(), static_cast<int>(output.size()))) {
        passwordBytes.fill('\0');
        if (error) *error = QStringLiteral("The bcrypt provider could not generate a password hash.");
        return false;
    }
    passwordBytes.fill('\0');
    *encoded = QByteArray(output.data(), bcryptHashLength);
    return true;
}

bool PasswordHasher::verify(const QString &password, const QByteArray &encoded)
{
    if (!validPasswordLength(password) || !validBcryptHash(encoded)) return false;
    QByteArray passwordBytes = password.toUtf8();
    std::array<char, 128> output{};
    const QByteArray setting = encoded.left(bcryptSaltLength);
    const char *result = _crypt_blowfish_rn(passwordBytes.constData(), setting.constData(), output.data(), static_cast<int>(output.size()));
    passwordBytes.fill('\0');
    return result && constantTimeEqual(QByteArray(output.data(), bcryptHashLength), encoded);
}

} // namespace gmst
