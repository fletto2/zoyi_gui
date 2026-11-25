#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QList>

#include "core/measure.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
struct WaveFrame;

// Bottom measurement card: one line, CH1
// items | CH2 items, each "tag value". "+" adds a measurement, a click on
// an item removes it. Values come from meas::screenBasis() on the raw
// screen window, 4 times a second like the meter (ReadoutScheduler 250 ms).
// A decimated window (5 us/div and slower) shows "--" until the firmware
// sends its own readouts (batch 17 item 1).
class MeasBar : public QFrame {
    Q_OBJECT
public:
    explicit MeasBar(QWidget *parent = nullptr);

public slots:
    void setFrame(const WaveFrame &f);
    void setFirmwareMeas(int ch, const meas::Result &res);
    void setOnline(bool on);
    // Hardware counter (MEAS:FCNT?, trigger channel) at the right end.
    void setCounter(bool known, double hz, int quality, int channel);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void rebuild();
    void paintValues();
    void addMenu(int ch, QPushButton *anchor);

    QHBoxLayout *m_line = nullptr;
    QList<meas::Kind> m_sel[2];
    meas::Result m_res[2];
    meas::Result m_fwRes[2];
    bool m_fwHas[2] = {false, false};
    bool m_decimated[2] = {false, false};
    bool m_on[2] = {false, false};
    QString m_why[2]; // tooltip when a channel has no values
    QList<QLabel *> m_items[2];
    QElapsedTimer m_last;
    QLabel *m_counter = nullptr;
    QString m_counterText;
};
