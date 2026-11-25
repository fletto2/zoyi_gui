#include "waveview.h"

#include "controldock.h" // fmt::seconds / fmt::volts
#include "theme.h"

#include <QFontMetrics>
#include <QAction>
#include <QActionGroup>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QSettings>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace {

// Plot margins: left CH1/CH2 markers, right trigger marker + voltage,
// top time scale + trigger T triangle.
constexpr int kLeft = 26, kRight = 58, kTop = 30, kBottom = 14;

QString fmtAxisTime(double t, double div)
{
    double factor = 1e-9;
    QString unit = QStringLiteral("ns");
    if (div >= 1.0 - 1e-9) {
        factor = 1.0;
        unit = QStringLiteral("s");
    } else if (div >= 1e-3 * (1.0 - 1e-6)) {
        factor = 1e-3;
        unit = QStringLiteral("ms");
    } else if (div >= 1e-6 * (1.0 - 1e-6)) {
        factor = 1e-6;
        unit = QString(QChar(0x00B5)) + QLatin1Char('s');
    }
    double val = t / factor;
    if (std::abs(val) < 1e-4)
        val = 0.0;
    const int dec = std::abs(val - std::round(val)) < 1e-3             ? 0
                    : std::abs(val * 10.0 - std::round(val * 10.0)) < 1e-3 ? 1
                                                                           : 2;
    return QStringLiteral("%1 %2").arg(QString::number(val, 'f', dec), unit);
}

const QColor kChColor[2] = {QColor(0xfa, 0xcc, 0x15), QColor(0x38, 0xbd, 0xf8)};

constexpr double kPi = 3.14159265358979323846;

// Phosphor time constant: the layer retains this fraction per 16.67 ms of
// WALL time (a 60 f/s CRT), independent of the frame rate the scope
// delivers.
constexpr double kRetainPerTick = 0.40;
constexpr double kTickMs = 1000.0 / 60.0;

// Lanczos-windowed sinc kernel from a precomputed table: one linear
// interpolation per (pixel, sample) term instead of two trig calls.
// a = 10 lobes; beyond that the window is exactly zero.
struct LanczosKernel {
    static constexpr int kLobes = 10;
    static constexpr int kRes = 512; // table steps per unit distance

    QVector<double> tab;
    LanczosKernel() : tab(kLobes * kRes + 2, 0.0)
    {
        tab[0] = 1.0;
        for (int i = 1; i < tab.size(); ++i) {
            const double x = double(i) / kRes;
            const double px = kPi * x;
            tab[i] = (std::sin(px) / px) * (std::sin(px / kLobes) / (px / kLobes));
        }
    }
    double at(double d) const
    {
        if (d >= kLobes)
            return 0.0;
        const double u = d * kRes;
        const int i = int(u);
        return tab[i] + (tab[i + 1] - tab[i]) * (u - i);
    }
};

// A thick line as one filled outline, for an antialiased fillPath. Wide AA
// pens on a 1000+ point polyline cost about 210 ms per frame on the Mac:
// the stroker's round joins make a huge self-overlapping outline that the
// rasterizer then fills (Mac port, 2026-11-20, `sample` profile, zoomed in
// with sinc). Here the line is cut into columns `step` wide (one device
// pixel), each column takes the min/max y of the piecewise linear line
// within hw of it on either side, and the outline runs hw above the min
// and hw below the max. That is the line drawn with a square brush 2*hw
// wide, close to a round pen at these widths.
QPolygonF thickLineBand(const QPolygonF &poly, double hw, double step)
{
    QPolygonF out;
    if (poly.size() < 2 || step <= 0.0)
        return out;
    double xMin = poly.first().x(), xMax = xMin;
    for (const QPointF &pt : poly) {
        xMin = qMin(xMin, pt.x());
        xMax = qMax(xMax, pt.x());
    }
    const int cols = qMax(1, int(std::ceil((xMax - xMin) / step)));
    QVector<double> lo(cols, 1e300), hi(cols, -1e300);
    for (int k = 1; k < poly.size(); ++k) {
        QPointF a = poly[k - 1], b = poly[k];
        if (a.x() > b.x())
            std::swap(a, b);
        const double xa = (a.x() - xMin) / step, xb = (b.x() - xMin) / step;
        const int c0 = qBound(0, int(std::floor(xa)), cols - 1);
        const int c1 = qBound(0, int(std::floor(xb)), cols - 1);
        const double dx = xb - xa;
        for (int c = c0; c <= c1; ++c) {
            double ya = a.y(), yb = b.y();
            if (dx > 1e-9) {
                const double l = qMax(xa, double(c));
                const double r = qMin(xb, double(c + 1));
                ya = a.y() + (b.y() - a.y()) * (l - xa) / dx;
                yb = a.y() + (b.y() - a.y()) * (r - xa) / dx;
            }
            lo[c] = qMin(lo[c], qMin(ya, yb));
            hi[c] = qMax(hi[c], qMax(ya, yb));
        }
    }
    // Dilate in x by hw: a running window over the neighbor columns.
    const int r = int(std::ceil(hw / step));
    QVector<double> dlo(cols), dhi(cols);
    for (int c = 0; c < cols; ++c) {
        double l = 1e300, h = -1e300;
        for (int j = qMax(0, c - r); j <= qMin(cols - 1, c + r); ++j) {
            l = qMin(l, lo[j]);
            h = qMax(h, hi[j]);
        }
        dlo[c] = l - hw;
        dhi[c] = h + hw;
    }
    out.reserve(2 * cols + 4);
    out.append(QPointF(xMin - hw, dlo[0]));
    for (int c = 0; c < cols; ++c)
        out.append(QPointF(xMin + (c + 0.5) * step, dlo[c]));
    out.append(QPointF(xMax + hw, dlo[cols - 1]));
    out.append(QPointF(xMax + hw, dhi[cols - 1]));
    for (int c = cols - 1; c >= 0; --c)
        out.append(QPointF(xMin + (c + 0.5) * step, dhi[c]));
    out.append(QPointF(xMin - hw, dhi[0]));
    return out;
}

} // namespace

WaveView::WaveView(QWidget *parent)
    : QWidget(parent)
    , m_render(this)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    // Trace rendering: sin(x)/x reconstruction by default (what the scope
    // itself shows), plain polyline as an option; phosphor-style intensity
    // fade on by default. All persisted.
    QSettings s;
    m_renderSinc = s.value(QStringLiteral("trace/sinc"), true).toBool();
    m_persist = s.value(QStringLiteral("trace/persist"), true).toBool();
    m_infinite = s.value(QStringLiteral("trace/infinite"), false).toBool();
    m_avgN = qBound(1, s.value(QStringLiteral("trace/average"), 1).toInt(), 64);
    m_xy = s.value(QStringLiteral("trace/xy"), false).toBool();
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        QMenu m(this);
        QAction *sinc = m.addAction(tr("Sinc interpolation (sin x/x)"));
        QAction *line = m.addAction(tr("Line"));
        m.addSeparator();
        QAction *fade = m.addAction(tr("Intensity fade (persistence)"));
        QAction *inf = m.addAction(tr("Infinite persistence"));
        QAction *clr = m.addAction(tr("Clear persistence"));
        m.addSeparator();
        QMenu *avg = m.addMenu(tr("Average (PC)"));
        QList<QAction *> avgActs;
        auto *agrp = new QActionGroup(&m);
        for (int n : {1, 2, 4, 8, 16, 32, 64}) {
            QAction *a = avg->addAction(n == 1 ? tr("Off") : tr("%1 frames").arg(n));
            a->setCheckable(true);
            a->setChecked(n == m_avgN);
            a->setData(n);
            agrp->addAction(a);
            avgActs << a;
        }
        QAction *xy = m.addAction(tr("XY (CH1 = X, CH2 = Y)"));
        m.addSeparator();
        QAction *clean = m.addAction(tr("Clean view (F11)"));
        sinc->setCheckable(true);
        line->setCheckable(true);
        fade->setCheckable(true);
        inf->setCheckable(true);
        xy->setCheckable(true);
        sinc->setChecked(m_renderSinc);
        line->setChecked(!m_renderSinc);
        fade->setChecked(m_persist);
        inf->setChecked(m_infinite);
        xy->setChecked(m_xy);
        auto *grp = new QActionGroup(&m);
        grp->addAction(sinc);
        grp->addAction(line);
        QAction *picked = m.exec(mapToGlobal(pos));
        if (!picked)
            return;
        QSettings s3;
        if (picked == clean) {
            emit cleanViewToggled();
            return;
        } else if (picked == clr) {
            if (!m_phosphor.isNull())
                m_phosphor.fill(Qt::transparent);
        } else if (picked == inf) {
            m_infinite = inf->isChecked();
            if (m_infinite)
                m_persist = true;
            s3.setValue(QStringLiteral("trace/infinite"), m_infinite);
            s3.setValue(QStringLiteral("trace/persist"), m_persist);
        } else if (picked == xy) {
            m_xy = xy->isChecked();
            s3.setValue(QStringLiteral("trace/xy"), m_xy);
            if (!m_phosphor.isNull())
                m_phosphor.fill(Qt::transparent);
        } else if (avgActs.contains(picked)) {
            m_avgN = picked->data().toInt();
            for (auto &a : m_avgAcc)
                a.clear();
            s3.setValue(QStringLiteral("trace/average"), m_avgN);
        } else if (picked == fade) {
            m_persist = fade->isChecked();
            if (!m_persist)
                m_infinite = false;
            QSettings s2;
            s2.setValue(QStringLiteral("trace/persist"), m_persist);
            s2.setValue(QStringLiteral("trace/infinite"), m_infinite);
            if (!m_persist && !m_phosphor.isNull())
                m_phosphor.fill(Qt::transparent);
        } else {
            m_renderSinc = picked == sinc;
            QSettings s2;
            s2.setValue(QStringLiteral("trace/sinc"), m_renderSinc);
        }
        m_liveDirty = true;
        update();
    });

    // The render tick drives everything visible: phosphor decay, new-frame
    // draws and repaints, all at ~60 Hz wall clock.
    m_render.setInterval(16);
    connect(&m_render, &QTimer::timeout, this, &WaveView::onRenderTick);
    m_render.start();
    m_decayClock.start();
    m_statClock.start();
    m_emitClock.start();
}

