#pragma once

#include <QColor>

class QApplication;

namespace gmst::theme {

inline const QColor night{QStringLiteral("#1b172b")};
inline const QColor background{QStringLiteral("#211b35")};
inline const QColor sidebar{QStringLiteral("#2c2244")};
inline const QColor plum{QStringLiteral("#463363")};
inline const QColor plumDark{QStringLiteral("#36264f")};
inline const QColor purple{QStringLiteral("#7657a7")};
inline const QColor lavender{QStringLiteral("#c8aaff")};
inline const QColor cream{QStringLiteral("#fff2d6")};
inline const QColor creamDeep{QStringLiteral("#f4ddb0")};
inline const QColor ink{QStringLiteral("#30233d")};
inline const QColor mutedInk{QStringLiteral("#715c74")};
inline const QColor gold{QStringLiteral("#f5d277")};
inline const QColor green{QStringLiteral("#65b96c")};
inline const QColor blue{QStringLiteral("#66a6e4")};
inline const QColor red{QStringLiteral("#e67f75")};
inline const QColor line{QStringLiteral("#c6a876")};
inline const QColor shadow{QStringLiteral("#110e1d")};

void apply(QApplication &application);

} // namespace gmst::theme
