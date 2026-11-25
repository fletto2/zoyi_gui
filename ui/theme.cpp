#include "theme.h"

#include <QFontDatabase>
#include <QStringList>

namespace {

QString pickFamily(std::initializer_list<QString> candidates)
{
    const QStringList families = QFontDatabase::families();
    for (const QString &c : candidates)
        if (families.contains(c))
            return c;
    return QString();
}

} // namespace

namespace theme {

// Picked once. The plot calls monoFamily() on every paint, and
// QFontDatabase::families() builds the whole family list each time.
QString monoFamily()
{
    static const QString family =
        pickFamily({QStringLiteral("IBM Plex Mono"), QStringLiteral("Cascadia Mono"),
                    QStringLiteral("Consolas"), QStringLiteral("Menlo")});
    return family;
}

QFont sansFont()
{
    static const QString family = pickFamily(
        {QStringLiteral("IBM Plex Sans"), QStringLiteral("Segoe UI"),
         QStringLiteral("Helvetica Neue")});
    return QFont(family, 10);
}

QFont monoFont()
{
    return QFont(monoFamily(), 10);
}

QString stylesheet()
{
    const QString mono = monoFamily();

    // BISECT-D fixed: full stylesheet restored (crash was missing
    // -DQT_NO_DEBUG/-DQT_*_LIB, not the QSS - see Makefile).
    return QStringLiteral(R"QSS(
* { outline: none; }

QMainWindow, QDialog {
    background: #0b1120;
}

/* ---------- toolbar (#toolbar: bg, 7px/10px padding, 10px gap) -------- */

QWidget#topbar {
    background: #0b1120;
    border-bottom: 1px solid #1d2c44;
}
QLabel#topbarLabel {
    color: #94a3b8;
    font-size: 11px;
}

/* .btn ------------------------------------------------------------------ */

QPushButton {
    background: #132034;
    border: 1px solid #243449;
    border-radius: 5px;
    padding: 5px 12px;
    color: #e2e8f0;
    font-size: 13px;
}
QPushButton:hover {
    border-color: #334c73;
}
QPushButton:pressed {
    background: #1b2c47;
}
QPushButton:disabled {
    background: #0d1726;
    border-color: #1d2c44;
    color: #64748b;
}
QPushButton[cls="primary"] {
    background: #0e2a1a;
    border-color: #1c5c38;
    color: #86efac;
}
QPushButton[cls="stop"] {
    background: #2a0e12;
    border-color: #5c1c28;
    color: #fca5a5;
}
QPushButton[cls="toggled"] {
    background: #12304a;
    border-color: #2a6a9e;
}
/* Same specificity as the cls rules, so it must come after them, or a
   disabled primary button looks live (test pass 1, bug 3). */
QPushButton[cls]:disabled {
    background: #0d1726;
    border-color: #1d2c44;
    color: #64748b;
}

/* port picker (.val / .dropdown look) ----------------------------------- */

QComboBox#portCombo {
    background: #0b1120;
    border: 1px solid #1d2c44;
    border-radius: 4px;
    padding: 3px 26px 3px 8px;
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 12px;
    min-width: 230px;
}
QComboBox#portCombo:hover {
    border-color: #334c73;
}
QComboBox#portCombo:focus {
    border-color: #2a6a9e;
}
QComboBox#portCombo::drop-down {
    border: none;
    width: 20px;
}
QComboBox#portCombo::down-arrow {
    width: 0;
    height: 0;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #94a3b8;
}
QComboBox QAbstractItemView {
    background: #101c30;
    border: 1px solid #243449;
    border-radius: 6px;
    padding: 4px;
    color: #e2e8f0;
    selection-background-color: #1b2c47;
    selection-color: #e2e8f0;
    outline: none;
}

/* chips (.chip / .chip.state) ------------------------------------------- */

QLabel[cls="chip"] {
    background: #0d1726;
    border: 1px solid #1d2c44;
    border-radius: 5px;
    padding: 4px 10px;
    color: #94a3b8;
    font-family: "%MONO%";
    font-size: 11px;
}
QLabel#stateChip {
    background: #0d1726;
    border: 1px solid #1d2c44;
    border-radius: 5px;
    padding: 4px 10px;
    color: #94a3b8;
    font-family: "%MONO%";
    font-size: 11px;
    font-weight: 700;
    min-width: 92px;
}
QLabel#stateChip[cls="ok"] {
    background: #0e2a1a;
    border-color: #1c5c38;
    color: #86efac;
}
QLabel#stateChip[cls="stop"] {
    background: #2a0e12;
    border-color: #5c1c28;
    color: #fca5a5;
}
QLabel#stateChip[cls="wait"] {
    background: #2a210e;
    border-color: #6e5a1c;
    color: #fde68a;
}

/* placeholder + console -------------------------------------------------- */

QDockWidget {
    color: #94a3b8;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: #0d1726;
    border-top: 1px solid #1d2c44;
    padding: 4px 10px 4px 10px;
    font-size: 11px;
}
QPlainTextEdit#console {
    background: #0e1a2b;
    border: 1px solid #1d2c44;
    border-radius: 7px;
    padding: 6px;
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 12px;
    selection-background-color: #1b2c47;
}
QLineEdit#cmdInput {
    background: #0b1120;
    border: 1px solid #1d2c44;
    border-radius: 4px;
    padding: 3px 8px;
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 12px;
}
QLineEdit#cmdInput:focus {
    border-color: #2a6a9e;
}