void WaveView::setRecord(const WaveFrame &rec)
{
    m_rec = rec;
    m_haveRec = true;
    if (!rec.running) {
        // Deep record: the stopped view shows it from now on. The view
        // window stays where it was (clamped into the record).
        m_frame = rec;
        m_deep = true;
        m_devLock = false;
        if (!m_have) {
            m_have = true;
            viewToRecord();
        } else {
            clampView();
        }
        m_newFrame = true;
    }
    emit recordChanged();
}

// : exponential running average, weight 1/n, like a scope's average
// mode. The accumulator restarts when the sampling changes (count, xinc,
// xorig, timebase), so a knob turn shows at once.
void WaveView::averageFrame(WaveFrame &f)
{
    for (int c = 0; c < 2; ++c) {
        ChannelFrame &ch = f.ch[c];
        QVector<float> &acc = m_avgAcc[c];
        if (!ch.valid || ch.v.isEmpty() || ch.isPeak()) {
            acc.clear();
            continue;
        }
        double *key = m_avgKey[c];
        const bool same = acc.size() == ch.v.size() && key[0] == ch.xinc
                          && std::abs(key[1] - ch.xorig) <= ch.xinc * 1.5 && key[2] == f.scal;
        if (!same) {
            acc = ch.v;
            key[0] = ch.xinc;
            key[1] = ch.xorig;
            key[2] = f.scal;
            continue;
        }
        const float k = 1.0f / float(m_avgN);
        float *a = acc.data();
        const float *v = ch.v.constData();
        for (int i = 0; i < acc.size(); ++i)
            a[i] += (v[i] - a[i]) * k;
        ch.v = acc;
    }
}

