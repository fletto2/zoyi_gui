#include "overview.h"

#include "theme.h"
#include "waveview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace {

// Same horizontal margins as the plot grid, so the strip lines up under it.
constexpr int kLeft = 40, kRight = 18;
// (E) Trace area: below the label row, 8 plot divisions tall.
constexpr double kTop = 12.0, kBot = 4.0;
const QColor kChColor[2] = {QColor(0xfa, 0xcc, 0x15), QColor(0x38, 0xbd, 0xf8)};

QString fmtTime(double s)
{
    const double a = std::abs(s);
    if (a >= 1.0)
        return QString::number(s, 'g', 3) + QStringLiteral(" s");
    if (a >= 1e-3)
        return QString::number(s * 1e3, 'g', 3) + QStringLiteral(" ms");
    if (a >= 1e-6)
        return QString::number(s * 1e6, 'g', 3) + QStringLiteral(" us");
    return QString::number(s * 1e9, 'g', 3) + QStringLiteral(" ns");
}

} // namespace

OverviewStrip::OverviewStrip(WaveView *view, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
    , m_poll(this)
{
    setFixedHeight(60);
    setMouseTracking(true);
    setToolTip(tr("Acquisition record - the box is the slice on screen. Drag the box, "
                  "or click beside it, to move the view"));
    connect(m_view, &WaveView::recordChanged, this, [this] { update(); });
    m_poll.setInterval(16); // 60 f/s: the roll history scrolls with the plot
    connect(&m_poll, &QTimer::timeout, this, &OverviewStrip::poll);
    m_poll.start();
}

void OverviewStrip::poll()
{
    const WaveView::RollHist *h = nullptr;
    double right = 0.0;
    if (m_view->rollHistory(h, right)) {
        update(); // rolling: the strip moves every tick
        return;
    }
    if (!(currentMap() == m_envMap)) {
        update(); // (E) V/div or offset changed: redraw at the new scale
        return;
    }
    double s, w, t;
    if (!m_view->overviewBox(s, w, t))
        return;
    if (s != m_lastStart || w != m_lastWidth || t != m_lastTrig)
        update();
}

bool OverviewStrip::VMap::operator==(const VMap &o) const
{
    for (int c = 0; c < 2; ++c) {
        if (ok[c] != o.ok[c] || (ok[c] && (vdiv[c] != o.vdiv[c] || offs[c] != o.offs[c])))
            return false;
    }
    return true;
}

OverviewStrip::VMap OverviewStrip::currentMap() const
{
    VMap m;
    for (int c = 0; c < 2; ++c)
        m.ok[c] = m_view->channelMap(c, m.vdiv[c], m.offs[c]);
    return m;
}

double OverviewStrip::mapY(const VMap &m, int c, double v) const
{
    // Same formula as the plot: screen divs = (v + offs) / vdiv from the
    // center, 8 divisions over the area height.
    const double top = kTop, bot = height() - kBot;
    const double mid = (top + bot) / 2.0;
    const double y = mid - ((v + m.offs[c]) / m.vdiv[c]) * ((bot - top) / 8.0);
    // Far off-screen values are pulled in just past the edge (clipped
    // there), so huge numbers never reach the painter.
    return qBound(top - 4.0, y, bot + 4.0);
}

double OverviewStrip::fracAt(int x) const
{
    const int gw = width() - kLeft - kRight;
    return gw > 0 ? double(x - kLeft) / gw : 0.0;
}

void OverviewStrip::buildEnvelope()
{
    const qreal dpr = devicePixelRatioF();
    m_env = QPixmap(QSize(qRound(width() * dpr), qRound(height() * dpr)));
    m_env.setDevicePixelRatio(dpr);
    m_env.fill(Qt::transparent);
    m_envSize = size();
    const WaveFrame &rec = m_view->record();
    m_envStamp = m_view->hasRecord() ? rec.stampMs : -1;

    const VMap vm = currentMap();
    m_envMap = vm;
    QPainter e(&m_env);
    e.setRenderHint(QPainter::Antialiasing, true);
    const int gx = kLeft, gw = width() - kLeft - kRight;
    const double top = kTop, bot = height() - kBot;
    if (gw >= 2)
        e.fillRect(QRectF(gx, top, gw, bot - top), QColor(255, 255, 255, 8));
    // (E) Traces outside the 8 divisions are cut at the area edge, as on
    // the plot.
    e.setClipRect(QRectF(gx, top, gw, bot - top));
    for (int c = 0; c < 2; ++c) {
        const ChannelFrame &cf = rec.ch[c];
        const bool on = m_view->hasRecord() && cf.valid && cf.v.size() > 1 && vm.ok[c];
        if (!on || gw < 2)
            continue;
        // Per-column min/max over the record (index space: the strip shows
        // the record edge to edge).
        const int cols = gw;
        const int n = cf.v.size();
        const bool peak = cf.isPeak();
        QVector<double> lo(cols + 1), hi(cols + 1);
        for (int i = 0; i <= cols; ++i) {
            const int a = int(double(i) * (n - 1) / cols);
            const int b = qMax(a, int(double(i + 1) * (n - 1) / cols));
            double l = cf.v[a], h = peak ? cf.vHi[a] : cf.v[a];
            for (int k = a; k <= qMin(b, n - 1); ++k) {
                l = qMin(l, double(cf.v[k]));
                h = qMax(h, double(peak ? cf.vHi[k] : cf.v[k]));
            }
            // Screen y; a flat column still gets 1.5 px so it shows.
            double yh = mapY(vm, c, h), yl = mapY(vm, c, l);
            if (yl - yh < 1.5) {
                const double m = (yh + yl) / 2.0;
                yh = m - 0.75;
                yl = m + 0.75;
            }
            hi[i] = yh;
            lo[i] = yl;
        }
        QPainterPath band;
        band.moveTo(gx, hi[0]);
        for (int i = 1; i <= cols; ++i)
            band.lineTo(gx + i, hi[i]);
        for (int i = cols; i >= 0; --i)
            band.lineTo(gx + i, lo[i]);
        band.closeSubpath();
        QColor glow = kChColor[c];
        glow.setAlpha(41);
        e.strokePath(band, QPen(glow, 4.0));
        QColor fill = kChColor[c];
        fill.setAlpha(77);
        e.fillPath(band, fill);
        e.strokePath(band, QPen(kChColor[c], 1.4));
    }
}

void OverviewStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0d, 0x15, 0x26));
    p.setPen(QColor(0x24, 0x34, 0x49));
    p.drawLine(0, 0, width(), 0);

    const int gx = kLeft, gw = width() - kLeft - kRight;
    double s, w, t;
    const bool box = m_view->overviewBox(s, w, t) && gw >= 2;
    QFont f(theme::monoFamily());
    f.setPixelSize(9);
    p.setFont(f);
    const WaveView::RollHist *h = nullptr;
    double right = 0.0;
    if (m_view->rollHistory(h, right)) {
        paintRoll(p, *h, right);
        p.setPen(QColor(0x42, 0x59, 0x7e));
        p.drawText(QPointF(4, 10), fmtTime(h->bucketS * h->n));
    } else {
        const qint64 stamp = m_view->hasRecord() ? m_view->record().stampMs : -1;
        if (m_env.isNull() || m_envSize != size() || m_envStamp != stamp
            || !(currentMap() == m_envMap))
            buildEnvelope();
        // The record picture is fixed: only the box moves.
        p.drawPixmap(0, 0, m_env);
        if (m_view->hasRecord()) {
            const WaveFrame &r = m_view->record();
            const QString n = r.recN >= 1024
                                  ? QString::number(r.recN / 1024) + QLatin1Char('K')
                                  : QString::number(r.recN);
            p.setPen(QColor(0x42, 0x59, 0x7e));
            p.drawText(QPointF(4, 10),
                       QStringLiteral("%1 - %2").arg(n, fmtTime(r.recT1 - r.recT0)));
        }
    }

    if (!box) {
        m_lastStart = m_lastWidth = m_lastTrig = -1.0;
        return;
    }
    m_lastStart = s;
    m_lastWidth = w;
    m_lastTrig = t;
    // Box at least 2 px wide so it stays grabbable at deep zoom; the rest
    // of the record is dimmed so the on-screen slice stands out.
    const double bw = qMax(2.0, gw * w);
    const double bx = gx + gw * s;
    const QColor dim(4, 9, 18, 140);
    p.fillRect(QRectF(gx, 0, qMax(0.0, bx - gx), height()), dim);
    p.fillRect(QRectF(bx + bw, 0, qMax(0.0, gx + gw - (bx + bw)), height()), dim);
    p.setPen(QPen(QColor(226, 232, 240, 217), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(bx + 0.5, 1.5, bw - 1.0, height() - 3.0));
    if (t >= 0.0 && t <= 1.0) {
        const double tx = gx + gw * t;
        QPolygonF tri;
        tri << QPointF(tx, 7) << QPointF(tx - 5, 0) << QPointF(tx + 5, 0);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xfb, 0x92, 0x3c));
        p.drawPolygon(tri);
    }
}