/* control dock + horizontal bar ------------------------------------------ */

QWidget#controlDock {
    background: #0b1120;
    border-right: 1px solid #1d2c44;
}
QFrame#card, QFrame#sumCard {
    background: #132034;
    border: 1px solid #1d2c44;
    border-radius: 7px;
}
QFrame#sumCard:hover {
    border-color: #3b557f;
}
QPushButton[cls="collapseTab"] {
    background: transparent;
    border: 0;
    padding: 0 2px;
    color: #94a3b8;
    font-size: 10px;
    letter-spacing: 1px;
    text-transform: uppercase;
}
QPushButton[cls="collapseTab"]:hover {
    color: #e2e8f0;
}
QLabel[cls="sumBig"] {
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 14px;
}
QLabel[cls="sumSm"] {
    color: #94a3b8;
    font-family: "%MONO%";
    font-size: 10px;
}
QLabel[cls="sumHint"] {
    color: #475569;
    font-size: 9px;
    margin-top: 3px;
}
QLabel[cls="cardTitle"] {
    color: #94a3b8;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 1px;
}
QLabel[cls="dim"] {
    color: #64748b;
    font-size: 10px;
}
QLabel[cls="rowLabel"], QLabel[cls="barLabel"] {
    color: #94a3b8;
    font-size: 10px;
}
QLabel[cls="barLabel"] {
    font-size: 11px;
    letter-spacing: 1px;
}
QLabel[cls="val"] {
    background: #0b1120;
    border: 1px solid #1d2c44;
    border-radius: 4px;
    padding: 3px 6px;
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 13px;
}
QLabel[cls="val"][big="true"] {
    font-size: 17px;
    padding: 7px 6px;
}
QPushButton[cls="arrow"] {
    min-width: 24px;
    max-width: 24px;
    min-height: 24px;
    padding: 0;
    font-size: 10px;
    color: #94a3b8;
}
QPushButton[cls="iconbtn"] {
    background: transparent;
    border: 1px solid #1d2c44;
    border-radius: 4px;
    padding: 2px 6px;
    color: #94a3b8;
}
QPushButton[cls="iconbtn"]:hover {
    color: #e2e8f0;
    border-color: #3b557f;
}
QPushButton[cls="seg"] {
    background: #0d1726;
    border: 1px solid #243449;
    border-radius: 0;
    padding: 4px 6px;
    color: #94a3b8;
    font-size: 11px;
}
QPushButton[cls="seg"]:checked {
    background: #12304a;
    color: #e2e8f0;
}
QPushButton[cls="switch"] {
    background: #1b2c47;
    border: 1px solid #243449;
    border-radius: 9px;
    padding: 1px 8px;
    color: #94a3b8;
    font-size: 10px;
    font-weight: 700;
}
QPushButton[cls="switch"]:checked {
    background: #155e42;
    border-color: #1c5c38;
    color: #86efac;
}
QWidget#horiz {
    background: #0b1120;
    border-bottom: 1px solid #1d2c44;
}
QScrollArea, QScrollArea > QWidget > QWidget#controlDock {
    background: #0b1120;
}

/* status bar ------------------------------------------------------------- */

QStatusBar#status {
    background: #0b1120;
    border-top: 1px solid #1d2c44;
    color: #94a3b8;
    font-size: 11px;
    min-height: 26px;
}
QStatusBar#status::item {
    border: none;
}
QStatusBar#status QLabel {
    color: #94a3b8;
    padding: 0 7px;
}

/* thin scrollbars -------------------------------------------------------- */

QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #243449;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: #334c73;
}
QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #243449;
    border-radius: 4px;
    min-width: 24px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* bottom row + measurement card ------------------------------------------ */

QWidget#bottomRow {
    background: #0d1726;
    border-top: 1px solid #1d2c44;
}
QFrame#measCard {
    background: #132034;
    border: 1px solid #1d2c44;
    border-radius: 7px;
}
QFrame#measSep {
    background: #1d2c44;
}
QLabel[cls="measCh"] {
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 1px;
}
QLabel[cls="measItem"] {
    color: #e2e8f0;
    font-family: "%MONO%";
    font-size: 15px;
}
QLabel[cls="measItem"]:hover {
    color: #f87171;
}
QPushButton[cls="measAdd"] {
    background: transparent;
    border: 1px solid #1d2c44;
    border-radius: 3px;
    color: #64748b;
    font-size: 10px;
    padding: 2px 5px;
    min-width: 0;
}
QPushButton[cls="measAdd"]:hover {
    color: #e2e8f0;
    border-color: #3b557f;
}

/* menus + tooltips (.dropdown) ------------------------------------------- */

QMenu {
    background: #101c30;
    border: 1px solid #243449;
    border-radius: 6px;
    padding: 5px;
    color: #e2e8f0;
}
QMenu::item {
    padding: 6px 24px;
    border-radius: 4px;
}
QMenu::item:selected {
    background: #1b2c47;
}
QToolTip {
    background: #0d1726;
    border: 1px solid #243449;
    border-radius: 4px;
    color: #e2e8f0;
    padding: 4px 8px;
}
)QSS")
        .replace(QStringLiteral("%MONO%"), mono);
}

} // namespace theme