// : XY. CH1 drives X, CH2 drives Y, both in their own screen
// divisions ((v + offs) / vdiv), 8 divisions square in the grid center.
void WaveView::drawXY(QPainter &p, const Grid &g)
{
    const ChannelFrame &x = m_frame.ch[0], &y = m_frame.ch[1];
    if (!x.valid || !y.valid || x.v.isEmpty() || y.v.isEmpty() || x.vdiv <= 0.0
        || y.vdiv <= 0.0) {
        p.setPen(QColor(0x94, 0xa3, 0xb8));
        p.drawText(QRect(g.x, g.y, g.w, g.h), Qt::AlignCenter, tr("XY needs CH1 and CH2 on"));
        return;
    }
    const double div = g.h / 8.0;
    const double cx = g.x + g.w / 2.0, cy = g.y + g.h / 2.0;
    const int n = qMin(x.v.size(), y.v.size());
    QPainterPath path;
    for (int i = 0; i < n; ++i) {
        const double px = cx + (x.v.at(i) + x.offs) / x.vdiv * div;
        const double py = cy - (y.v.at(i) + y.offs) / y.vdiv * div;
        if (i == 0)
            path.moveTo(px, py);
        else
            path.lineTo(px, py);
    }
    p.setClipRect(QRect(g.x, g.y, g.w, g.h));
    QColor c(0x4a, 0xde, 0x80);
    p.setPen(QPen(c, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setClipping(false);
}

void WaveView::setFrame(const WaveFrame &f)
{
    // Stopped with the deep record: stopped frames still in flight carry a
    // window slice only, the deep record is better.
    if (m_deep && !f.running)
        return;
    if (f.running && (m_deep || (m_have && !m_frame.running)))
        m_runShift = 0.0; // RUN starts on the device screen (pump does the same)
    if (f.running)
        m_deep = false;
    // Hold the last good frame while the pump settles a wheel step (a
    // short wait beats a blank or half trace). Guard: after
    // 2 s of unsettled frames take them anyway.
    if (!f.settled && m_have) {
        if (!m_holdClock.isValid())
            m_holdClock.start();
        if (m_holdClock.elapsed() < 2000)
            return;
    }
    m_holdClock.invalidate();
    WaveFrame use = f; // : PC-side running average of running frames
    if (m_avgN > 1 && f.running && !f.roll)
        averageFrame(use);
    if (f.roll && f.running && f.snapMs > 0) {
        // Smooth roll: the display delay must cover the time from one
        // SNAP until the next frame arrives, or the right edge runs dry.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_rollPrevSnap > 0) {
            const double need = double(now - m_rollPrevSnap) + 10.0;
            m_rollTarget = need > m_rollTarget ? need
                                               : m_rollTarget + 0.05 * (need - m_rollTarget);
            m_rollTarget = qBound(30.0, m_rollTarget, 380.0);
        } else {
            m_rollD = m_rollTarget;
        }
        m_rollPrevSnap = f.snapMs;
        histFeed(f);
    } else {
        m_rollPrevSnap = 0;
        m_rollShift = 0.0;
        m_hist = RollHist{};
    }
    m_frame = use;
    if (!m_have) {
        m_have = true;
        if (f.running)
            applyDeviceWindow();
        else
            viewToRecord();
    } else if (f.running && !m_devLock) {
        applyDeviceWindow();
    }
    m_newFrame = true; // the render tick draws it
}

WaveView::Grid WaveView::grid() const
{
    return {kLeft, kTop, width() - kLeft - kRight, height() - kTop - kBottom};
}

int WaveView::firstChannel() const
{
    for (int i = 0; i < 2; ++i)
        if (m_frame.ch[i].valid && m_frame.ch[i].count > 0)
            return i;
    return -1;
}

double WaveView::dataSpan() const
{
    const int c = firstChannel();
    if (c < 0)
        return 0.0;
    const int n = m_frame.ch[c].v.size();
    return n > 1 ? double(n - 1) * m_frame.ch[c].xinc : m_frame.ch[c].xinc;
}

double WaveView::dataStart() const
{
    const int c = firstChannel();
    if (c < 0)
        return 0.0;
    return m_frame.ch[c].xorig;
}

void WaveView::applyDeviceWindow()
{
    // The device screen window: 10*scal wide, trigger at POS% (screen
    // center time == TIM:OFFS, measured on the meter).
    if (m_frame.scal > 0.0) {
        m_span = 10.0 * m_frame.scal;
        m_center = devCenter() + m_runShift;
        clampView();
    }
}

double WaveView::devCenter() const
{
    return (0.5 - m_frame.pos / 100.0) * m_span;
}

void WaveView::emitShift(bool final)
{
    if (final || !m_emitClock.isValid() || m_emitClock.elapsed() >= 80) {
        emit viewShiftRequested(m_runShift);
        m_emitClock.restart();
    }
    // A skipped move is covered by the next one or by the final emit on
    // release.
}

void WaveView::viewToRecord()
{
    // Test pass 3 bug (5): an app started while the meter is stopped fitted
    // the whole 64K record into the plot (and drew it at 4-5 f/s). Show the
    // meter's own screen window instead, as a Space-stop from a running app
    // does. The whole record stays reachable by zooming out.
    if (m_frame.scal > 0.0) {
        m_runShift = 0.0;
        applyDeviceWindow();
        return;
    }
    if (m_frame.recT1 > m_frame.recT0) {
        m_span = m_frame.recT1 - m_frame.recT0;
        m_center = (m_frame.recT0 + m_frame.recT1) / 2.0;
    } else {
        m_span = dataSpan();
        m_center = dataStart() + m_span / 2.0;
    }
    clampView();
}

void WaveView::clampView()
{
    if (m_span <= 0.0)
        return;
    if (m_have && !m_frame.running && m_frame.recT1 > m_frame.recT0) {
        // Stopped: free view inside the stored record.
        const double t0 = m_frame.recT0, t1 = m_frame.recT1;
        if (m_span > t1 - t0)
            m_span = t1 - t0;
        const double lo = t0 + m_span / 2.0, hi = t1 - m_span / 2.0;
        m_center = lo > hi ? (t0 + t1) / 2.0 : qBound(lo, m_center, hi);
    } else if (m_have && m_frame.running && m_frame.recT1 > m_frame.recT0) {
        // Running: the view may sit anywhere inside the record (pan).
        const double t0 = m_frame.recT0, t1 = m_frame.recT1;
        const double lo = t0 + m_span / 2.0, hi = t1 - m_span / 2.0;
        m_center = lo > hi ? (t0 + t1) / 2.0 : qBound(lo, m_center, hi);
    } else {
        // Running, record not known yet: the window contains the trigger.
        m_center = qBound(-m_span / 2.0, m_center, m_span / 2.0);
    }
}

QList<double> WaveView::spanLadder() const
{
    // Absolute 1-2-5 spans matching TIM:SCAL's 10 ns .. 20 s range, that is
    // 100 ns .. 200 s. the loop ran on to 500 s (50 s/div).
    QList<double> out;
    for (int e = -7; e <= 2; ++e) {
        const double base = std::pow(10.0, e);
        out << base << 2.0 * base;
        if (e < 2)
            out << 5.0 * base;
    }
    return out;
}

bool WaveView::ensurePhosphor()
{
    const qreal dpr = devicePixelRatioF();
    if (width() <= 0 || height() <= 0)
        return false;
    const QSize phys(qRound(width() * dpr), qRound(height() * dpr));
    if (m_phosphor.isNull() || !qFuzzyCompare(double(m_phosDpr), double(dpr))
        || m_phosphor.size() != phys) {
        m_phosphor = QPixmap(phys);
        m_phosphor.setDevicePixelRatio(dpr);
        m_phosphor.fill(Qt::transparent);
        m_live = QPixmap(phys);
        m_live.setDevicePixelRatio(dpr);
        m_live.fill(Qt::transparent);
        m_phosDpr = dpr;
    }
    return true;
}

bool WaveView::phosKeyMatches() const
{
    if (m_span != m_phSpan || m_center != m_phCenter || m_renderSinc != m_phSinc)
        return false;
    for (int c = 0; c < 2; ++c) {
        const ChannelFrame &cf = m_frame.ch[c];
        const bool v = cf.valid && !cf.v.isEmpty() && cf.xinc > 0.0 && cf.yinc > 0.0;
        if (v != m_phValid[c])
            return false;
        if (v && (chanVdiv(c) != m_phVdiv[c] || chanOffs(c) != m_phOffs[c]))
            return false;
        // Keyed on the meter's time base (xorig - tshift): the per-frame
        // sub-sample trigger shift must not clear the ghosts.
        if (v && (cf.xinc != m_phXinc[c] || cf.xorig - cf.tshift != m_phXorig[c]
                  || cf.yinc != m_phYinc[c] || cf.stride != m_phStride[c]))
            return false;
    }
    return true;
}

void WaveView::phosKeySave()
{
    m_phSpan = m_span;
    m_phCenter = m_center;
    m_phSinc = m_renderSinc;
    for (int c = 0; c < 2; ++c) {
        const ChannelFrame &cf = m_frame.ch[c];
        const bool v = cf.valid && !cf.v.isEmpty() && cf.xinc > 0.0 && cf.yinc > 0.0;
        m_phValid[c] = v;
        m_phXinc[c] = cf.xinc;
        m_phXorig[c] = cf.xorig - cf.tshift;
        m_phYinc[c] = cf.yinc;
        m_phStride[c] = cf.stride;
        m_phVdiv[c] = chanVdiv(c);
        m_phOffs[c] = chanOffs(c);
    }
}

double WaveView::chanVdiv(int c) const
{
    const ChannelFrame &cf = m_frame.ch[c];
    return cf.vdiv > 0.0 ? cf.vdiv : cf.yinc * kCodesPerDiv;
}

double WaveView::chanOffs(int c) const
{
    if (m_tagDrag == c || m_tagHeld[c])
        return m_tagOffs[c];
    return m_frame.ch[c].offs;
}

bool WaveView::channelMap(int c, double &vdiv, double &offs) const
{
    if (!m_have || c < 0 || c > 1 || !m_frame.ch[c].valid)
        return false;
    vdiv = chanVdiv(c);
    offs = chanOffs(c);
    return vdiv > 0.0;
}

double WaveView::tagY(int c, const Grid &g) const
{
    const double vdiv = chanVdiv(c);
    const double cy = g.y + g.h / 2.0;
    const double y = vdiv > 0.0 ? cy - (chanOffs(c) / vdiv) * (g.h / 8.0) : cy;
    return qBound(double(g.y), y, double(g.y + g.h));
}

int WaveView::tagAt(const QPoint &pos) const
{
    const Grid g = grid();
    if (pos.x() < 0 || pos.x() > g.x + 2)
        return -1;
    // CH2 is drawn last (on top), so it wins a tie.
    for (int c = 1; c >= 0; --c) {
        if (m_frame.ch[c].valid && std::abs(pos.y() - tagY(c, g)) <= 10.0)
            return c;
    }
    return -1;
}

void WaveView::setTrigger(int source, double level, bool known)
{
    m_trigSrc = qBound(1, source, 2);
    m_trigLevel = level;
    m_trigKnown = known && std::isfinite(level);
    update();
}

double WaveView::trigLevel() const
{
    return m_trigDrag || m_trigHeld ? m_trigPrev : m_trigLevel;
}

bool WaveView::trigY(const Grid &g, double &y) const
{
    const int c = m_trigSrc - 1;
    const double vdiv = chanVdiv(c);
    if (!m_trigKnown || !m_frame.ch[c].valid || vdiv <= 0.0)
        return false;
    // Screen divisions of a voltage = (v + offs) / vdiv (ChannelFrame).
    y = g.y + g.h / 2.0 - ((trigLevel() + chanOffs(c)) / vdiv) * (g.h / 8.0);
    return true;
}

bool WaveView::trigTagAt(const QPoint &pos) const
{
    const Grid g = grid();
    double y = 0.0;
    if (pos.x() < g.x + g.w - 2 || pos.x() > width() || !trigY(g, y))
        return false;
    const double ty = qBound(double(g.y), y, double(g.y + g.h));
    return pos.y() >= ty - 10.0 && pos.y() <= ty + 20.0;
}

void WaveView::onRenderTick()
{
    if (m_devLock && m_devLockSince.elapsed() > 1500)
        m_devLock = false; // device window takes over again
    bool repaint = false; // Repaint only when something changed
    if (m_trigHeld && (m_trigLock.elapsed() > 1500
                       || std::abs(m_trigLevel - m_trigPrev)
                              < chanVdiv(m_trigSrc - 1) / 40.0)) {
        m_trigHeld = false; // the meter's level caught up (or gave up)
        repaint = true;
    }
    for (int c = 0; c < 2; ++c) {
        // Ground tag preview ends once a frame shows the dragged offset.
        if (!m_tagHeld[c])
            continue;
        const double vdiv = chanVdiv(c);
        if (m_tagLock[c].elapsed() > 1500
            || (vdiv > 0.0 && std::abs(m_frame.ch[c].offs - m_tagOffs[c]) < vdiv / 40.0)) {
            m_tagHeld[c] = false;
            m_liveDirty = true;
        }
    }

    // Decay first (a fresh trace keeps its full brightness for one tick).
    if (m_have && m_persist && m_infinite) {
        m_decayClock.restart(); // : infinite persistence, no decay
    } else if (m_have && m_persist && !m_phosphor.isNull()) {
        const qint64 dt = m_decayClock.restart();
        if (dt > 1000) {
            m_phosphor.fill(Qt::transparent); // long stall: no stale ghosts
        } else if (dt > 0) {
            const double retain = std::pow(kRetainPerTick, double(dt) / kTickMs);
            const int alpha = qRound(255.0 * (1.0 - retain));
            if (alpha > 0) {
                QPainter pp(&m_phosphor);
                pp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                pp.fillRect(rect(), QColor(0, 0, 0, alpha));
            }
        }
    } else {
        m_decayClock.restart(); // keep the clock fresh while not decaying
    }

    if (rollActive()) {
        // Slew the delay (a step would jerk the trace), faster up than down.
        const double d = m_rollTarget - m_rollD;
        m_rollD += qBound(-0.5, d, 2.0);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_rollShift = (double(now - m_frame.snapMs) - m_rollD) / 1000.0;
        m_liveDirty = true; // redraw every tick at the new shift
    }
    if (m_newFrame || m_liveDirty) {
        const bool fresh = m_newFrame;
        m_newFrame = false;
        m_liveDirty = false;
        renderLive(fresh);
        if (fresh)
            m_lastFresh.restart();
        repaint = true;
    }
    // The ghosts keep fading for a while after the last new frame (40% per
    // tick, gone to the eye in about 300 ms). A slow keepalive repaint
    // covers any overlay change that did not ask for a repaint itself.
    if (m_persist && m_lastFresh.isValid() && m_lastFresh.elapsed() < 400)
        repaint = true;
    if (!m_lastUpdate.isValid() || m_lastUpdate.elapsed() >= 250)
        repaint = true;
    if (repaint) {
        m_lastUpdate.restart();
        update();
    }
}

void WaveView::renderLive(bool fresh)
{
    if (!ensurePhosphor())
        return;
    // The live layer still shows the previous frame drawn with the saved
    // mapping. On a fresh frame with the same mapping it becomes a ghost.
    const bool same = phosKeyMatches();
    if (!m_persist || !same || rollActive()) {
        // Mapping changed / fade off. Roll: the trace moves every tick, a
        // ghost would stand still and smear, so no ghosts.
        m_phosphor.fill(Qt::transparent);
    } else if (fresh) {
        QPainter pp(&m_phosphor);
        pp.drawPixmap(0, 0, m_live);
    }
    phosKeySave();
    m_live.fill(Qt::transparent);
    if (firstChannel() < 0)
        return; // channels off: nothing live, ghosts already cleared above
    QElapsedTimer drawClock;
    drawClock.start();
    QPainter lp(&m_live);
    lp.setRenderHint(QPainter::Antialiasing, true);
    if (m_xy)
        drawXY(lp, grid());
    else
        drawTraces(lp, grid());
    lp.end();
    m_lastDrawMs = drawClock.nsecsElapsed() / 1e6;
}

void WaveView::drawTraces(QPainter &p, const Grid &g)
{
    if (g.w <= 0 || g.h <= 0 || m_span <= 0.0)
        return;
    static const LanczosKernel kernel;
    const int cy = g.y + g.h / 2;
    const double t0 = m_center - m_span / 2.0, t1 = m_center + m_span / 2.0;

    // Three render paths:
    //  - sparse RAW samples (stride 1) + sinc mode: Lanczos-windowed
    //    sin(x)/x reconstruction, one point per pixel column, evaluated
    //    ONLY between the first and last real sample. Beyond the data the
    //    truncated kernel decays toward 0, which drew a fake line back to
    //    0 at the right edge.
    //  - dense samples (< 0.8 px apart, raw or decimated): per-pixel-column
    //    min/max envelope, so peaks never vanish - regardless of sinc mode.
    //  - sparse decimated (stride > 1) or Line mode: plain polyline. Peak-picking
    //    decimation is firmware work (batch 11 item 2).
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setClipRect(QRect(g.x, g.y, g.w, g.h));
    const double rollShift = rollActive() ? m_rollShift : 0.0;
    for (int c = 0; c < 2; ++c) {
        // A copy (the sample vector is shared, not copied) so the roll
        // shift can move xorig for this draw only.
        ChannelFrame cf = m_frame.ch[c];
        if (!cf.valid || cf.v.isEmpty() || cf.xinc <= 0.0 || cf.yinc <= 0.0)
            continue;
        cf.xorig -= rollShift;
        const double vdiv = chanVdiv(c);
        const double off = chanOffs(c); // screen divs = (v + off) / vdiv
        const int n = cf.v.size();
        const double pxPerSample = (cf.xinc / m_span) * g.w;
        const int i0 = qMax(0, int(std::ceil((t0 - cf.xorig) / cf.xinc)));
        const int i1 = qMin(n - 1, int(std::floor((t1 - cf.xorig) / cf.xinc)));
        // The line path takes one sample past each screen edge, so the
        // segment into the edge is drawn (clipped), and a zoom between two
        // samples still shows the line. j0..j1 is that wider range.
        const int j0 = qMax(0, int(std::floor((t0 - cf.xorig) / cf.xinc)));
        const int j1 = qMin(n - 1, int(std::ceil((t1 - cf.xorig) / cf.xinc)));
        if (j1 < j0)
            continue;

        QPolygonF poly;
        // Envelope columns as separate lines, drawn with drawLines() and no
        // antialiasing. As one QPainterPath with wide AA pens it took 1.6 s
        // per frame at about 1400 columns (test run 6 "traces" readout).
        QVector<QLineF> envelope;
        if (cf.isPeak()) {
            // 0.18.0 WAV:POIN:MODE PEAK: each bucket carries exact [min, max]
            // volts over its stride window. Draw directly as a per-pixel-column
            // envelope so narrow pulses and high-freq carriers never alias away.
            if (pxPerSample < 1.0) {
                int col = -1;
                double yMin = 0.0, yMax = 0.0;
                double prevMin = 0.0, prevMax = 0.0;
                bool havePrev = false;
                auto seal = [&](int colIdx) {
                    if (havePrev) {
                        if (prevMax < yMin)
                            yMin = prevMax;
                        else if (prevMin > yMax)
                            yMax = prevMin;
                    }
                    const double x = g.x + colIdx + 0.5;
                    envelope.append(QLineF(x, yMin, x, yMax));
                    prevMin = yMin;
                    prevMax = yMax;
                    havePrev = true;
                };
                for (int i = i0; i <= i1; ++i) {
                    const double x = g.x + g.w * (cf.xorig + double(i) * cf.xinc - t0)
                                             / m_span;
                    const int px = int(std::floor(x - g.x));
                    if (px < 0)
                        continue;
                    if (px > g.w)
                        break;
                    // Screen y is inverted: vHi (max V) -> smaller y (top).
                    const double yTop = cy - ((cf.vHi[i] + off) / vdiv) * (g.h / 8.0);
                    const double yBot = cy - ((cf.v[i] + off) / vdiv) * (g.h / 8.0);
                    if (px != col) {
                        if (col >= 0)
                            seal(col);
                        col = px;
                        yMin = yTop;
                        yMax = yBot;
                    } else {
                        yMin = qMin(yMin, yTop);
                        yMax = qMax(yMax, yBot);
                    }
                }
                if (col >= 0)
                    seal(col);
            } else {
                // Wide window with >= 1 px per bucket: interpolate upper/lower
                // envelope linearly across columns so the band stays continuous.
                for (int i = j0; i < j1; ++i) {
                    const double xa = g.w * (cf.xorig + double(i) * cf.xinc - t0) / m_span;
                    const double xb = g.w * (cf.xorig + double(i + 1) * cf.xinc - t0) / m_span;
                    const double ta = cy - ((cf.vHi[i] + off) / vdiv) * (g.h / 8.0);
                    const double ba = cy - ((cf.v[i] + off) / vdiv) * (g.h / 8.0);
                    const double tb = cy - ((cf.vHi[i + 1] + off) / vdiv) * (g.h / 8.0);
                    const double bb = cy - ((cf.v[i + 1] + off) / vdiv) * (g.h / 8.0);
                    const int c0 = qMax(0, int(std::floor(xa)));
                    const int c1 = qMin(g.w, int(std::ceil(xb)));
                    const double dx = xb - xa;
                    for (int px = c0; px <= c1; ++px) {
                        const double u = dx > 1e-9 ? qBound(0.0, (double(px) - xa) / dx, 1.0) : 0.0;
                        const double yTop = ta + (tb - ta) * u;
                        const double yBot = ba + (bb - ba) * u;
                        envelope.append(QLineF(g.x + px + 0.5, yTop, g.x + px + 0.5, yBot));
                    }
                }
            }
        } else if (cf.stride == 1 && m_renderSinc && pxPerSample >= 0.8) {
            // Reconstruct y(t) = sum v[i]*lanczos((t-t_i)/xinc) for pixel
            // columns whose t lies inside the sampled interval.
            const double u0 = (t0 - cf.xorig) / cf.xinc;
            const double du = (t1 - t0) / cf.xinc / g.w; // u per pixel
            for (int px = 0; px <= g.w; ++px) {
                const double u = u0 + du * px;
                if (u < 0.0)
                    continue; // before the first sample
                if (u > n - 1)
                    break;   // past the last sample: no extrapolation
                int k0 = qMax(0, int(std::floor(u)) - LanczosKernel::kLobes);
                int k1 = qMin(n - 1, int(std::ceil(u)) + LanczosKernel::kLobes);
                // Normalized by the weight sum: near the ends of the fetched
                // data the kernel is cut short, and the raw sum then rings
                // and droops (the edge wiggle). The
                // pump also fetches margin beyond the screen, so the visible
                // edges sit inside the data.
                double y = 0.0, wsum = 0.0;
                for (int k = k0; k <= k1; ++k) {
                    const double w = kernel.at(std::abs(u - k));
                    y += double(cf.v[k]) * w;
                    wsum += w;
                }
                if (wsum > 1e-6)
                    y /= wsum;
                poly.append(QPointF(g.x + px, cy - ((y + off) / vdiv) * (g.h / 8.0)));
            }
        } else if (pxPerSample < 0.8) {
            // Any dense data, decimated too: a polyline of thousands of
            // overlapping segments with wide antialiased pens cost the GUI
            // thread so much that render fell to 0.2 f/s at wide zoom (test
            // run, 3576 pts/ch). Columns cap the work at the plot width.
            // Min/max envelope: one vertical segment per pixel column. Each
            // column starts from the previous column's last sample, so
            // neighbors overlap and the trace stays continuous. Without it,
            // at 1-2 samples per column the segments did not touch and a
            // sine fell apart into dashes.
            int col = -1;
            double yMin = 0.0, yMax = 0.0, yLast = 0.0;
            bool haveLast = false;
            auto seal = [&](int colIdx) {
                const double x = g.x + colIdx + 0.5;
                envelope.append(QLineF(x, yMin, x, yMax));
            };
            for (int i = i0; i <= i1; ++i) {
                const double x = g.x + g.w * (cf.xorig + double(i) * cf.xinc - t0)
                                         / m_span;
                const int px = int(std::floor(x - g.x));
                if (px < 0)
                    continue;
                if (px > g.w)
                    break;
                const double y = cy - ((cf.v[i] + off) / vdiv) * (g.h / 8.0);
                if (px != col) {
                    if (col >= 0)
                        seal(col);
                    col = px;
                    yMin = yMax = haveLast ? yLast : y;
                }
                yMin = qMin(yMin, y);
                yMax = qMax(yMax, y);
                yLast = y;
                haveLast = true;
            }
            if (col >= 0)
                seal(col);
        } else {
            poly.reserve(j1 - j0 + 1);
            for (int i = j0; i <= j1; ++i) {
                const double t = cf.xorig + double(i) * cf.xinc;
                const double x = g.x + g.w * (t - t0) / m_span;
                const double y = cy - ((cf.v[i] + off) / vdiv) * (g.h / 8.0);
                poly.append(QPointF(x, y));
            }
        }

        // Glow pass then crisp pass.
        QColor glowC = kChColor[c];
        glowC.setAlpha(0x33);
        // A busy polyline (many tall segments) is slow to stroke with wide
        // AA pens: one draw took 1.8 s on a zoom-in at about 1 px per
        // sample (test run 9). Past a total vertical travel of 20 plot
        // heights, rasterize the line into per-column min/max instead
        // (exact for a piecewise linear trace) and fill columns below.
        if (poly.size() >= 2) {
            double travel = 0.0;
            for (int k = 1; k < poly.size(); ++k)
                travel += std::abs(poly[k].y() - poly[k - 1].y());
            if (travel > 20.0 * g.h) {
                const int cols = g.w + 1;
                QVector<double> lo(cols, 1e300), hi(cols, -1e300);
                for (int k = 1; k < poly.size(); ++k) {
                    QPointF a = poly[k - 1], b = poly[k];
                    if (a.x() > b.x())
                        std::swap(a, b);
                    const double xa = a.x() - g.x, xb = b.x() - g.x;
                    if (xb < 0.0 || xa > g.w)
                        continue;
                    const int c0 = qMax(0, int(std::floor(xa)));
                    const int c1 = qMin(g.w, int(std::floor(xb)));
                    const double dx = xb - xa;
                    for (int c = c0; c <= c1; ++c) {
                        double ya = a.y(), yb = b.y();
                        if (dx > 1e-9) {
                            const double l = qMax(xa, double(c));
                            const double r = qMin(xb, double(c + 1));
                            ya = a.y() + (b.y() - a.y()) * (l - xa) / dx;
                            yb = a.y() + (b.y() - a.y()) * (r - xa) / dx;
                        }
                        lo[c] = qMin(lo[c], qMin(ya, yb));
                        hi[c] = qMax(hi[c], qMax(ya, yb));
                    }
                }
                for (int c = 0; c < cols; ++c) {
                    if (hi[c] >= lo[c]) {
                        const double x = g.x + c + 0.5;
                        envelope.append(QLineF(x, lo[c], x, hi[c]));
                    }
                }
                poly.clear();
            }
        }
        if (poly.size() >= 2) {
            // Filled bands, not wide pens (see thickLineBand). Widths
            // match the glow 3 px and crisp 1.6 px pens.
            const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
            const double step = 1.0 / qMax<qreal>(1.0, dpr);
            p.setPen(Qt::NoPen);
            p.setBrush(glowC);
            p.drawPolygon(thickLineBand(poly, 1.5, step));
            p.setBrush(kChColor[c]);
            p.drawPolygon(thickLineBand(poly, 0.8, step));
            p.setBrush(Qt::NoBrush);
        }
        if (!envelope.isEmpty()) {
            // Columns: AA off. Square caps so a flat column (yMin == yMax)
            // still draws. Glow 3 px wide, then a 2 px core (1 px columns
            // read thin next to the 1.6 px AA polyline).
            // Filled rects, not pens: no stroker at all (drawLines with
            // 2-3 px pens still cost about 20-27 ms per frame, run 9).
            // One filled band per pass, not one fillRect per column: Qt
            // 6.12 on macOS hands every raster fillRect to a thread pool
            // and waits, so about 3000 small rects cost 88-108 ms per frame
            // (Mac port, 2026-11-20, `sample` profile). Each pixel column
            // of the band spans the union of the rects that covered it.
            p.setRenderHint(QPainter::Antialiasing, false);
            const int yLo = g.y - 2, yHi = g.y + g.h + 2;
            const int bx0 = g.x - 2, cols = g.w + 6;
            QVector<int> bt(cols), bb(cols);
            for (int pass = 0; pass < 2; ++pass) {
                bt.fill(INT_MAX);
                bb.fill(INT_MIN);
                // Glow: 3 px wide (xi-1..xi+1), top-1..bot+1. Core: 2 px
                // wide (xi..xi+1), top-1..bot.
                const int xFrom = pass == 0 ? -1 : 0, xTo = 1;
                const int padBot = pass == 0 ? 2 : 1;
                for (const QLineF &l : std::as_const(envelope)) {
                    const int xi = int(std::floor(l.x1()));
                    int top = int(std::floor(qMin(l.y1(), l.y2())));
                    int bot = int(std::ceil(qMax(l.y1(), l.y2())));
                    top = qBound(yLo, top, yHi) - 1;
                    bot = qBound(yLo, bot, yHi) + padBot; // exclusive
                    for (int dx = xFrom; dx <= xTo; ++dx) {
                        const int k = xi + dx - bx0;
                        if (k < 0 || k >= cols)
                            continue;
                        bt[k] = qMin(bt[k], top);
                        bb[k] = qMax(bb[k], bot);
                    }
                }
                // Runs of covered columns -> one polygon each: top edge
                // left to right, bottom edge back.
                QPainterPath band;
                int k = 0;
                while (k < cols) {
                    if (bb[k] <= bt[k]) {
                        ++k;
                        continue;
                    }
                    int e = k;
                    while (e < cols && bb[e] > bt[e])
                        ++e;
                    QPolygonF poly2;
                    poly2.reserve(4 * (e - k));
                    for (int i = k; i < e; ++i) {
                        poly2.append(QPointF(bx0 + i, bt[i]));
                        poly2.append(QPointF(bx0 + i + 1, bt[i]));
                    }
                    for (int i = e - 1; i >= k; --i) {
                        poly2.append(QPointF(bx0 + i + 1, bb[i]));
                        poly2.append(QPointF(bx0 + i, bb[i]));
                    }
                    band.addPolygon(poly2);
                    band.closeSubpath();
                    k = e;
                }
                p.fillPath(band, pass == 0 ? glowC : kChColor[c]);
            }
            p.setRenderHint(QPainter::Antialiasing, true);
        }
        // Real samples as dots once they are far enough apart to matter.
        if (cf.stride == 1 && pxPerSample >= 10.0) {
            p.setPen(Qt::NoPen);
            p.setBrush(kChColor[c]);
            for (int i = i0; i <= i1; ++i) {
                const double t = cf.xorig + double(i) * cf.xinc;
                const double x = g.x + g.w * (t - t0) / m_span;
                const double y = cy - ((cf.v[i] + off) / vdiv) * (g.h / 8.0);
                p.drawEllipse(QPointF(x, y), 1.6, 1.6);
            }
        }
    }
    p.restore();
}

void WaveView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QElapsedTimer paintClock;
    paintClock.start();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0x0b, 0x11, 0x20));

    const Grid g = grid();
    const int cy = g.y + g.h / 2, cx = g.x + g.w / 2;

    // Grid: 10 x 8 divisions, center axes at full strength.
    for (int i = 0; i <= 10; ++i) {
        const double x = g.x + g.w * i / 10.0;
        QColor c(0x24, 0x34, 0x49, i == 5 ? 255 : 204);
        p.setPen(QPen(c, 1.0));
        p.drawLine(QPointF(x, g.y), QPointF(x, g.y + g.h));
    }
    for (int j = 0; j <= 8; ++j) {
        const double y = g.y + g.h * j / 8.0;
        QColor c(0x24, 0x34, 0x49, j == 4 ? 255 : 204);
        p.setPen(QPen(c, 1.0));
        p.drawLine(QPointF(g.x, y), QPointF(g.x + g.w, y));
    }
    // Center ticks.
    p.setPen(QPen(QColor(0x3b, 0x50, 0x6e), 1.0));
    for (int i = 0; i <= 50; ++i) {
        if (i % 5 == 0)
            continue;
        const double x = g.x + g.w * i / 50.0;
        p.drawLine(QPointF(x, cy - 3), QPointF(x, cy + 3));
    }
    for (int j = 0; j <= 40; ++j) {
        if (j % 5 == 0)
            continue;
        const double y = g.y + g.h * j / 40.0;
        p.drawLine(QPointF(cx - 3, y), QPointF(cx + 3, y));
    }

    if (!m_have || firstChannel() < 0) {
        p.setPen(QColor(0x64, 0x74, 0x8b));
        QFont f = p.font();
        f.setPixelSize(14);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, tr("waiting for frames..."));
        m_lastPaintMs = double(paintClock.elapsed());
        return;
    }

    const double t0 = m_center - m_span / 2.0;

    // Traces come from the persistence layer (see class comment): crisp
    // antialiased curves with a 60-fps-wall-clock intensity ghost.
    // Everything else on screen is a non-accumulating overlay.
    if (!m_phosphor.isNull())
        p.drawPixmap(0, 0, m_phosphor);
    if (!m_live.isNull())
        p.drawPixmap(0, 0, m_live);

    QFont tagFont(theme::monoFamily());
    tagFont.setPixelSize(10);
    tagFont.setBold(true);

    // Ground tags at each channel's 0 V (CHAN:OFFS), on the LEFT edge.
    for (int c = 0; c < 2; ++c) {
        if (!m_frame.ch[c].valid)
            continue;
        const double ty = tagY(c, g);
        QPolygonF tag;
        tag.append(QPointF(g.x, ty));
        tag.append(QPointF(g.x - 8, ty - 5));
        tag.append(QPointF(g.x - 8, ty + 5));
        p.setPen(Qt::NoPen);
        p.setBrush(kChColor[c]);
        p.drawPolygon(tag);
        p.setPen(kChColor[c]);
        p.setFont(tagFont);
        p.drawText(QRectF(0, ty - 7, g.x - 10, 14),
                   Qt::AlignVCenter | Qt::AlignRight, QString::number(c + 1));
    }

    // Trigger level: faint dotted line across the grid
    // while on screen, plus orange arrow + "T" on the RIGHT edge with the
    // trigger voltage below it. Draggable: sets trigger level.
    double ly = 0.0;
    if (trigY(g, ly)) {
        if (ly >= g.y && ly <= g.y + g.h) {
            QPen lp(QColor(0xfb, 0x92, 0x3c, 102), 1.0);
            lp.setDashPattern({2.0, 5.0});
            p.setPen(lp);
            p.drawLine(QPointF(g.x, ly + 0.5), QPointF(g.x + g.w, ly + 0.5));
        }
        const double ty = qBound(double(g.y), ly, double(g.y + g.h));
        const QColor tc(0xfb, 0x92, 0x3c);
        QPolygonF tag;
        tag.append(QPointF(g.x + g.w, ty));
        tag.append(QPointF(g.x + g.w + 8, ty - 5));
        tag.append(QPointF(g.x + g.w + 8, ty + 5));
        p.setPen(Qt::NoPen);
        p.setBrush(tc);
        p.drawPolygon(tag);
        p.setPen(tc);
        p.setFont(tagFont);
        p.drawText(QRectF(g.x + g.w + 10, ty - 7, kRight - 12, 14),
                   Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("T"));
        QFont vFont(theme::monoFamily());
        vFont.setPixelSize(9);
        p.setFont(vFont);
        const double vy = qMin(ty + 6.0, double(height() - 12));
        p.drawText(QRectF(g.x + g.w + 4, vy, kRight - 6, 12),
                   Qt::AlignTop | Qt::AlignLeft, fmt::volts(trigLevel()));
    }

    // Top-axis time scale (Item D): Roll = -10*div .. 0 at right (fixed
    // while the trace slides); Normal = trigger-relative t, following zoom
    // and pan. Auto-thins to every 2nd division when labels would crowd.
    if (m_span > 0.0 && g.w > 0) {
        const double div = m_span / 10.0;
        const bool roll = rollActive();
        QFont axFont(theme::monoFamily());
        axFont.setPixelSize(10);
        p.setFont(axFont);
        const QFontMetrics fm(axFont);
        QString labels[11];
        int maxW = 0;
        for (int i = 0; i <= 10; ++i) {
            const double ti = roll ? -double(10 - i) * div : t0 + double(i) * div;
            labels[i] = fmtAxisTime(ti, div);
            maxW = qMax(maxW, fm.horizontalAdvance(labels[i]));
        }
        const int step = (maxW + 10.0 > double(g.w) / 10.0) ? 2 : 1;
        for (int i = 0; i <= 10; i += step) {
            const double x = g.x + g.w * i / 10.0;
            p.setPen(QPen(QColor(0x3b, 0x50, 0x6e), 1.0));
            p.drawLine(QPointF(x, g.y - 3), QPointF(x, g.y));
            const int tw = fm.horizontalAdvance(labels[i]);
            const double lx = qBound(double(g.x), x - tw / 2.0, double(g.x + g.w - tw));
            p.setPen(QColor(0x94, 0xa3, 0xb8));
            p.drawText(QRectF(lx, 2, tw + 2, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       labels[i]);
        }
    }

    // Trigger point (t=0): dotted line + top marker (hidden in roll mode).
    if (!rollActive()) {
        const double tf = (0.0 - t0) / m_span;
        if (tf >= -0.02 && tf <= 1.02) {
            const double tx = g.x + g.w * tf;
            QPen tp(QColor(0xfb, 0x92, 0x3c, 102), 1.0);
            tp.setDashPattern({2.0, 5.0});
            p.setPen(tp);
            p.drawLine(QPointF(tx, g.y), QPointF(tx, g.y + g.h));
            QPolygonF tri;
            tri.append(QPointF(tx, g.y - 2));
            tri.append(QPointF(tx - 6, g.y - 11));
            tri.append(QPointF(tx + 6, g.y - 11));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xfb, 0x92, 0x3c));
            p.drawPolygon(tri);
        }
    }

    drawCursors(p, g);

    // Crosshair with its t / V readout.
    if (m_mouseIn && !m_dragging && m_curDrag < 0 && m_mouse.x() >= g.x
        && m_mouse.x() <= g.x + g.w && m_mouse.y() >= g.y && m_mouse.y() <= g.y + g.h) {
        QPen cp(QColor(0x3b, 0x50, 0x6e), 1.0);
        cp.setDashPattern({2.0, 4.0});
        p.setPen(cp);
        p.drawLine(QPointF(m_mouse.x(), g.y), QPointF(m_mouse.x(), g.y + g.h));
        p.drawLine(QPointF(g.x, m_mouse.y()), QPointF(g.x + g.w, m_mouse.y()));
        drawReadout(p, g);
    }

    // Paint-time + f/s readout, emitted every 2 s.
    m_lastPaintMs = double(paintClock.elapsed());
    ++m_paints;
    if (m_statClock.elapsed() >= 2000) {
        emit renderStats(m_paints * 1000.0 / double(m_statClock.elapsed()),
                         m_lastPaintMs, m_lastDrawMs);
        m_paints = 0;
        m_statClock.restart();
    }
}

