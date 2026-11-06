#pragma once

#include <QString>

class QWidget;

namespace gmst {

class GameRepository;
void showObjectHistory(QWidget *parent, const GameRepository &repository, const QString &objectId, const QString &name);

} // namespace gmst