void OverviewStrip::paintRoll(QPainter &p, const WaveView::RollHist &h, double right)
{
    // The bands are cached at bucket positions (bucket j ends at x = gx +
    // gw x (j+1)/n, the newest bucket at the right edge) and drawn shifted by
    // the time between the newest bucket and the plot's right edge now.
    const int gx = kLeft, gw = width() - kLeft - kRight;
    const double top = kTop, bot = height() - kBot;
    if (gw < 2 || h.n < 2)
        return;
    const VMap vm = currentMap();
    if (m_rollEnv.isNull() || m_envSize != size() || m_rollGen != h.gen
        || !(vm == m_rollMap)) {
        m_rollMap = vm;
        const qreal dpr = devicePixelRatioF();
        m_rollEnv = QPixmap(QSize(qRound(width() * dpr), qRound(height() * dpr)));
        m_rollEnv.setDevicePixelRatio(dpr);
        m_rollEnv.fill(Qt::transparent);
        m_envSize = size();
        m_env = QPixmap(); // the record cache is stale now
        m_rollGen = h.gen;
        QPainter e(&m_rollEnv);
        e.setRenderHint(QPainter::Antialiasing, true);
        e.setClipRect(QRectF(gx, top, gw, bot - top)); // (E) cut at the area
        const double dx = double(gw) / h.n;
        for (int c = 0; c < 2; ++c) {
            const float *lo = h.lo[c].constData(), *hi = h.hi[c].constData();
            m_rollOn[c] = false;
            for (int j = 0; j < h.n && !m_rollOn[c]; ++j)
                m_rollOn[c] = !std::isnan(lo[j]);
            // (E) Drawn at the plot's V/div and offset, so a channel the
            // plot has turned off (no mapping) is not drawn either.
            if (!m_rollOn[c] || !vm.ok[c])
                continue;
            auto map = [&](double v) { return mapY(vm, c, v); };
            // Rebuilt on every roll frame (about 16 f/s), so no wide AA
            // strokes: those took the whole GUI thread down to 10.6 f/s
            // (measured). One plain column per bucket, then the
            // top and bottom edges as thin lines. Gaps (no data) stay empty.
            e.setRenderHint(QPainter::Antialiasing, false);
            const double flat = 0.5; // a flat trace still shows as a line
            QVector<QLineF> cols;
            QVector<QLineF> edges;
            cols.reserve(h.n);
            edges.reserve(2 * h.n);
            for (int j = 0; j < h.n; ++j) {
                if (std::isnan(lo[j]))
                    continue;
                const double x = gx + dx * (j + 0.5);
                const double yh = map(hi[j]) - flat, yl = map(lo[j]) + flat;
                cols.append(QLineF(x, yh, x, yl));
                if (j + 1 < h.n && !std::isnan(lo[j + 1])) {
                    const double x2 = x + dx;
                    edges.append(QLineF(x, yh, x2, map(hi[j + 1]) - flat));
                    edges.append(QLineF(x, yl, x2, map(lo[j + 1]) + flat));
                }
            }
            QColor fill = kChColor[c];
            fill.setAlpha(77);
            e.setPen(QPen(fill, qMax(1.0, dx)));
            e.drawLines(cols);
            e.setPen(QPen(kChColor[c], 1.0));
            e.drawLines(edges);
        }
    }
    p.fillRect(QRectF(gx, top, gw, bot - top), QColor(255, 255, 255, 8));
    const double shift = gw * (h.headS - right) / (h.bucketS * h.n);
    p.save();
    p.setClipRect(QRectF(gx, 0, gw, height()));
    p.drawPixmap(QPointF(shift, 0), m_rollEnv);
    p.restore();
}