void WaveView::wheelEvent(QWheelEvent *event)
{
    const Grid g = grid();
    const double frac = g.w > 0 ? qBound(0.0, (event->position().x() - g.x) / g.w, 1.0)
                                : 0.5;
    wheelZoom(event, frac);
}

void WaveView::wheelZoom(QWheelEvent *event, double frac)
{
    event->accept();
    // A mouse notch is 120 angle units, one step. A Mac trackpad sends a
    // stream of small pixel deltas plus momentum events, and one step per
    // event zoomed far too fast. So: drop the
    // momentum tail, sum the travel, one step per threshold, and at most
    // one step per 150 ms (each running step is a TIM:SCAL round trip).
    if (event->phase() == Qt::ScrollMomentum)
        return;
    if (event->phase() == Qt::ScrollBegin)
        m_wheelAcc = 0.0;
    const bool pixels = !event->pixelDelta().isNull();
    double delta = pixels ? event->pixelDelta().y() : event->angleDelta().y();
    // macOS natural scrolling reports the content direction, so a swipe up
    // arrived as "wheel down" and zoomed out. Flip it
    // back to the finger direction: swipe up = wheel up = zoom in, as a
    // mouse wheel on Windows.
    if (event->inverted())
        delta = -delta;
    const double threshold = pixels ? 40.0 : 120.0;
    if (delta == 0.0)
        return;
    if ((delta > 0) != (m_wheelAcc > 0) && m_wheelAcc != 0.0)
        m_wheelAcc = 0.0; // direction change starts over
    m_wheelAcc += delta;
    if (std::abs(m_wheelAcc) < threshold)
        return;
    if (m_wheelStep.isValid() && m_wheelStep.elapsed() < 150) {
        m_wheelAcc = std::copysign(threshold, m_wheelAcc); // hold, do not pile up
        return;
    }
    const int dir = m_wheelAcc > 0 ? -1 : +1; // up = zoom in
    m_wheelAcc = 0.0;
    m_wheelStep.restart();
    zoomAt(dir, frac);
}

