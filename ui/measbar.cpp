#include "measbar.h"

#include "core/framepump.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>

#include <cmath>

namespace {

const char *const kChColor[2] = {"#facc15", "#38bdf8"};

// Every raw point of the meter's 10-division screen, as signed ADC codes.
// Empty when the frame cannot give it (decimated, roll, missing points).
bool screenCodes(const WaveFrame &f, const ChannelFrame &c, QVector<int8_t> &out,
                 QString &why)
{
    out.clear();
    if (!c.valid || c.count < 2 || c.yinc <= 0.0 || !(c.xinc > 0.0) || f.scal <= 0.0) {
        why = QObject::tr("No data on this channel");
        return false;
    }
    if (f.roll) {
        why = QObject::tr("No values in roll mode");
        return false;
    }
    if (c.stride != 1) {
        why = QObject::tr("Values need 2 us/div or faster for now. Slower timebases "
                          "wait for the meter to send its own readouts");
        return false;
    }
    // The meter's own time base: xorig without the PC-side sub-sample shift.
    const double x0 = c.xorig - c.tshift;
    const double t0 = -(qBound(0.0, f.pos, 100.0) / 100.0) * 10.0 * f.scal;
    const double t1 = t0 + 10.0 * f.scal;
    const int i0 = qMax(0, int(std::ceil((t0 - x0) / c.xinc - 1e-6)));
    const int i1 = qMin(c.count - 1, int(std::floor((t1 - x0) / c.xinc + 1e-6)));
    const int want = int(std::lround(10.0 * f.scal / c.xinc)) + 1;
    if (i1 - i0 + 1 < want * 9 / 10) {
        why = QObject::tr("The screen is not all in the fetched window");
        return false;
    }
    out.resize(i1 - i0 + 1);
    const double k = 1.0 / c.yinc, add = c.yorg + c.yref - 128.0;
    for (int i = i0; i <= i1; ++i) {
        const long q = std::lround(double(c.v[i]) * k + add);
        out[i - i0] = int8_t(qBound(-128L, q, 127L));
    }
    return true;
}

} // namespace

MeasBar::MeasBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("measCard"));
    setAttribute(Qt::WA_StyledBackground, true);
    m_sel[0] = {meas::Freq, meas::Vpp, meas::Rms};
    m_sel[1] = {meas::Freq, meas::Vpp, meas::Duty};
    m_line = new QHBoxLayout(this);
    m_line->setContentsMargins(8, 3, 8, 3);
    m_line->setSpacing(10);
    rebuild();
}

void MeasBar::setOnline(bool on)
{
    if (on)
        return;
    for (int c = 0; c < 2; ++c) {
        m_res[c] = meas::Result();
        m_fwRes[c] = meas::Result();
        m_fwHas[c] = false;
        m_decimated[c] = false;
        m_on[c] = false;
    }
    rebuild();
    setCounter(false, 0.0, -1, 1);
}

void MeasBar::rebuild()
{
    while (QLayoutItem *it = m_line->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    auto *title = new QLabel(tr("MEAS"), this);
    title->setProperty("cls", QStringLiteral("cardTitle"));
    m_line->addWidget(title);
    for (int c = 0; c < 2; ++c) {
        m_items[c].clear();
        if (c == 1) {
            auto *sep = new QFrame(this);
            sep->setObjectName(QStringLiteral("measSep"));
            sep->setFixedWidth(1);
            m_line->addWidget(sep);
        }
        auto *ch = new QLabel(QStringLiteral("CH%1").arg(c + 1), this);
        ch->setProperty("cls", QStringLiteral("measCh"));
        ch->setStyleSheet(QStringLiteral("color:%1").arg(QLatin1String(kChColor[c])));
        m_line->addWidget(ch);
        for (meas::Kind k : m_sel[c]) {
            auto *item = new QLabel(this);
            item->setProperty("cls", QStringLiteral("measItem"));
            item->setCursor(Qt::PointingHandCursor);
            item->setToolTip(tr("%1. Click to remove").arg(QLatin1String(meas::name(k))));
            item->installEventFilter(this);
            item->setProperty("measCh", c);
            item->setProperty("measKind", int(k));
            m_items[c].append(item);
            m_line->addWidget(item);
        }
        auto *add = new QPushButton(QStringLiteral("+"), this);
        add->setProperty("cls", QStringLiteral("measAdd"));
        add->setToolTip(tr("Add a measurement to CH%1").arg(c + 1));
        connect(add, &QPushButton::clicked, this, [this, c, add] { addMenu(c, add); });
        m_line->addWidget(add);
    }
    m_line->addStretch(1);
    auto *sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("measSep"));
    sep->setFixedWidth(1);
    m_line->addWidget(sep);
    m_counter = new QLabel(this);
    m_counter->setProperty("cls", QStringLiteral("measItem"));
    m_counter->setToolTip(tr("Hardware frequency counter on the trigger channel "
                             "(MEAS:FCNT?, MEAS:FCNT:QUAL?)"));
    m_line->addWidget(m_counter);
    if (m_counterText.isEmpty())
        setCounter(false, 0.0, -1, 1);
    else
        m_counter->setText(m_counterText);
    paintValues();
}

