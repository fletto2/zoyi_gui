#pragma once

#include <QFrame>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Generator page. Shown when the meter is in GEN mode
// (SYST:MODE GEN). Reads every SOUR:* setting and OUTP? on entry and after
// each set (tags 40-49 through MainWindow -> FramePump::request). A shape
// change with the output on goes OUTP 0, SOUR:FUNC, OUTP 1, since the
// firmware refuses SOUR:FUNC while the output runs (-221).
class GenPanel : public QFrame {
    Q_OBJECT
public:
    explicit GenPanel(QWidget *parent = nullptr);

    static constexpr int kTagFirst = 40;
    static constexpr int kTagLast = 49;

    void setActive(bool on);
    void onReply(int tag, const QString &text);

signals:
    void wantRequest(int tag, const QString &cmd, bool raw);
    void wantSet(const QString &cmd);

private:
    enum Tag { TFunc = 40, TFreq, TDuty, TProf, TLow, THigh, TDrive, TPath, TOut, TStart };
    void readAll();
    void set(const QString &cmd);

    bool m_active = false;
    bool m_outOn = false;
    QComboBox *m_shape = nullptr;
    QDoubleSpinBox *m_freq = nullptr;
    QDoubleSpinBox *m_duty = nullptr;
    QComboBox *m_prof = nullptr;
    QSpinBox *m_low = nullptr;
    QSpinBox *m_high = nullptr;
    QComboBox *m_drive = nullptr;
    QLabel *m_path = nullptr;
    QComboBox *m_startup = nullptr;
    QPushButton *m_out = nullptr;
};