void WaveView::zoomStep(int dir)
{
    zoomAt(dir, 0.5);
}

void WaveView::histFeed(const WaveFrame &f)
{
    const double span = 10.0 * f.scal * kHistScreens;
    if (span <= 0.0)
        return;
    const int N = kHistBuckets;
    if (m_hist.n == 0 || std::abs(m_hist.bucketS * N - span) > 1e-9 * span) {
        // First roll frame or a new timebase: start over.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        m_hist = RollHist{};
        m_hist.n = N;
        m_hist.bucketS = span / N;
        for (int c = 0; c < 2; ++c) {
            m_hist.lo[c].fill(nan, N);
            m_hist.hi[c].fill(nan, N);
        }
        m_histBase = f.snapMs;
    }
    const double bs = m_hist.bucketS;
    const double snapS = double(f.snapMs - m_histBase) / 1000.0;
    if (snapS > m_hist.headS) {
        // Scroll: drop the oldest buckets, open empty ones on the right.
        const double adv = std::ceil((snapS - m_hist.headS) / bs);
        const int k = adv >= N ? N : int(adv);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (int c = 0; c < 2; ++c) {
            for (QVector<float> *vec : {&m_hist.lo[c], &m_hist.hi[c]}) {
                if (k < N)
                    std::move(vec->begin() + k, vec->end(), vec->begin());
                std::fill(vec->end() - k, vec->end(), nan);
            }
        }
        m_hist.headS += adv * bs;
    }
    // The frame replaces the buckets it covers (the newest data is the
    // authority there). Buckets older than its first sample keep their
    // values, so a record the meter restarted after a move leaves the old
    // trace in the strip. The newest sample (t = recT1) is at snapS.
    auto idxOf = [&](double a) {
        return N - 1 - int(std::floor((m_hist.headS - a) / bs));
    };
    for (int c = 0; c < 2; ++c) {
        const ChannelFrame &cf = f.ch[c];
        if (!cf.valid || cf.v.isEmpty() || cf.xinc <= 0.0)
            continue;
        const int n = cf.v.size();
        const double a0 = snapS - (f.recT1 - cf.xorig);
        const int j0 = qMax(0, idxOf(a0));
        const int j1 = qMin(N - 1, idxOf(a0 + double(n - 1) * cf.xinc));
        if (j1 < j0)
            continue;
        float *lo = m_hist.lo[c].data(), *hi = m_hist.hi[c].data();
        for (int j = j0; j <= j1; ++j)
            lo[j] = hi[j] = std::numeric_limits<float>::quiet_NaN();
        const bool peak = cf.isPeak();
        for (int i = 0; i < n; ++i) {
            const int j = idxOf(a0 + double(i) * cf.xinc);
            if (j < 0)
                continue;
            if (j >= N)
                break;
            const float vLo = cf.v[i];
            const float vHi = peak ? cf.vHi[i] : vLo;
            if (std::isnan(lo[j])) {
                lo[j] = vLo;
                hi[j] = vHi;
            } else {
                lo[j] = qMin(lo[j], vLo);
                hi[j] = qMax(hi[j], vHi);
            }
        }
    }
    ++m_hist.gen;
}

