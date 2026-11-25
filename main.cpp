// ZOYI ZT-703S PC GUI.
// Scaffolding + dark window. link toolbar, SCPI console, *IDN?.

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QPalette>
#include <QStyleFactory>
#include <QtPlugin>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#include "ui/mainwindow.h"
#include "ui/theme.h"

// Static Qt: pull the Windows platform plugin in. It is linked first in the
// Makefile link line; this reference is what loads it out of the archive.
#ifdef _WIN32
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

namespace {

// Dark palette matching the QSS theme.
QPalette darkPalette()
{
    QPalette p;
    const QColor bg     {QStringLiteral("#0b1120")}; // --bg
    const QColor panel  {QStringLiteral("#132034")}; // --panel
    const QColor panel2 {QStringLiteral("#0d1726")}; // --panel2
    const QColor fg     {QStringLiteral("#e2e8f0")}; // --fg
    const QColor dim    {QStringLiteral("#64748b")}; // --dim
    const QColor accent {QStringLiteral("#38bdf8")}; // --accent / --ch2

    p.setColor(QPalette::Window,          bg);
    p.setColor(QPalette::WindowText,      fg);
    p.setColor(QPalette::Base,            panel2);
    p.setColor(QPalette::AlternateBase,   panel);
    p.setColor(QPalette::Text,            fg);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Button,          panel);
    p.setColor(QPalette::ButtonText,      fg);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::ToolTipBase,     panel2);
    p.setColor(QPalette::ToolTipText,     fg);
    p.setColor(QPalette::Highlight,       accent);
    p.setColor(QPalette::HighlightedText, bg);
    p.setColor(QPalette::Link,            accent);
    p.setColor(QPalette::BrightText,      fg);
    return p;
}

} // namespace

namespace {

// -mwindows swallows Qt's warnings (they go to a nonexistent console), and
// they are exactly the evidence cross-thread and lifecycle bugs print
// ("Timers cannot be started from another thread" et al). Mirror every
// message to stderr and to %TEMP%\zt703s-qt.log.
QMutex g_msgMutex;

void logMessage(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    QMutexLocker lock(&g_msgMutex);
    const char *tag = "msg";
    switch (type) {
    case QtDebugMsg:    tag = "debug"; break;
    case QtInfoMsg:     tag = "info";  break;
    case QtWarningMsg:  tag = "WARN";  break;
    case QtCriticalMsg: tag = "CRIT";  break;
    case QtFatalMsg:    tag = "FATAL"; break;
    }
    const QString line = QStringLiteral("[%1] qt-%2: %3\n")
                             .arg(QDateTime::currentDateTime()
                                      .toString(QStringLiteral("HH:mm:ss.zzz")),
                                  QLatin1String(tag), message);
    QFile f(QDir::tempPath() + QStringLiteral("/zt703s-qt.log"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        f.write(line.toUtf8());
    std::fputs(line.toLocal8Bit().constData(), stderr);
    if (type == QtFatalMsg)
        std::abort();
}

#ifdef _WIN32
// Crash dump: an unhandled fault writes %TEMP%\zt703s-crash.dmp
// (MiniDumpNormal plus the thread list) and a line on stderr, so a crash
// outside gdb still leaves a stack to read. Open the dump with gdb or
// WinDbg next to build/zt703s.exe (built with -g).
LONG WINAPI writeCrashDump(EXCEPTION_POINTERS *info)
{
    wchar_t dir[MAX_PATH];
    const DWORD len = GetTempPathW(MAX_PATH, dir);
    if (len == 0 || len > MAX_PATH - 20)
        return EXCEPTION_CONTINUE_SEARCH;
    wchar_t path[MAX_PATH];
    lstrcpyW(path, dir);
    lstrcatW(path, L"zt703s-crash.dmp");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    std::fprintf(stderr, "! CRASH 0x%08lx at %p, dump in %%TEMP%%\\zt703s-crash.dmp\n",
                 info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    // GUI-subsystem stderr is block-buffered when redirected: logs were
    // lost on every crash/kill. Unbuffered from the start.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(writeCrashDump);
#endif
    qInstallMessageHandler(logMessage);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("zt703s"));
    QApplication::setOrganizationName(QStringLiteral("zoyi"));
    // Fusion renders every QSS rule; the native Windows and macOS styles fight them.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(darkPalette());
    QApplication::setFont(theme::sansFont());
    app.setStyleSheet(theme::stylesheet());

    // zt703s [-v] [COMxx]  - -v mirrors console traffic to stderr; a port
    // name auto-connects at startup (test runs and scripted checks).
    QString autoPort;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("-v") || arg == QLatin1String("--verbose"))
            verbose = true;
        else if (!arg.startsWith(QLatin1Char('-')))
            autoPort = arg;
    }

    MainWindow win(autoPort, verbose);
    win.show();
    return app.exec();
}
