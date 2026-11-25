#pragma once

#include <QFont>
#include <QString>

// Global look: one QSS theme from a small set of colour tokens. Fonts use
// family stacks with installed-family detection.
namespace theme {

QFont sansFont();      // IBM Plex Sans -> Segoe UI
QFont monoFont();      // IBM Plex Mono -> Cascadia Mono -> Consolas
QString monoFamily();
QString stylesheet();  // the whole app theme; call once on the QApplication

} // namespace theme