bool WaveView::rollHistory(const RollHist *&h, double &dispRightS) const
{
    if (!rollActive() || m_hist.n == 0)
        return false;
    h = &m_hist;
    dispRightS = double(QDateTime::currentMSecsSinceEpoch() - m_histBase) / 1000.0
                 - m_rollD / 1000.0;
    return true;
}

bool WaveView::overviewBox(double &start, double &width, double &trig) const
{
    if (!m_have || m_span <= 0.0)
        return false;
    if (rollActive()) {
        // Roll: the strip holds kHistScreens screens, the plot is the
        // newest one, so the box sits snapped to the right.
        width = 1.0 / kHistScreens;
        start = 1.0 - width;
        trig = -1.0; // no trigger point in roll
        return true;
    }
    {
        // Running and stopped alike: the view in record time. The record
        // picture is fixed, only the box moves.
        const double recSpan = m_frame.recT1 - m_frame.recT0;
        if (recSpan <= 0.0)
            return false;
        width = qMin(1.0, m_span / recSpan);
        start = qBound(0.0, (m_center - m_span / 2.0 - m_frame.recT0) / recSpan, 1.0);
        trig = (0.0 - m_frame.recT0) / recSpan;
    }
    return true;
}

void WaveView::overviewMoveTo(double startFrac, bool final)
{
    if (!m_have || m_span <= 0.0 || rollActive())
        return; // roll: the box stays snapped to the right
    if (m_frame.running) {
        // Pan in the record; the trigger stays where it is.
        const double recSpan = m_frame.recT1 - m_frame.recT0;
        if (recSpan <= 0.0)
            return;
        m_center = m_frame.recT0 + startFrac * recSpan + m_span / 2.0;
        clampView();
        m_runShift = m_center - devCenter();
        emitShift(final);
    } else {
        const double recSpan = m_frame.recT1 - m_frame.recT0;
        if (recSpan <= 0.0)
            return;
        m_center = m_frame.recT0 + startFrac * recSpan + m_span / 2.0;
        clampView();
        if (!m_deep && (final || m_emitClock.elapsed() >= 80)) {
            emit viewWindowChanged(m_center, m_span);
            m_emitClock.restart();
        }
    }
    m_liveDirty = true;
    update();
}

