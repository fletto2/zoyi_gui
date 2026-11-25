#pragma once

#include <QMainWindow>

class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QThread;
class QTimer;
class ScpiClient;
class FramePump;
class WaveView;
class ControlDock;
class HorizBar;

// Main window: link toolbar (port picker, connect, status chip, IDN),
// central waveform view (frame pump + plot), and a bottom SCPI console
// dock that echoes client traffic and sends manual commands.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &autoPort = QString(),
                        bool verbose = false,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void wantOpen(const QString &port);
    void wantClose();
    void wantCommand(const QString &cmd, qint64 cookie);
    void wantQuery(const QString &cmd, qint64 cookie);
    void wantRunStop(bool run);
    void wantArm();
    void wantRequest(int tag, const QString &cmd, bool raw); // FramePump::request
    void wantSet(const QString &cmd);                       // FramePump::sendCommand
    void wantPumpStart(); // 5: the scope pump runs only in SCOP mode

protected:
    void closeEvent(QCloseEvent *event) override; // QSettings

private slots:
    void onPumpReply(int tag, const QString &text, const QByteArray &raw);
    void onAcqStatus(const QString &stat); // TRIG:STAT? -> chip + Run button
    void refreshPorts();
    void toggleConnect();
    void onOpened(const QString &port);
    void onClosed();
    void onFailed(const QString &what);
    void onTraffic(const QString &line);
    void onResponse(qint64 cookie, const QString &cmd, const QString &answer);
    void onScpiError(const QString &err);
    void sendManual();

private:
    void setChipState(const QString &cls, const QString &text);
    void setLinkChip(const QString &ledColor, const QString &text);

    QComboBox *m_ports = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_run = nullptr;    // Run / Stopped toggle (RUN / STOP)
    QPushButton *m_arm = nullptr;    // SING
    QPushButton *m_center = nullptr; // TIM:OFFS 0
    bool m_acqRunning = true;        // last TRIG:STAT? != STOP
    // Force, Autoscale, Screenshot, Remote, auto reconnect.
    QPushButton *m_force = nullptr;
    QPushButton *m_autoscale = nullptr;
    QPushButton *m_more = nullptr;
    class QAction *m_shotAct = nullptr;
    class QAction *m_remoteAct = nullptr;
    class QMenu *m_saveMenu = nullptr;
    class QAction *m_deviceAct = nullptr;
    class QAction *m_autoReAct = nullptr;
    QTimer *m_reTimer = nullptr;
    bool m_userClose = false;  // Disconnect pressed: no auto reconnect
    QString m_lastPort;        // port of the last good link
    void saveScreenshot(const QByteArray &bmp);
    // CSV save of the shown frame (whole = false) or the whole record.
    void saveCsv(bool whole);
    // Device info dialog (IDN, PERF, MEM, BOOT, STAL, clock).
    void showDeviceInfo();
    class QDialog *m_info = nullptr;
    QList<QLabel *> m_infoVals;
    // 5/6: meter mode (SYST:MODE?) picks the page: scope, DMM or generator.
    void setMeterMode(const QString &mode);
    class QStackedWidget *m_stack = nullptr;
    class DmmPanel *m_dmm = nullptr;
    class GenPanel *m_gen = nullptr;
    QComboBox *m_mode = nullptr;
    QString m_meterMode;  // "SCOP" / "DMM" / "GEN", empty = not known yet
    QTimer *m_modeTimer = nullptr; // 2.5 s SYST:MODE? fallback
    bool m_clean = false;          // Clean view active
    QByteArray m_normalState;      // dock layout saved before Clean/DMM/GEN hide
    class QDockWidget *m_leftDock = nullptr;
    QLabel *m_chip = nullptr;
    QLabel *m_link = nullptr;
    // Status bar: trigger state, acquisition, core clock.
    QLabel *m_statLed = nullptr;
    QLabel *m_statTrig = nullptr;
    QLabel *m_statAcq = nullptr;
    QLabel *m_statCore = nullptr;
    void setStatusTele(double srat, int mdep, double coreHz, int known);
    QPlainTextEdit *m_console = nullptr;
    QLineEdit *m_input = nullptr;
    WaveView *m_view = nullptr;
    ControlDock *m_controls = nullptr;
    HorizBar *m_horiz = nullptr;
    class MeasBar *m_meas = nullptr;
    ScpiClient *m_client = nullptr;
    FramePump *m_pump = nullptr;
    QThread *m_thread = nullptr;
    bool m_verbose = false;
    bool m_online = false;
    QString m_port;
    qint64 m_cookie = 0;
};
