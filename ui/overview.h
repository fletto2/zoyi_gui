#pragma once

#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include "waveview.h"

// Buffer overview strip under the plot: the whole raw
// record (in roll: the view's own history of 4 screens, box snapped right)
// as a filled, glowing min/max envelope per channel. One shared area that mirrors the plot, each channel at the
// plot's own V/div and offset (8 divisions tall), true scale, traces only.
// The screen window is a box,
// everything outside it is dimmed, the trigger point is an orange tick.
// Drag the box, or click beside it to jump there. The wheel steps the
// timebase like the plot does. All geometry comes from the WaveView, so
// the strip and the plot always agree.
class OverviewStrip : public QWidget {
    Q_OBJECT
public:
    explicit OverviewStrip(WaveView *view, QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(400, 60); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void buildEnvelope();         // cache the record lanes into m_env
    double fracAt(int x) const;   // widget x -> record fraction
    void poll();                  // repaint when the box moved
    // Roll: the view's PC-side history, 4 screens, box on the right.
    void paintRoll(QPainter &p, const WaveView::RollHist &h, double right);
    // (E) The plot's vertical mapping now; caches drawn with another one
    // are rebuilt. ok[c] false = channel has no mapping (not drawn).
    struct VMap {
        bool ok[2] = {false, false};
        double vdiv[2] = {0.0, 0.0}, offs[2] = {0.0, 0.0};
        bool operator==(const VMap &o) const;
    };
    VMap currentMap() const;
    double mapY(const VMap &m, int c, double v) const; // volts -> strip y
    VMap m_envMap, m_rollMap;     // mapping each cache was drawn with

    WaveView *m_view;
    QPixmap m_env;                // cached lanes
    QPixmap m_rollEnv;            // cached roll history bands
    qint64 m_rollGen = -1;        // history gen the roll cache was built from
    bool m_rollOn[2] = {false, false}; // channel has roll history
    qint64 m_envStamp = -1;       // record stamp the cache was built from
    QSize m_envSize;
    QTimer m_poll;                // 30 ms box check
    double m_lastStart = -1.0, m_lastWidth = -1.0, m_lastTrig = -1.0;
    bool m_dragging = false;
    double m_grab = 0.0;          // click fraction minus box start
};