void WaveView::centerTrigger()
{
    if (!m_have || m_span <= 0.0 || rollActive())
        return;
    m_center = 0.0;
    m_runShift = 0.0;
    clampView();
    m_liveDirty = true;
    if (m_frame.running) {
        m_devLock = true;
        m_devLockSince.restart();
        emit centerRequested();
    } else if (!m_deep) {
        emit viewWindowChanged(m_center, m_span);
    }
    update();
}

void WaveView::zoomAt(int dir, double frac)
{
    if (!m_have || m_span <= 0.0)
        return;
    const QList<double> ladder = spanLadder();
    if (ladder.size() < 2)
        return;
    // Nearest ladder step at/below the current span, then one step in/out.
    int idx = 0;
    for (int i = 0; i < ladder.size(); ++i) {
        if (ladder.at(i) <= m_span * (1.0 + 1e-9))
            idx = i;
    }
    idx = qBound(0, idx + dir, ladder.size() - 1);
    const double newSpan = ladder.at(idx);

    if (m_frame.running) {
        // Device timebase step: the meter keeps TRIG:POS (percent of
        // screen) across a SCAL change, not TIM:OFFS (test log
        // 2026-11-19), so the trigger stays at the same screen spot and
        // the center time scales with the span.
        m_span = newSpan;
        m_center = devCenter() + m_runShift; // the pan shift is kept
        clampView();
        m_devLock = true;
        m_devLockSince.restart();
        m_liveDirty = true; // view changed: ghosts clear in renderLive
        emit scaleChangeRequested(newSpan / 10.0);
    } else {
        // Stopped: zoom inside the stored record anchored at the cursor.
        // With the deep record it is all local; without it the pump
        // re-fetches the window with real samples (path).
        const double tAt = m_center - m_span / 2.0 + frac * m_span;
        m_center = tAt + (m_center - tAt) * (newSpan / m_span);
        m_span = newSpan;
        clampView();
        m_liveDirty = true;
        if (!m_deep)
            emit viewWindowChanged(m_center, m_span);
    }
    update();
}