void MeasBar::setCounter(bool known, double hz, int quality, int channel)
{
    static const char *const kQual[5] = {"CLEAN", "FILTERED", "UNVERIFIED", "UNCERTAIN",
                                         "CHANGING"};
    QString val = QStringLiteral("--");
    if (known && hz > 0.0) {
        const char *pre = "";
        double v = hz;
        if (v >= 1e6) {
            v /= 1e6;
            pre = "M";
        } else if (v >= 1e3) {
            v /= 1e3;
            pre = "k";
        }
        val = QStringLiteral("%1 %2Hz").arg(v, 0, 'f', 6).arg(QLatin1String(pre));
    }
    const QString q = quality >= 0 && quality < 5 ? QLatin1String(kQual[quality]) : QString();
    const QString color = quality == 0 ? QStringLiteral("#4ade80")
                          : quality == 1 ? QStringLiteral("#94a3b8")
                                         : QStringLiteral("#fbbf24");
    m_counterText = QStringLiteral("<span style=\"color:#94a3b8;font-size:11px\">FC%1</span>"
                                   "&nbsp;%2 <span style=\"color:%3;font-size:10px\">%4</span>")
                        .arg(channel)
                        .arg(val.toHtmlEscaped(), color, q);
    if (m_counter)
        m_counter->setText(m_counterText);
}

bool MeasBar::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::MouseButtonRelease) {
        const QVariant ch = o->property("measCh"), k = o->property("measKind");
        if (ch.isValid() && k.isValid()) {
            m_sel[ch.toInt()].removeAll(meas::Kind(k.toInt()));
            QMetaObject::invokeMethod(this, &MeasBar::rebuild, Qt::QueuedConnection);
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}

void MeasBar::addMenu(int ch, QPushButton *anchor)
{
    QMenu menu(this);
    for (int i = 0; i < meas::Count; ++i) {
        const auto k = meas::Kind(i);
        if (k == meas::Off || k == meas::Fc || m_sel[ch].contains(k))
            continue;
        menu.addAction(QLatin1String(meas::name(k)), this, [this, ch, k] {
            m_sel[ch].append(k);
            QMetaObject::invokeMethod(this, &MeasBar::rebuild, Qt::QueuedConnection);
        });
    }
    if (!menu.isEmpty())
        menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void MeasBar::paintValues()
{
    for (int c = 0; c < 2; ++c)
        for (QLabel *item : m_items[c]) {
            const auto k = meas::Kind(item->property("measKind").toInt());
            QString mark;
            const QString val = m_on[c] ? meas::text(m_res[c], k, &mark) : QStringLiteral("--");
            item->setText(QStringLiteral("<span style=\"color:#94a3b8;font-size:11px\">%1%2"
                                         "</span>&nbsp;%3")
                              .arg(QLatin1String(meas::tag(k)), mark.toHtmlEscaped(),
                                   val.toHtmlEscaped()));
            QString tip = tr("%1. Click to remove").arg(QLatin1String(meas::name(k)));
            if (!m_on[c] && !m_why[c].isEmpty())
                tip = m_why[c] + QStringLiteral("\n") + tip;
            item->setToolTip(tip);
        }
}

void MeasBar::setFirmwareMeas(int ch, const meas::Result &res)
{
    if (ch < 0 || ch > 1)
        return;
    m_fwRes[ch] = res;
    m_fwHas[ch] = res.valid;
    if (m_decimated[ch]) {
        m_res[ch] = res;
        m_on[ch] = res.valid;
        m_why[ch].clear();
        paintValues();
    }
}

void MeasBar::setFrame(const WaveFrame &f)
{
    // 250 ms like the meter's ReadoutScheduler. A stopped scope keeps the
    // last values (no new frames).
    if (m_last.isValid() && m_last.elapsed() < 250)
        return;
    if (!f.settled || f.full)
        return;
    m_last.start();
    QVector<int8_t> codes;
    for (int c = 0; c < 2; ++c) {
        const ChannelFrame &cf = f.ch[c];
        m_decimated[c] = cf.valid && !f.roll && cf.stride != 1;
        if (m_decimated[c] && m_fwHas[c]) {
            m_res[c] = m_fwRes[c];
            m_on[c] = m_fwRes[c].valid;
            m_why[c].clear();
            continue;
        }
        if (!screenCodes(f, cf, codes, m_why[c])) {
            m_on[c] = false;
            continue;
        }
        const double rate = 1.0 / cf.xinc;
        const double zero = cf.yorg + cf.yref - 128.0;
        m_res[c] = meas::screenBasis(codes.constData(), codes.size(), rate, zero, cf.yinc);
        m_on[c] = m_res[c].valid;
        m_why[c].clear();
    }
    paintValues();
}
