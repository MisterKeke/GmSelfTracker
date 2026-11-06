#include "app/theme.h"

#include <QApplication>
#include <QFont>

namespace gmst::theme {

void apply(QApplication &application)
{
    QFont body(QStringLiteral("Segoe UI"), 10);
    body.setStyleHint(QFont::SansSerif);
    application.setFont(body);
    application.setStyleSheet(QStringLiteral(R"QSS(
        QWidget { color: #fff2d6; }
        QMainWindow, QWidget#appFrame { background: #211b35; }
        QFrame#topbar { background: #36264f; border: 3px solid #8e72b5; }
        QFrame#sidebar, QFrame#mainPanel { background: #2c2244; border: 3px solid #8a6eab; }
        QFrame#gameArea { background: #2b2142; border: 3px solid #80659f; }
        QLabel#brandTitle { color: #fff0c4; font: 16px "Courier New"; font-weight: 700; }
        QLabel#brandSubtitle { color: #fff5e3; font-size: 9px; font-weight: 800; letter-spacing: 1px; }
        QLabel#pixelHeading { color: #fff1c9; font: 9px "Courier New"; font-weight: 700; }
        QLabel#mutedText { color: #eadcf7; }
        QLabel#inkText { color: #30233d; }
        QPushButton { min-height: 32px; padding: 4px 10px; color: #fff4dc; background: #684b8a; border: 2px solid #c5a6dd; font-weight: 800; }
        QPushButton:hover { color: #33243e; background: #f5d277; border-color: #fff1c8; }
        QPushButton:pressed { padding-top: 6px; padding-bottom: 2px; }
        QPushButton#goldButton { color: #372844; background: #f8d77f; border-color: #fff4cd; }
        QPushButton#softButton { background: #3c2c53; border-color: #856aa5; }
        QPushButton#iconButton { min-width: 38px; max-width: 38px; min-height: 38px; max-height: 38px; padding: 0; font-size: 17px; }
        QPushButton#tabButton { min-height: 40px; padding: 4px 12px; background: #4a355f; border: 3px solid #9674b0; font: 9px "Courier New"; }
        QPushButton#tabButton:checked { color: #382343; background: #ffdf91; border-color: #fff4cd; }
        QPushButton#gameButton { text-align: left; min-height: 46px; background: #392b53; border-color: #785f99; }
        QPushButton#gameButton:checked { color: #30233d; background: #ffe7a2; border-color: #fff; }
        QPushButton#treeButton { text-align: left; color: #30233d; background: #fff5df; border-color: #dec298; }
        QLineEdit, QTextEdit, QComboBox { min-height: 30px; padding: 4px 7px; color: #fff; background: #241c35; border: 2px solid #795f99; selection-background-color: #7657a7; }
        QTextEdit { min-height: 72px; }
        QLineEdit:focus, QTextEdit:focus, QComboBox:focus { border-color: #f5d277; }
        QComboBox QAbstractItemView { color: #fff2d6; background: #36264f; selection-background-color: #7657a7; }
        QProgressBar#goalProgressBar { color: #30233d; background: #fffaf0; border: 1px solid #b18c68; text-align: center; }
        QProgressBar#goalProgressBar::chunk { background: #9dd19a; }
        QWidget#goalList { background: #2b2142; }
        QFrame#goalCard QCheckBox, QFrame#goalCard QLabel { color: #30233d; }
        QFrame#goalCard QDoubleSpinBox { color: #30233d; background: #fffaf0; border: 1px solid #b18c68; }
        QTreeWidget { color: #fff2d6; background: transparent; border: 0; outline: 0; }
        QTreeWidget::item { min-height: 32px; }
        QTreeWidget::item:selected { color: #30233d; background: #ffe7a2; }
        QScrollArea { border: 0; background: transparent; }
        QScrollBar:vertical { width: 12px; background: #211b35; margin: 0; }
        QScrollBar::handle:vertical { min-height: 24px; background: #7657a7; border: 2px solid #c8aaff; }
        QScrollBar:horizontal { height: 12px; background: #211b35; margin: 0; }
        QScrollBar::handle:horizontal { min-width: 24px; background: #7657a7; border: 2px solid #c8aaff; }
        QDialog { background: #fff2d6; }
        QDialog QLabel { color: #30233d; }
        QDialog QLineEdit, QDialog QTextEdit, QDialog QComboBox { color: #30233d; background: #fffaf0; border-color: #b99973; }
        QTableWidget#historyTimeline { color: #30233d; background: #fffaf0; gridline-color: #e1cca3;
            selection-color: #30233d; selection-background-color: #f8d77f; }
        QTableWidget#historyTimeline QHeaderView::section { color: #30233d; background: #f4ddb0; padding: 5px; }
        QStatusBar { color: #fff2d6; background: #36264f; border-top: 2px solid #8e72b5; }
        QToolTip { color: #30233d; background: #fff2d6; border: 2px solid #c6a876; }
    )QSS"));
}

} // namespace gmst::theme
