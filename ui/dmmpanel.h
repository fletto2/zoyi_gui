#pragma once

#include <QFile>
#include <QFrame>
#include <QTextStream>
#include <QVector>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class DmmTrend;

// DMM page. Shown when the meter is in DMM mode (SYST:MODE DMM).
// Polls SYST:DMM:RES? 4 times a second and SYST:DMM:STAT? once a second
// through MainWindow -> FramePump::request (tags 20-29). Function keys send
// CONF:*, the rest AUTO, RANG, SYST:DMM:HOLD/REL, FUNC2 and
// SYST:DMM:TEMP:UNIT. Each set is followed by a fresh poll, so the page
// always shows what the meter reports, never what was asked.
class DmmPanel : public QFrame {
    Q_OBJECT
public:
    explicit DmmPanel(QWidget *parent = nullptr);

    static constexpr int kTagFirst = 20;
    static constexpr int kTagLast = 29;

    void setActive(bool on); // polling on/off (page shown and link up)
    void onReply(int tag, const QString &text);

signals:
    void wantRequest(int tag, const QString &cmd, bool raw);
    void wantSet(const QString &cmd);

private:
    enum Tag { TRes = 20, TStat, TRanges, TUnit };
    void poll();
    void set(const QString &cmd);
    void applyResult(const QString &text);
    void applyStats(const QString &text);
    void applyRanges(const QString &text);
    void toggleLog(bool on);
    QString unitFor(int fn) const;

    QTimer *m_timer = nullptr;
    int m_tick = 0;
    bool m_active = false;
    int m_fn = -1;           // dmm::Function index, -1 unknown
    QVector<double> m_ranges; // full scales of the current function
    unsigned long m_lastSeq = 0;

    QLabel *m_value = nullptr;
    QLabel *m_unit = nullptr;
    QLabel *m_func = nullptr;
    QLabel *m_second = nullptr;
    QLabel *m_flags = nullptr;
    QLabel *m_stats = nullptr;
    QList<QPushButton *> m_fnButtons;
    QPushButton *m_auto = nullptr;
    QComboBox *m_range = nullptr;
    QPushButton *m_hold = nullptr;
    QPushButton *m_rel = nullptr;
    QPushButton *m_freq = nullptr;
    QComboBox *m_tunit = nullptr;
    QPushButton *m_log = nullptr;
    DmmTrend *m_trend = nullptr;
    QFile m_logFile;
    QTextStream m_logOut;
};
