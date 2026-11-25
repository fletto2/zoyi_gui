#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "core/devicestate.h"

class QLabel;
class QPushButton;

// Left control dock: per channel the display
// switch, V/div arrows, position in divisions with arrows and zero,
// coupling AC/DC and probe 1x/10x/100x. Values come from the device state
// model (FramePump::stateChanged); every control emits a set that the pump
// sends and reads back, so the widgets always show what the meter holds.
class ControlDock : public QWidget {
    Q_OBJECT
public:
    explicit ControlDock(QWidget *parent = nullptr);
    bool isCollapsed() const { return m_collapsed; }

public slots:
    void setState(const DeviceState &s);
    void setOnline(bool online);
    void setCollapsed(bool collapsed);
    void setCursorReadout(int mode, double tA, double tB, double vA, double vB, int ch);

signals:
    void channelValue(int ch, int field, double value); // DeviceState::Field
    void triggerValue(int field, double value);         // DeviceState::TrigField
    void trigAlignChanged(bool on);                     // PC sub-sample alignment
    void collapsedChanged(bool collapsed);
    void cursorModeChanged(int mode);
    void cursorChannelChanged(int ch);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    // Trigger card.
    struct TrigCard {
        QPushButton *src[2] = {nullptr, nullptr};
        QPushButton *rise = nullptr, *fall = nullptr;
        QLabel *level = nullptr;
        QPushButton *swe[3] = {nullptr, nullptr, nullptr};
        QPushButton *nrejOff = nullptr, *nrejOn = nullptr;
        QPushButton *alignOff = nullptr, *alignOn = nullptr;
    };
    QWidget *buildTrigCard();
    QWidget *buildCursorCard();
    void refreshTrig();
    void stepLevel(int dir);
    TrigCard m_trig;
    QPushButton *m_curTime = nullptr;
    QPushButton *m_curLevel = nullptr;
    QPushButton *m_curCh = nullptr;
    QLabel *m_curReadout = nullptr;
    bool m_online = false;

    struct Card {
        QPushButton *on = nullptr;
        QLabel *probeLbl = nullptr;
        QLabel *scale = nullptr;
        QLabel *pos = nullptr;
        QPushButton *ac = nullptr, *dc = nullptr;
        QPushButton *prob[3] = {nullptr, nullptr, nullptr};
    };
    QWidget *buildCard(int c);
    void stepScale(int c, int dir);
    void stepPos(int c, double divs);
    void refresh(int c);
    void applyState(const DeviceState &s);
    void localStep(); // an arrow step changed m_state locally

    QWidget *buildSummaryBox();
    struct SumCard {
        QLabel *scale = nullptr;
        QLabel *meta = nullptr;
        QLabel *pos = nullptr;
    };
    struct SumTrig {
        QLabel *main = nullptr;
        QLabel *level = nullptr;
        QLabel *sweep = nullptr;
    };
    QPushButton *m_toggleBtn = nullptr;
    QWidget *m_fullBox = nullptr;
    QWidget *m_sumBox = nullptr;
    SumCard m_sumCh[2];
    SumTrig m_sumTrig;
    QLabel *m_sumCur = nullptr;
    bool m_collapsed = false;

    Card m_card[2];
    DeviceState m_state;
    // The pump reports the meter's value after every set, so a
    // refused step snaps back. While arrow steps auto-repeat, read-backs of
    // earlier steps would pull the local value back mid-burst: hold them
    // until kHoldMs after the last local step, then show the newest one.
    static constexpr int kHoldMs = 450;
    QElapsedTimer m_lastStep;
    QTimer m_hold;
    DeviceState m_pending;
    bool m_havePending = false;
};

// Horizontal bar above the plot: timebase arrows and value,
// trigger position and a zero button. Values follow the frames.
class HorizBar : public QWidget {
    Q_OBJECT
public:
    explicit HorizBar(QWidget *parent = nullptr);

public slots:
    void setHoriz(double scal, double pos); // s/div, TRIG:POS %
    void setOnline(bool online);
    void setRoll(bool known, bool on);      // ACQ:ROLL read-back

signals:
    void timebaseStep(int dir); // +1 slower (zoom out), -1 faster
    void rollChanged(bool on);  // ACQ:ROLL 0|1

private:
    QLabel *m_tb = nullptr;
    QLabel *m_pos = nullptr;
    QPushButton *m_btn[2] = {nullptr, nullptr};
    QPushButton *m_rollOff = nullptr, *m_rollOn = nullptr;
};

namespace fmt {
QString volts(double v);    // 500 mV, 1.00 V
QString seconds(double s);  // 200 us, 1.00 ms (micro sign)
} // namespace fmt
