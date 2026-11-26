#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QPoint>
#include <QTimer>
#include <QWidget>

#include "core/framepump.h"

class QPainter;

// The waveform canvas, custom QPainter with custom painting:
// bg #0b1120, 10x8 grid #243449 with full-strength center axes, #3b506e
// center ticks, traces as glow (3 px, 20% alpha) + crisp (1.6 px) passes
// in #facc15 / #38bdf8, ground tags at the left edge, orange trigger
// marker (t=0) with its dotted line, #3b506e crosshair on hover.
//
// Rendering is tick-driven: a ~16 ms timer decays the trace layer,
// draws any newly stored frame and repaints, so the CRT emulation runs at
// a fixed 60 f/s wall-clock rate no matter how fast frames arrive (the
// phosphor retains ~40% per 16.67 ms of elapsed time; at 30 f/s input it
// simply decays twice between frames). setFrame() only stores the latest
// frame (latest-frame-wins: a slow paint never builds a queue).
//
// Two trace layers (fixed 2026-11-19): m_live holds the newest frame at
// full brightness and never decays. When the next frame arrives the old
// live layer is stamped into m_phosphor, which holds only the ghosts and
// decays. So the current trace never dims or flickers between frames.
// Grid/markers/crosshair are crisp overlays that never accumulate. Any
// view or data-mapping change clears the ghosts so they never land in
// the wrong place.
//
// The view is the device: while running, the wheel steps the
// device's TIM:SCAL (view span = 10*scal, previewed instantly). Drag pans
// through the record (viewShiftRequested: only WAV:STAR/STOP move, the
// trigger stays put). Frames then confirm the window; a
// 1.5 s preview lock stops in-flight old frames from snapping the view
// back. While stopped, wheel and drag
// zoom/pan freely inside the stored record and each change re-fetches
// real samples (WAV:STAR/STOP, POIN 0 when it fits).
class WaveView : public QWidget {
    Q_OBJECT
public:
    explicit WaveView(QWidget *parent = nullptr);

    void setFrame(const WaveFrame &f); // stores latest; the tick renders it
    // Whole raw record. Always feeds the overview strip. A stopped one
    // (rec.running == false) is the deep record: it replaces the displayed
    // frame, and wheel/drag then zoom and pan in it locally, no re-fetch.
    void setRecord(const WaveFrame &rec);
    const WaveFrame &record() const { return m_rec; }
    const WaveFrame &frame() const { return m_frame; } // CSV of the shown frame
    bool hasFrame() const { return m_have; }
    bool hasRecord() const { return m_haveRec; }
    // Overview strip geometry as fractions of the record: the screen box
    // start and width, and the trigger point. False until there is a frame.
    bool overviewBox(double &start, double &width, double &trig) const;
    void overviewMoveTo(double startFrac, bool final); // strip box drag / click
    void zoomStep(int dir);   // -1 in, +1 out, anchored at center
    // Wheel/trackpad zoom anchored at frac of the grid: summed travel, no
    // momentum, natural scrolling undone. Also used by the overview strip.
    void wheelZoom(QWheelEvent *event, double frac);
    void centerTrigger();     // Center button: trigger point to screen center
    // Trigger level line + "T" tag. From the state model.
    void setTrigger(int source, double level, bool known);
    // Roll history for the overview strip: the pump
    // fetches no whole record while rolling, so the view keeps its own
    // miniature, min/max buckets over kHistScreens screen widths, newest on
    // the right. Times are s on the PC clock since the first roll frame.
    static constexpr int kHistBuckets = 1200;
    static constexpr int kHistScreens = 4;
    struct RollHist {
        int n = 0;                   // buckets, 0 = no history
        double bucketS = 0.0;        // s per bucket
        double headS = 0.0;          // time of the right end of the last bucket
        QVector<float> lo[2], hi[2]; // per channel, NaN = no data
        qint64 gen = 0;              // bumps on every change
    };
    // True while rolling. dispRightS is the time at the plot's right edge
    // now (the smooth scroll draws m_rollD late), so the strip scrolls with
    // the plot.
    bool rollHistory(const RollHist *&h, double &dispRightS) const;
    // Overview strip (E): the plot's own vertical mapping for channel c
    // (V/div, offset V, screen divs = (v + offs) / vdiv), so the strip
    // draws each trace where the plot does. False when the plot has none.
    bool channelMap(int c, double &vdiv, double &offs) const;
    QSize minimumSizeHint() const override { return QSize(280, 180); }
    // Cursors: mode bit 1 = A/B time cursors, bit 2 = A/B level cursors
    // on channel ch (0/1). Dragged on the plot. cursorsChanged reports
    // their values for the bottom-row readout.
    void setCursorMode(int mode);
    void setCursorChannel(int ch);
    int cursorMode() const { return m_curMode; }

signals:
    void cleanViewToggled(); // : context menu "Clean view" (F11)
    void cursorsChanged(int mode, double tA, double tB, double vA, double vB, int ch);
    void recordChanged();                 // a new whole record arrived
    void centerRequested();               // running: TIM:OFFS 0
    // View-is-the-device inputs; MainWindow wires these to the pump.
    void scaleChangeRequested(double sDiv);             // running: TIM:SCAL
    void viewShiftRequested(double s);                  // running: pan in the record
    void viewWindowChanged(double center, double span); // stopped: STAR/STOP
    void channelOffsetRequested(int ch, double volts);  // ground tag drag: CHAN:OFFS
    void triggerLevelRequested(double volts);           // T tag drag: TRIG:EDGE:LEV
    void renderStats(double fps, qreal lastPaintMs, qreal lastDrawMs); // 2 s, debug
    void renderSincChanged(bool on);      // Sinc vs Line: the pump aligns on that curve

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Grid {
        int x, y, w, h;
    };
    Grid grid() const;
    int firstChannel() const; // 0/1 with valid data, -1 none
    double dataSpan() const;  // (count-1)*xinc: samples map edge to edge
    double dataStart() const;
    void applyDeviceWindow(); // running: view = the device screen window
    void viewToRecord();      // stopped (first frame): fit the record
    void clampView();
    QList<double> spanLadder() const; // absolute 1-2-5 spans (100 ns .. 200 s)
    void zoomAt(int dir, double frac); // wheel logic, anchor at frac of the grid