void WaveView::mousePressEvent(QMouseEvent *event)
{
    const int tag = event->button() == Qt::LeftButton && m_have ? tagAt(event->pos()) : -1;
    if (tag >= 0) {
        // Ground tag drag: vertical position of that channel.
        m_tagOffs[tag] = chanOffs(tag);
        m_tagDrag = tag;
        m_tagDirty = false;
        m_emitClock.restart();
        setCursor(Qt::SizeVerCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_have && trigTagAt(event->pos())) {
        // Trigger level drag; ground tags win where they overlap.
        m_trigPrev = trigLevel();
        m_trigDrag = true;
        m_trigDirty = false;
        m_emitClock.restart();
        setCursor(Qt::SizeVerCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const int cur = cursorAt(event->pos());
        if (cur >= 0) {
            m_curDrag = cur;
            setCursor(cur < 2 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            update();
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton && m_have && !rollActive()) {
        m_dragging = true;
        m_dragPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void WaveView::mouseMoveEvent(QMouseEvent *event)
{
    m_mouse = event->pos();
    m_mouseIn = true;
    if (m_tagDrag >= 0) {
        const int c = m_tagDrag;
        const Grid g = grid();
        const double vdiv = chanVdiv(c);
        if (g.h > 0 && vdiv > 0.0) {
            const double cy = g.y + g.h / 2.0;
            double divs = (cy - event->pos().y()) / (g.h / 8.0);
            divs = qBound(-4.0, std::round(divs * 20.0) / 20.0, 4.0); // 1/20 div steps
            const double off = divs * vdiv;
            if (off != m_tagOffs[c]) {
                m_tagOffs[c] = off;
                m_tagDirty = true;
                m_liveDirty = true;
            }
            if (m_tagDirty && m_emitClock.elapsed() >= 80) {
                emit channelOffsetRequested(c + 1, m_tagOffs[c]);
                m_tagDirty = false;
                m_emitClock.restart();
            }
        }
        update();
        event->accept();
        return;
    }
    if (m_trigDrag) {
        const Grid g = grid();
        const int c = m_trigSrc - 1;
        const double vdiv = chanVdiv(c);
        if (g.h > 0 && vdiv > 0.0) {
            const double cy = g.y + g.h / 2.0;
            // Screen divisions, kept on the grid, in 1/25 div steps (the
            // dock's level arrows step vdiv/25).
            double divs = (cy - event->pos().y()) / (g.h / 8.0);
            divs = qBound(-4.0, std::round(divs * 25.0) / 25.0, 4.0);
            const double lev = divs * vdiv - chanOffs(c);
            if (lev != m_trigPrev) {
                m_trigPrev = lev;
                m_trigDirty = true;
            }
            if (m_trigDirty && m_emitClock.elapsed() >= 80) {
                emit triggerLevelRequested(m_trigPrev);
                m_trigDirty = false;
                m_emitClock.restart();
            }
        }
        update();
        event->accept();
        return;
    }
    if (m_curDrag >= 0) {
        const Grid g = grid();
        if (m_curDrag < 2 && g.w > 0 && m_span > 0.0) {
            const double x = qBound(double(g.x), double(event->pos().x()), double(g.x + g.w));
            m_curT[m_curDrag] = m_center - m_span / 2.0 + m_span * (x - g.x) / g.w;
        } else if (m_curDrag >= 2 && g.h > 0 && chanVdiv(m_curCh) > 0.0) {
            const double y = qBound(double(g.y), double(event->pos().y()), double(g.y + g.h));
            const double divs = (g.y + g.h / 2.0 - y) / (g.h / 8.0);
            m_curV[m_curDrag - 2] = divs * chanVdiv(m_curCh) - chanOffs(m_curCh);
        }
        emitCursors();
        update();
        event->accept();
        return;
    }
    if (!m_dragging) {
        const int cur = cursorAt(event->pos());
        setCursor(cur >= 0                                              ? (cur < 2 ? Qt::SizeHorCursor
                                                                                   : Qt::SizeVerCursor)
                  : tagAt(event->pos()) >= 0 || trigTagAt(event->pos()) ? Qt::SizeVerCursor
                                                                        : Qt::CrossCursor);
    }
    if (m_dragging && !rollActive()) {
        const Grid g = grid();
        const int dx = event->pos().x() - m_dragPos.x();
        if (g.w > 0 && dx != 0) {
            const double dt = -double(dx) * m_span / g.w; // window shift, s
            m_center += dt;
            clampView();
            if (m_frame.running) {
                // Pan through the record: no TIM:OFFS.
                m_runShift = m_center - devCenter();
                emitShift(false);
            } else if (!m_deep && m_emitClock.elapsed() >= 80) {
                emit viewWindowChanged(m_center, m_span);
                m_emitClock.restart();
            }
        }
        m_dragPos = event->pos();
        m_liveDirty = true; // the live trace follows the drag every tick
    }
    update();
    event->accept();
}

void WaveView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_curDrag >= 0) {
        m_curDrag = -1;
        setCursor(Qt::CrossCursor);
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_tagDrag >= 0) {
        const int c = m_tagDrag;
        m_tagDrag = -1;
        // Always send the final position, then hold the preview until a
        // frame carries it (or 1.5 s pass), so the tag does not snap back.
        emit channelOffsetRequested(c + 1, m_tagOffs[c]);
        m_tagDirty = false;
        m_tagHeld[c] = true;
        m_tagLock[c].restart();
        setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_trigDrag) {
        m_trigDrag = false;
        emit triggerLevelRequested(m_trigPrev); // final position always
        m_trigDirty = false;
        m_trigHeld = true;
        m_trigLock.restart();
        setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::CrossCursor);
        // Flush any un-emitted remainder so the device lands where the
        // mouse did.
        if (m_have) {
            if (m_frame.running) {
                emitShift(true);
            } else if (!m_frame.running && !m_deep) {
                emit viewWindowChanged(m_center, m_span);
            }
        }
        event->accept();
    }
}

// --- Cursors + crosshair readout --------------------------------------

void WaveView::setCursorMode(int mode)
{
    const int added = mode & ~m_curMode;
    m_curMode = mode & 3;
    const double t0 = m_center - m_span / 2.0;
    if ((added & 1) && m_span > 0.0) {
        m_curT[0] = t0 + 0.3 * m_span;
        m_curT[1] = t0 + 0.7 * m_span;
    }
    if (added & 2) {
        const double vdiv = chanVdiv(m_curCh), offs = chanOffs(m_curCh);
        m_curV[0] = 2.0 * vdiv - offs;
        m_curV[1] = -2.0 * vdiv - offs;
    }
    m_curDrag = -1;
    emitCursors();
    update();
}

void WaveView::setCursorChannel(int ch)
{
    m_curCh = qBound(0, ch, 1);
    const int mode = m_curMode;
    m_curMode &= ~2;
    setCursorMode(mode); // re-places the level cursors on the new scale
}

void WaveView::emitCursors()
{
    emit cursorsChanged(m_curMode, m_curT[0], m_curT[1], m_curV[0], m_curV[1], m_curCh);
}

int WaveView::cursorAt(const QPoint &pos) const
{
    if (!m_curMode)
        return -1;
    const Grid g = grid();
    if (g.w <= 0 || g.h <= 0)
        return -1;
    if ((m_curMode & 1) && m_span > 0.0) {
        const double t0 = m_center - m_span / 2.0;
        for (int i = 0; i < 2; ++i)
            if (std::abs(g.x + g.w * (m_curT[i] - t0) / m_span - pos.x()) <= 5.0)
                return i;
    }
    const double vdiv = chanVdiv(m_curCh);
    if ((m_curMode & 2) && vdiv > 0.0) {
        const double cy = g.y + g.h / 2.0;
        for (int i = 0; i < 2; ++i) {
            const double y = cy - (m_curV[i] + chanOffs(m_curCh)) / vdiv * (g.h / 8.0);
            if (std::abs(y - pos.y()) <= 5.0)
                return 2 + i;
        }
    }
    return -1;
}

void WaveView::drawCursors(QPainter &p, const Grid &g)
{
    if (!m_curMode || g.w <= 0 || g.h <= 0)
        return;
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    if ((m_curMode & 1) && m_span > 0.0) {
        const QColor c(0xa7, 0x8b, 0xfa);
        const double t0 = m_center - m_span / 2.0;
        for (int i = 0; i < 2; ++i) {
            const double x = g.x + g.w * (m_curT[i] - t0) / m_span;
            if (x < g.x - 1 || x > g.x + g.w + 1)
                continue;
            QPen pen(c, i == m_curDrag ? 1.6 : 1.0);
            pen.setDashPattern({6.0, 3.0});
            p.setPen(pen);
            p.drawLine(QPointF(x, g.y), QPointF(x, g.y + g.h));
            p.drawText(QPointF(x + 3, g.y + 12), i ? QStringLiteral("B") : QStringLiteral("A"));
        }
    }
    const double vdiv = chanVdiv(m_curCh);
    if ((m_curMode & 2) && vdiv > 0.0) {
        const QColor c(0x34, 0xd3, 0x99);
        const double cy = g.y + g.h / 2.0;
        for (int i = 0; i < 2; ++i) {
            const double y = cy - (m_curV[i] + chanOffs(m_curCh)) / vdiv * (g.h / 8.0);
            if (y < g.y - 1 || y > g.y + g.h + 1)
                continue;
            QPen pen(c, i == m_curDrag - 2 ? 1.6 : 1.0);
            pen.setDashPattern({6.0, 3.0});
            p.setPen(pen);
            p.drawLine(QPointF(g.x, y), QPointF(g.x + g.w, y));
            p.drawText(QPointF(g.x + g.w - 12, y - 3), i ? QStringLiteral("B") : QStringLiteral("A"));
        }
    }
}

void WaveView::drawReadout(QPainter &p, const Grid &g)
{
    if (m_span <= 0.0 || g.w <= 0 || g.h <= 0)
        return;
    const double t = m_center - m_span / 2.0 + m_span * (m_mouse.x() - g.x) / g.w;
    QStringList lines;
    lines << QStringLiteral("t %1").arg(fmt::seconds(t));
    const double cy = g.y + g.h / 2.0;
    const double divs = (cy - m_mouse.y()) / (g.h / 8.0);
    for (int c = 0; c < 2; ++c) {
        if (!m_have || !m_frame.ch[c].valid)
            continue;
        const double vdiv = chanVdiv(c);
        if (vdiv > 0.0)
            lines << QStringLiteral("CH%1 %2").arg(c + 1).arg(fmt::volts(divs * vdiv - chanOffs(c)));
    }
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(false);
    p.setFont(f);
    const QFontMetrics fm(f);
    int w = 0;
    for (const QString &l : lines)
        w = qMax(w, fm.horizontalAdvance(l));
    const int lh = fm.height();
    QRectF box(m_mouse.x() + 12, m_mouse.y() + 12, w + 12, lh * lines.size() + 8);
    if (box.right() > g.x + g.w)
        box.moveRight(m_mouse.x() - 12);
    if (box.bottom() > g.y + g.h)
        box.moveBottom(m_mouse.y() - 12);
    p.setPen(QColor(0x24, 0x34, 0x49));
    p.setBrush(QColor(0x0d, 0x17, 0x26, 230));
    p.drawRoundedRect(box, 4, 4);
    for (int i = 0; i < lines.size(); ++i) {
        p.setPen(i == 0 ? QColor(0xe2, 0xe8, 0xf0)
                 : lines.at(i).startsWith(QLatin1String("CH1")) ? QColor(0xfa, 0xcc, 0x15)
                                                                 : QColor(0x38, 0xbd, 0xf8));
        p.drawText(QPointF(box.x() + 6, box.y() + 4 + fm.ascent() + i * lh), lines.at(i));
    }
}

void WaveView::resizeEvent(QResizeEvent *event)
{
    m_liveDirty = true;
    QWidget::resizeEvent(event);
}

void WaveView::leaveEvent(QEvent *event)
{
    m_mouseIn = false;
    update();
    QWidget::leaveEvent(event);
}