void OverviewStrip::mousePressEvent(QMouseEvent *event)
{
    const WaveView::RollHist *h = nullptr;
    double right = 0.0;
    if (m_view->rollHistory(h, right))
        return; // roll: the box stays snapped to the right
    double s, w, t;
    if (event->button() != Qt::LeftButton || !m_view->overviewBox(s, w, t))
        return;
    const double f = fracAt(event->pos().x());
    const int gw = width() - kLeft - kRight;
    const double halfPx = gw > 0 ? qMax(6.0 / gw, w / 2.0) : w / 2.0;
    if (std::abs(f - (s + w / 2.0)) > halfPx) {
        // Beside the box: center the view there, then drag from the middle.
        m_grab = w / 2.0;
        m_view->overviewMoveTo(f - m_grab, false);
    } else {
        m_grab = f - s;
    }
    m_dragging = true;
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void OverviewStrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_view->overviewMoveTo(fracAt(event->pos().x()) - m_grab, false);
    } else {
        double s, w, t;
        if (m_view->overviewBox(s, w, t)) {
            const double f = fracAt(event->pos().x());
            const int gw = width() - kLeft - kRight;
            const double halfPx = gw > 0 ? qMax(6.0 / gw, w / 2.0) : w / 2.0;
            setCursor(std::abs(f - (s + w / 2.0)) <= halfPx ? Qt::OpenHandCursor
                                                             : Qt::PointingHandCursor);
        }
    }
    event->accept();
}

void OverviewStrip::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        m_view->overviewMoveTo(fracAt(event->pos().x()) - m_grab, true);
        setCursor(Qt::OpenHandCursor);
        event->accept();
    }
}

void OverviewStrip::wheelEvent(QWheelEvent *event)
{
    // Same pacing and direction as the plot (trackpad fix, 2026-11-20).
    m_view->wheelZoom(event, 0.5);
}