    // Vertical mapping: the frame's own V/div and offset; a ground
    // tag drag previews its offset locally until the meter confirms.
    double chanVdiv(int c) const;
    double chanOffs(int c) const;
    double tagY(int c, const Grid &g) const; // ground tag y, clamped to the grid
    int tagAt(const QPoint &pos) const;      // channel 0/1 under pos, -1 none
    // Trigger level: y of the level on the source channel's scale,
    // unclamped; false when there is no level or no source data.
    bool trigY(const Grid &g, double &y) const;
    double trigLevel() const;                // previewed while dragged / held
    bool trigTagAt(const QPoint &pos) const;

    void onRenderTick();      // ~60 Hz: decay, draw new frame, update
    void drawTraces(QPainter &p, const Grid &g); // glow+crisp(+dots), clipped
    void renderLive(bool fresh); // redraw the live layer; fresh = new frame
    bool ensurePhosphor();    // (re)create both layers to fit the widget
    bool phosKeyMatches() const; // view/data mapping unchanged since last frame?
    void phosKeySave();

    WaveFrame m_frame;
    bool m_have = false;
    WaveFrame m_rec;          // last whole record (overview / deep)
    bool m_haveRec = false;
    bool m_deep = false;      // stopped: m_frame IS the deep record
    double m_span = 0.0;   // visible duration, s
    double m_center = 0.0; // visible center time, s (trigger-relative)
    // Running pan: view center minus the device screen
    // center, s. Drag and the strip box change only this, never the trigger.
    double m_runShift = 0.0;
    double devCenter() const; // running: the device screen center, s
    void emitShift(bool final); // throttled viewShiftRequested
    bool m_dragging = false;
    QPoint m_dragPos;
    QElapsedTimer m_emitClock; // throttles drag-time signal emits
    bool m_mouseIn = false;
    QPoint m_mouse;
    bool m_renderSinc = true; // trace reconstruction: sinc (default) or line
    bool m_persist = true;    // intensity fade (phosphor decay)
    // Extras, all persisted (trace/*).
    bool m_infinite = false;  // infinite persistence: ghosts never decay
    int m_avgN = 1;           // PC-side running average over n frames (1 = off)
    bool m_xy = false;        // XY: CH1 on X, CH2 on Y
    QVector<float> m_avgAcc[2]; // average accumulator per channel
    double m_avgKey[2][3] = {}; // per channel: xinc, xorig, scal of the accumulator
    void averageFrame(WaveFrame &f);
    void drawXY(QPainter &p, const Grid &g);
    QPixmap m_phosphor;       // ghosts of PREVIOUS frames only, decaying
    QPixmap m_live;           // the newest frame, always full brightness
    bool m_liveDirty = false; // view/size/mode changed: redraw m_live
    qreal m_phosDpr = 0.0;    // devicePixelRatio the layer was created with
    QTimer m_render;          // ~60 Hz render tick
    QElapsedTimer m_decayClock; // wall time since the last phosphor decay
    QElapsedTimer m_lastFresh;  // since a new frame was drawn (ghosts still fading)
    QElapsedTimer m_lastUpdate; // since the tick last asked for a repaint
    bool m_newFrame = false;
    // Smooth roll: the trace is drawn m_rollD ms late and slid left by
    // the PC time since its WAV:SNAP on every tick. m_rollShift (s) is
    // added to every sample time in drawTraces().
    double m_rollShift = 0.0;
    double m_rollD = 150.0;       // display delay, ms, slewed toward target
    double m_rollTarget = 150.0;  // longest recent snap-to-next-arrival, ms
    qint64 m_rollPrevSnap = 0;
    bool rollActive() const { return m_have && m_frame.running && m_frame.roll; }
    RollHist m_hist;
    qint64 m_histBase = 0;     // PC ms of history time 0
    void histFeed(const WaveFrame &f);
    bool m_devLock = false;   // preview pending: frames don't move the view
    QElapsedTimer m_devLockSince;
    double m_wheelAcc = 0.0;    // wheel/trackpad travel not yet turned into a step
    QElapsedTimer m_wheelStep;  // time since the last wheel zoom step
    QElapsedTimer m_holdClock; // valid while unsettled frames are held
    // Render stats (paint-time + f/s readout).
    QElapsedTimer m_statClock;
    int m_paints = 0;
    qreal m_lastPaintMs = 0.0;
    qreal m_lastDrawMs = 0.0; // renderLive trace drawing, last tick
    // Mapping the phosphor was drawn with; any change clears it next frame.
    double m_phSpan = 0.0, m_phCenter = 0.0;
    bool m_phSinc = false;
    bool m_phValid[2] = {false, false};
    double m_phXinc[2] = {0.0, 0.0};
    double m_phXorig[2] = {0.0, 0.0};
    double m_phYinc[2] = {0.0, 0.0};
    int m_phStride[2] = {1, 1};
    double m_phVdiv[2] = {0.0, 0.0};
    double m_phOffs[2] = {0.0, 0.0};
    // Ground tag drag.
    int m_tagDrag = -1;           // channel being dragged, -1 none
    double m_tagOffs[2] = {0.0, 0.0}; // previewed offset, V
    QElapsedTimer m_tagLock[2];   // preview held 1.5 s after the last move
    bool m_tagHeld[2] = {false, false};
    bool m_tagDirty = false;
    // Trigger level tag: same preview rules as the ground tags.
    int m_trigSrc = 1;
    double m_trigLevel = 0.0;     // last level from the meter, V
    bool m_trigKnown = false;
    bool m_trigDrag = false;
    bool m_trigHeld = false;
    double m_trigPrev = 0.0;      // previewed level, V
    QElapsedTimer m_trigLock;
    bool m_trigDirty = false;
    // Cursors.
    int m_curMode = 0;
    int m_curCh = 0;
    double m_curT[2] = {0.0, 0.0}; // s, trigger-relative
    double m_curV[2] = {0.0, 0.0}; // V on m_curCh
    int m_curDrag = -1;            // 0/1 time A/B, 2/3 level A/B
    int cursorAt(const QPoint &pos) const;
    void drawCursors(QPainter &p, const Grid &g);
    void drawReadout(QPainter &p, const Grid &g); // crosshair t / V box
    void emitCursors();
};
