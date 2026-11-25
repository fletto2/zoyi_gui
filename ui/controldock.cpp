#include "controldock.h"

#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Glyphs as escapes: source files stay ASCII.
const QChar kUp(0x25B2), kDown(0x25BC), kLeft(0x25C4), kRight(0x25BA);
const QChar kMicro(0x00B5);

// V/div steps of the firmware at 1x (VoltageRanges::Values, mV).
const int kMv[] = {20, 40, 50, 100, 200, 500, 1000, 2000, 2500, 5000, 10000};
constexpr int kMvCount = int(sizeof kMv / sizeof kMv[0]);
const int kProbes[3] = {1, 10, 100};
const QColor kChColor[2] = {QColor(0xfa, 0xcc, 0x15), QColor(0x38, 0xbd, 0xf8)};

void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

// SetChecked(false) does nothing on the checked button of an exclusive
// QButtonGroup, so an unknown or offline state kept the old segment lit.
void setSegChecked(QPushButton *b, bool on)
{
    QButtonGroup *g = b->group();
    if (!on && b->isChecked() && g && g->exclusive()) {
        g->setExclusive(false);
        b->setChecked(false);
        g->setExclusive(true);
        return;
    }
    b->setChecked(on);
}

QPushButton *arrow(QChar glyph, const QString &tip, QWidget *parent)
{
    auto *b = new QPushButton(QString(glyph), parent);
    b->setProperty("cls", QStringLiteral("arrow"));
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAutoRepeat(true);
    b->setAutoRepeatDelay(400);
    b->setAutoRepeatInterval(120);
    return b;
}

QLabel *value(QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setProperty("cls", QStringLiteral("val"));
    l->setAlignment(Qt::AlignCenter);
    return l;
}

QLabel *rowLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setProperty("cls", QStringLiteral("rowLabel"));
    l->setFixedWidth(52);
    return l;
}

QPushButton *seg(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setProperty("cls", QStringLiteral("seg"));
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

} // namespace

namespace fmt {

QString volts(double v)
{
    const double a = std::abs(v);
    if (a > 0.0 && a < 1.0)
        return QStringLiteral("%1 mV").arg(v * 1000.0, 0, 'f', a < 0.1 ? 1 : 0);
    return QStringLiteral("%1 V").arg(v, 0, 'f', 2);
}

QString seconds(double s)
{
    const double a = std::abs(s);
    if (a == 0.0)
        return QStringLiteral("0 s");
    struct U {
        double f;
        QString n;
    };
    const U units[] = {{1.0, QStringLiteral("s")},
                       {1e-3, QStringLiteral("ms")},
                       {1e-6, QString(kMicro) + QLatin1Char('s')},
                       {1e-9, QStringLiteral("ns")}};
    for (const U &u : units) {
        if (a >= u.f * 0.9995 || u.f == 1e-9) {
            const double x = s / u.f;
            const int dec = std::abs(x) >= 100 ? 0 : std::abs(x) >= 10 ? 1 : 2;
            return QStringLiteral("%1 %2").arg(x, 0, 'f', dec).arg(u.n);
        }
    }
    return QString();
}

} // namespace fmt

// ---------------------------------------------------------------- ControlDock

ControlDock::ControlDock(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("controlDock"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(8);

    m_toggleBtn = new QPushButton(QStringLiteral("\u00AB ") + tr("Controls"), this);
    m_toggleBtn->setProperty("cls", QStringLiteral("collapseTab"));
    m_toggleBtn->setFlat(true);
    m_toggleBtn->setFocusPolicy(Qt::NoFocus);
    m_toggleBtn->setCursor(Qt::PointingHandCursor);
    m_toggleBtn->setToolTip(tr("Collapse / expand the channel and trigger controls"));
    connect(m_toggleBtn, &QPushButton::clicked, this,
            [this] { setCollapsed(!m_collapsed); });
    lay->addWidget(m_toggleBtn, 0, Qt::AlignLeft);

    m_fullBox = new QWidget(this);
    auto *fullLay = new QVBoxLayout(m_fullBox);
    fullLay->setContentsMargins(0, 0, 0, 0);
    fullLay->setSpacing(8);
    fullLay->addWidget(buildCard(0));
    fullLay->addWidget(buildCard(1));
    fullLay->addWidget(buildTrigCard());
    fullLay->addWidget(buildCursorCard());
    lay->addWidget(m_fullBox);

    m_sumBox = buildSummaryBox();
    m_sumBox->hide();
    lay->addWidget(m_sumBox);

    lay->addStretch(1);
    // The original design had a 238 px dock, but at that width the Position and Level
    // value boxes clip ("-2.90 div" showed as ".90 d").
    setFixedWidth(272);
    m_hold.setSingleShot(true);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        if (m_havePending) {
            m_havePending = false;
            applyState(m_pending);
        }
    });
    setOnline(false);
}

QWidget *ControlDock::buildSummaryBox()
{
    auto *box = new QWidget(this);
    auto *lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto makeSumCard = [&](const QString &colorHex, const QString &title) {
        auto *card = new QFrame(box);
        card->setObjectName(QStringLiteral("sumCard"));
        card->setCursor(Qt::PointingHandCursor);
        card->setToolTip(tr("Click to expand controls"));
        card->installEventFilter(this);
        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(10, 8, 10, 8);
        v->setSpacing(2);
        auto *name = new QLabel(QStringLiteral("<span style=\"color:%1\">&#9632;</span> %2")
                                    .arg(colorHex, title),
                                card);
        name->setProperty("cls", QStringLiteral("cardTitle"));
        v->addWidget(name);
        return std::pair<QFrame *, QVBoxLayout *>{card, v};
    };

    for (int c = 0; c < 2; ++c) {
        auto [card, v] = makeSumCard(kChColor[c].name(), QStringLiteral("CH%1").arg(c + 1));
        m_sumCh[c].scale = new QLabel(QStringLiteral("--"), card);
        m_sumCh[c].scale->setProperty("cls", QStringLiteral("sumBig"));
        m_sumCh[c].meta = new QLabel(QStringLiteral("--"), card);
        m_sumCh[c].meta->setProperty("cls", QStringLiteral("sumSm"));
        m_sumCh[c].pos = new QLabel(QStringLiteral("--"), card);
        m_sumCh[c].pos->setProperty("cls", QStringLiteral("sumSm"));
        auto *hint = new QLabel(tr("click to adjust"), card);
        hint->setProperty("cls", QStringLiteral("sumHint"));
        v->addWidget(m_sumCh[c].scale);
        v->addWidget(m_sumCh[c].meta);
        v->addWidget(m_sumCh[c].pos);
        v->addWidget(hint);
        lay->addWidget(card);
    }

    auto [tcard, tv] = makeSumCard(QStringLiteral("#fb923c"), tr("Trigger"));
    m_sumTrig.main = new QLabel(QStringLiteral("--"), tcard);
    m_sumTrig.main->setProperty("cls", QStringLiteral("sumBig"));
    m_sumTrig.level = new QLabel(QStringLiteral("--"), tcard);
    m_sumTrig.level->setProperty("cls", QStringLiteral("sumSm"));
    m_sumTrig.sweep = new QLabel(QStringLiteral("--"), tcard);
    m_sumTrig.sweep->setProperty("cls", QStringLiteral("sumSm"));
    auto *thint = new QLabel(tr("click to adjust"), tcard);
    thint->setProperty("cls", QStringLiteral("sumHint"));
    tv->addWidget(m_sumTrig.main);
    tv->addWidget(m_sumTrig.level);
    tv->addWidget(m_sumTrig.sweep);
    tv->addWidget(thint);
    lay->addWidget(tcard);

    auto [ccard, cv] = makeSumCard(QStringLiteral("#a78bfa"), tr("Cursors"));
    m_sumCur = new QLabel(QStringLiteral("--"), ccard);
    m_sumCur->setProperty("cls", QStringLiteral("sumSm"));
    m_sumCur->setTextFormat(Qt::RichText);
    m_sumCur->setWordWrap(true);
    auto *chint = new QLabel(tr("click to adjust"), ccard);
    chint->setProperty("cls", QStringLiteral("sumHint"));
    cv->addWidget(m_sumCur);
    cv->addWidget(chint);
    lay->addWidget(ccard);

    return box;
}

void ControlDock::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    m_fullBox->setVisible(!collapsed);
    m_sumBox->setVisible(collapsed);
    setFixedWidth(collapsed ? 148 : 272);
    m_toggleBtn->setText(collapsed ? QStringLiteral("\u00BB ") + tr("Controls")
                                   : QStringLiteral("\u00AB ") + tr("Controls"));
    emit collapsedChanged(collapsed);
}

bool ControlDock::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton && o->objectName() == QLatin1String("sumCard")) {
            setCollapsed(false);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

QWidget *ControlDock::buildCard(int c)
{
    Card &k = m_card[c];
    const int ch = c + 1;
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(10, 9, 10, 9);
    v->setSpacing(6);

    // Header: swatch + name, probe label, display switch.
    auto *h = new QHBoxLayout;
    auto *name = new QLabel(QStringLiteral("<span style=\"color:%1\">&#9632;</span> CH%2")
                                .arg(kChColor[c].name())
                                .arg(ch),
                            card);
    name->setProperty("cls", QStringLiteral("cardTitle"));
    h->addWidget(name);
    h->addStretch(1);
    k.probeLbl = new QLabel(card);
    k.probeLbl->setProperty("cls", QStringLiteral("dim"));
    h->addWidget(k.probeLbl);
    k.on = new QPushButton(card);
    k.on->setProperty("cls", QStringLiteral("switch"));
    k.on->setCheckable(true);
    k.on->setFocusPolicy(Qt::NoFocus);
    k.on->setToolTip(tr("Show or hide CH%1").arg(ch));
    connect(k.on, &QPushButton::clicked, this,
            [this, ch](bool on) { emit channelValue(ch, DeviceState::Disp, on ? 1.0 : 0.0); });
    h->addWidget(k.on);
    v->addLayout(h);

    // V/div: arrows around a big value.
    auto *vd = new QHBoxLayout;
    vd->setSpacing(6);
    auto *up = arrow(kUp, tr("Increase CH%1 V/div (wider voltage range)").arg(ch), card);
    auto *dn = arrow(kDown, tr("Decrease CH%1 V/div (zoom in vertically)").arg(ch), card);
    k.scale = value(card);
    k.scale->setProperty("big", true);
    connect(up, &QPushButton::clicked, this, [this, c] { stepScale(c, +1); });
    connect(dn, &QPushButton::clicked, this, [this, c] { stepScale(c, -1); });
    // Down on the left, up on the right.
    vd->addWidget(dn);
    vd->addWidget(k.scale, 1);
    vd->addWidget(up);
    v->addLayout(vd);

    // Position in divisions: arrows, value, zero.
    auto *pr = new QHBoxLayout;
    pr->setSpacing(5);
    pr->addWidget(rowLabel(tr("Position"), card));
    auto *pu = arrow(kUp, tr("Move CH%1 trace up").arg(ch), card);
    auto *pd = arrow(kDown, tr("Move CH%1 trace down").arg(ch), card);
    k.pos = value(card);
    auto *pz = new QPushButton(QStringLiteral("0"), card);
    pz->setProperty("cls", QStringLiteral("iconbtn"));
    pz->setFocusPolicy(Qt::NoFocus);
    pz->setToolTip(tr("Move CH%1 back to the center line").arg(ch));
    connect(pu, &QPushButton::clicked, this, [this, c] { stepPos(c, +0.2); });
    connect(pd, &QPushButton::clicked, this, [this, c] { stepPos(c, -0.2); });
    connect(pz, &QPushButton::clicked, this,
            [this, ch] { emit channelValue(ch, DeviceState::Offs, 0.0); });
    pr->addWidget(pd);
    pr->addWidget(k.pos, 1);
    pr->addWidget(pu);
    pr->addWidget(pz);
    v->addLayout(pr);

    // Coupling (AC/DC only: no GND in the firmware).
    auto *cr = new QHBoxLayout;
    cr->setSpacing(0);
    cr->addWidget(rowLabel(tr("Coupling"), card));
    cr->addSpacing(5);
    auto *cg = new QButtonGroup(card);
    k.ac = seg(QStringLiteral("AC"), card);
    k.dc = seg(QStringLiteral("DC"), card);
    cg->addButton(k.ac);
    cg->addButton(k.dc);
    k.ac->setToolTip(tr("AC coupling (blocks DC offset)"));
    k.dc->setToolTip(tr("DC coupling (shows AC and DC)"));
    connect(k.ac, &QPushButton::clicked, this,
            [this, ch] { emit channelValue(ch, DeviceState::Coup, 1.0); });
    connect(k.dc, &QPushButton::clicked, this,
            [this, ch] { emit channelValue(ch, DeviceState::Coup, 0.0); });
    cr->addWidget(k.ac, 1);
    cr->addWidget(k.dc, 1);
    v->addLayout(cr);

    // Probe presets (the firmware also takes 50/500/1000 over SCPI).
    auto *pb = new QHBoxLayout;
    pb->setSpacing(0);
    pb->addWidget(rowLabel(tr("Probe"), card));
    pb->addSpacing(5);
    auto *pg = new QButtonGroup(card);
    for (int i = 0; i < 3; ++i) {
        k.prob[i] = seg(QStringLiteral("%1x").arg(kProbes[i]), card);
        k.prob[i]->setToolTip(tr("Set CH%1 probe attenuation to %2x").arg(ch).arg(kProbes[i]));
        pg->addButton(k.prob[i]);
        const double p = kProbes[i];
        connect(k.prob[i], &QPushButton::clicked, this,
                [this, ch, p] { emit channelValue(ch, DeviceState::Prob, p); });
        pb->addWidget(k.prob[i], 1);
    }
    v->addLayout(pb);
    return card;
}

QWidget *ControlDock::buildTrigCard()
{
    TrigCard &k = m_trig;
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(10, 9, 10, 9);
    v->setSpacing(6);

    auto *h = new QHBoxLayout;
    auto *name = new QLabel(QStringLiteral("<span style=\"color:#fb923c\">&#9632;</span> TRIGGER"),
                            card);
    name->setProperty("cls", QStringLiteral("cardTitle"));
    h->addWidget(name);
    h->addStretch(1);
    auto *edge = new QLabel(tr("Edge"), card);
    edge->setProperty("cls", QStringLiteral("dim"));
    h->addWidget(edge);
    v->addLayout(h);

    // One segmented row: label + buttons in an exclusive group.
    auto segRow = [&](const QString &label, std::initializer_list<QPushButton **> btns,
                      std::initializer_list<QString> texts) {
        auto *r = new QHBoxLayout;
        r->setSpacing(0);
        r->addWidget(rowLabel(label, card));
        r->addSpacing(5);
        auto *g = new QButtonGroup(card);
        auto t = texts.begin();
        for (QPushButton **b : btns) {
            *b = seg(*t++, card);
            g->addButton(*b);
            r->addWidget(*b, 1);
        }
        v->addLayout(r);
    };

    segRow(tr("Source"), {&k.src[0], &k.src[1]}, {QStringLiteral("CH1"), QStringLiteral("CH2")});
    for (int i = 0; i < 2; ++i) {
        k.src[i]->setToolTip(tr("Trigger on CH%1 (channel must be on)").arg(i + 1));
        connect(k.src[i], &QPushButton::clicked, this,
                [this, i] { emit triggerValue(DeviceState::TSour, i + 1); });
    }

    // Arrows as escapes (ASCII source): up-right / down-right.
    segRow(tr("Slope"), {&k.rise, &k.fall},
           {QString(QChar(0x2197)) + tr(" Rising"), QString(QChar(0x2198)) + tr(" Falling")});
    k.rise->setToolTip(tr("Trigger on rising edge"));
    k.fall->setToolTip(tr("Trigger on falling edge"));
    connect(k.rise, &QPushButton::clicked, this,
            [this] { emit triggerValue(DeviceState::TSlop, 1.0); });
    connect(k.fall, &QPushButton::clicked, this,
            [this] { emit triggerValue(DeviceState::TSlop, 0.0); });

    auto *lr = new QHBoxLayout;
    lr->setSpacing(5);
    lr->addWidget(rowLabel(tr("Level"), card));
    auto *lu = arrow(kUp, tr("Raise trigger level"), card);
    auto *ld = arrow(kDown, tr("Lower trigger level"), card);
    k.level = value(card);
    auto *lz = new QPushButton(QStringLiteral("0"), card);
    lz->setProperty("cls", QStringLiteral("iconbtn"));
    lz->setFocusPolicy(Qt::NoFocus);
    lz->setToolTip(tr("Reset trigger level to 0 V"));
    connect(lu, &QPushButton::clicked, this, [this] { stepLevel(+1); });
    connect(ld, &QPushButton::clicked, this, [this] { stepLevel(-1); });
    connect(lz, &QPushButton::clicked, this,
            [this] { emit triggerValue(DeviceState::TLev, 0.0); });
    lr->addWidget(ld);
    lr->addWidget(k.level, 1);
    lr->addWidget(lu);
    lr->addWidget(lz);
    v->addLayout(lr);

    segRow(tr("Sweep"), {&k.swe[0], &k.swe[1], &k.swe[2]},
           {tr("Auto"), tr("Normal"), tr("Single")});
    const QString sweTips[3] = {
        tr("Auto sweep: triggers on signal, or free-runs if none"),
        tr("Normal sweep: updates only when triggered"),
        tr("Single sweep: captures one trigger and stops"),
    };
    for (int i = 0; i < 3; ++i) {
        k.swe[i]->setToolTip(sweTips[i]);
        connect(k.swe[i], &QPushButton::clicked, this,
                [this, i] { emit triggerValue(DeviceState::TSwe, i); });
    }

    segRow(tr("Noise rej"), {&k.nrejOff, &k.nrejOn}, {tr("Off"), tr("On")});
    k.nrejOff->setToolTip(tr("Turn trigger noise rejection off"));
    k.nrejOn->setToolTip(tr("Turn trigger noise rejection on (reduces false triggers on noisy signals)"));
    connect(k.nrejOff, &QPushButton::clicked, this,
            [this] { emit triggerValue(DeviceState::TNrej, 0.0); });
    connect(k.nrejOn, &QPushButton::clicked, this,
            [this] { emit triggerValue(DeviceState::TNrej, 1.0); });

    // PC-side setting, not on the meter: on by default.
    segRow(tr("Align"), {&k.alignOff, &k.alignOn}, {tr("Off"), tr("On")});
    const QString alignTip =
        tr("PC sub-sample trigger alignment: shifts each raw frame so the level "
           "crossing sits exactly at t = 0 (removes the one-sample jitter)");
    k.alignOff->setToolTip(alignTip);
    k.alignOn->setToolTip(alignTip);
    k.alignOn->setChecked(true);
    connect(k.alignOff, &QPushButton::clicked, this, [this] { emit trigAlignChanged(false); });
    connect(k.alignOn, &QPushButton::clicked, this, [this] { emit trigAlignChanged(true); });

    auto *hint = new QLabel(tr("Level tag on the grid is draggable."), card);
    hint->setProperty("cls", QStringLiteral("dim"));
    v->addWidget(hint);
    return card;
}

QWidget *ControlDock::buildCursorCard()
{
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(5);

    auto *h = new QHBoxLayout;
    h->setSpacing(4);
    auto *title = new QLabel(tr("CURSORS"), card);
    title->setProperty("cls", QStringLiteral("cardTitle"));
    h->addWidget(title);
    h->addStretch(1);

    m_curTime = seg(tr("Time"), card);
    m_curLevel = seg(tr("Level"), card);
    m_curCh = new QPushButton(QStringLiteral("CH1"), card);
    m_curCh->setProperty("cls", QStringLiteral("seg"));
    m_curCh->setFocusPolicy(Qt::NoFocus);
    m_curTime->setToolTip(tr("A/B time cursors, drag them on the plot"));
    m_curLevel->setToolTip(tr("A/B level cursors, drag them on the plot"));
    m_curCh->setToolTip(tr("Channel the level cursors read"));
    for (QPushButton *b : {m_curTime, m_curLevel, m_curCh})
        h->addWidget(b);
    v->addLayout(h);

    m_curReadout = new QLabel(QStringLiteral("--"), card);
    m_curReadout->setProperty("cls", QStringLiteral("sumSm"));
    m_curReadout->setTextFormat(Qt::RichText);
    m_curReadout->setWordWrap(true);
    m_curReadout->setMinimumHeight(2 * m_curReadout->fontMetrics().lineSpacing() + 4);
    m_curReadout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    v->addWidget(m_curReadout);

    auto emitMode = [this] {
        emit cursorModeChanged((m_curTime->isChecked() ? 1 : 0)
                               | (m_curLevel->isChecked() ? 2 : 0));
    };
    connect(m_curTime, &QPushButton::toggled, this, emitMode);
    connect(m_curLevel, &QPushButton::toggled, this, emitMode);
    connect(m_curCh, &QPushButton::clicked, this, [this] {
        const int ch = m_curCh->text() == QLatin1String("CH1") ? 1 : 0;
        m_curCh->setText(QStringLiteral("CH%1").arg(ch + 1));
        emit cursorChannelChanged(ch);
    });
    return card;
}

void ControlDock::setCursorReadout(int mode, double tA, double tB, double vA, double vB, int ch)
{
    QStringList parts, lparts;
    const QString tag = QStringLiteral("<span style=\"color:#64748b\">%1</span>&nbsp;");
    if (mode & 1) {
        const double dt = tB - tA;
        parts << tag.arg(QStringLiteral("dt")) + fmt::seconds(dt);
        if (std::abs(dt) > 0.0) {
            const double f = 1.0 / std::abs(dt);
            parts << tag.arg(QStringLiteral("1/dt"))
                         + (f >= 1e6   ? QStringLiteral("%1 MHz").arg(f / 1e6, 0, 'g', 4)
                            : f >= 1e3 ? QStringLiteral("%1 kHz").arg(f / 1e3, 0, 'g', 4)
                                       : QStringLiteral("%1 Hz").arg(f, 0, 'g', 4));
        }
    }
    if (mode & 2) {
        lparts << tag.arg(QStringLiteral("A%1").arg(ch + 1)) + fmt::volts(vA);
        lparts << tag.arg(QStringLiteral("B%1").arg(ch + 1)) + fmt::volts(vB);
        lparts << tag.arg(QStringLiteral("dV")) + fmt::volts(vB - vA);
    }
    const QString sep = QStringLiteral("&nbsp;&nbsp;");
    QStringList lines;
    if (!parts.isEmpty())
        lines << parts.join(sep);
    if (!lparts.isEmpty())
        lines << lparts.join(sep);
    const QString text = lines.isEmpty() ? QStringLiteral("--")
                                         : lines.join(QStringLiteral("<br>"));
    m_curReadout->setText(text);
    if (m_sumCur)
        m_sumCur->setText(text);
}

void ControlDock::refreshTrig()
{
    const TriggerState &t = m_state.trig;
    TrigCard &k = m_trig;
    const bool sourKnown = t.known & (1 << DeviceState::TSour);
    for (int i = 0; i < 2; ++i) {
        setSegChecked(k.src[i], sourKnown && t.source == i + 1);
        // The meter refuses (-221) a source that is not displayed.
        const ChannelState &cs = m_state.ch[i];
        const bool on = (cs.known & (1 << DeviceState::Disp)) && cs.disp;
        k.src[i]->setEnabled(m_online && on);
    }
    const bool slopKnown = t.known & (1 << DeviceState::TSlop);
    setSegChecked(k.rise, slopKnown && t.rising);
    setSegChecked(k.fall, slopKnown && !t.rising);
    if ((t.known & (1 << DeviceState::TLev)) && std::isfinite(t.level))
        k.level->setText((t.level >= 0.0 ? QStringLiteral("+") : QString())
                         + fmt::volts(t.level));
    else
        k.level->setText(QStringLiteral("--"));
    const bool sweKnown = t.known & (1 << DeviceState::TSwe);
    for (int i = 0; i < 3; ++i)
        setSegChecked(k.swe[i], sweKnown && t.sweep == i);
    const bool nrKnown = t.known & (1 << DeviceState::TNrej);
    setSegChecked(k.nrejOff, nrKnown && !t.nrej);
    setSegChecked(k.nrejOn, nrKnown && t.nrej);

    const QString dash = QStringLiteral("--");
    if (sourKnown || slopKnown) {
        const QString src = sourKnown ? QStringLiteral("CH%1").arg(t.source) : dash;
        const QString slp = slopKnown ? (t.rising ? QStringLiteral("\u2197") : QStringLiteral("\u2198")) : QString();
        m_sumTrig.main->setText(src + (slp.isEmpty() ? QString() : QLatin1Char(' ') + slp));
    } else {
        m_sumTrig.main->setText(dash);
    }
    m_sumTrig.level->setText(tr("level %1").arg(k.level->text()));
    static const char *const kSweNames[3] = {"Auto", "Normal", "Single"};
    m_sumTrig.sweep->setText(sweKnown && t.sweep >= 0 && t.sweep < 3
                                 ? tr(kSweNames[t.sweep])
                                 : dash);
}

void ControlDock::stepLevel(int dir)
{
    TriggerState &t = m_state.trig;
    const ChannelState &cs = m_state.ch[qBound(1, t.source, 2) - 1];
    if (!(t.known & (1 << DeviceState::TLev)) || !std::isfinite(t.level) || cs.scal <= 0.0)
        return;
    // Optimistic like stepPos: auto-repeat steps from the new value.
    t.level += dir * cs.scal / 25.0;
    refreshTrig();
    localStep();
    emit triggerValue(DeviceState::TLev, t.level);
}

void ControlDock::setOnline(bool online)
{
    m_online = online;
    for (QPushButton *b : findChildren<QPushButton *>()) {
        if (b != m_toggleBtn && b != m_curTime && b != m_curLevel && b != m_curCh)
            b->setEnabled(online);
    }
    if (!online) {
        m_hold.stop();
        m_havePending = false;
        m_state = DeviceState{};
        for (int c = 0; c < 2; ++c)
            refresh(c);
    }
    refreshTrig();
}

void ControlDock::setState(const DeviceState &s)
{
    if (m_lastStep.isValid()) {
        const qint64 elapsed = m_lastStep.elapsed();
        if (elapsed < kHoldMs) {
            m_pending = s;
            m_havePending = true;
            m_hold.start(qMax(1, int(kHoldMs - elapsed)));
            return;
        }
    }
    applyState(s);
}

void ControlDock::localStep()
{
    m_lastStep.restart();
}

void ControlDock::applyState(const DeviceState &s)
{
    m_state = s;
    refresh(0);
    refresh(1);
    refreshTrig();
}

void ControlDock::refresh(int c)
{
    const ChannelState &cs = m_state.ch[c];
    Card &k = m_card[c];
    const QString dash = QStringLiteral("--");
    k.on->setChecked(cs.disp);
    k.on->setText(cs.disp ? tr("ON") : tr("OFF"));
    repolish(k.on);
    k.probeLbl->setText(cs.known & (1 << DeviceState::Prob)
                            ? QStringLiteral("%1x").arg(cs.prob, 0, 'g', 4)
                            : dash);
    k.scale->setText(cs.known & (1 << DeviceState::Scal) ? fmt::volts(cs.scal) + tr("/div")
                                                         : dash);
    if ((cs.known & (1 << DeviceState::Offs)) && cs.scal > 0.0) {
        const double d = cs.offs / cs.scal;
        k.pos->setText(QStringLiteral("%1%2 div").arg(d >= 0 ? "+" : "").arg(d, 0, 'f', 2));
        k.pos->setToolTip(fmt::volts(cs.offs));
    } else {
        k.pos->setText(dash);
    }
    const bool coupKnown = cs.known & (1 << DeviceState::Coup);
    setSegChecked(k.ac, coupKnown && cs.ac);
    setSegChecked(k.dc, coupKnown && !cs.ac);
    for (int i = 0; i < 3; ++i)
        setSegChecked(k.prob[i], (cs.known & (1 << DeviceState::Prob))
                                     && std::abs(cs.prob - kProbes[i]) < 1e-9);

    const bool dispKnown = cs.known & (1 << DeviceState::Disp);
    m_sumCh[c].scale->setText(dispKnown && !cs.disp ? tr("OFF") : k.scale->text());
    const QString coupStr = coupKnown ? (cs.ac ? QStringLiteral("AC") : QStringLiteral("DC")) : dash;
    m_sumCh[c].meta->setText(QStringLiteral("%1 \u00B7 %2").arg(coupStr, k.probeLbl->text()));
    m_sumCh[c].pos->setText(tr("pos %1").arg(k.pos->text()));
}

void ControlDock::stepScale(int c, int dir)
{
    ChannelState &cs = m_state.ch[c];
    if (!(cs.known & (1 << DeviceState::Scal)) || cs.scal <= 0.0 || cs.prob <= 0.0)
        return;
    const double mv = cs.scal / cs.prob * 1000.0;
    int idx = 0;
    double best = 1e300;
    for (int i = 0; i < kMvCount; ++i) {
        const double d = std::abs(std::log(kMv[i] / mv));
        if (d < best) {
            best = d;
            idx = i;
        }
    }
    idx = qBound(0, idx + dir, kMvCount - 1);
    const double v = kMv[idx] / 1000.0 * cs.prob;
    if (std::abs(v - cs.scal) < 1e-12)
        return;
    // Optimistic: auto-repeat steps from the new value; the read-back
    // corrects it if the meter refused. The offset in volts is not
    // touched here: the read-back cycle picks up what the meter did.
    cs.scal = v;
    refresh(c);
    localStep();
    emit channelValue(c + 1, DeviceState::Scal, v);
}

void ControlDock::stepPos(int c, double divs)
{
    ChannelState &cs = m_state.ch[c];
    if (!(cs.known & (1 << DeviceState::Scal)) || cs.scal <= 0.0)
        return;
    double d = std::round((cs.offs / cs.scal + divs) * 20.0) / 20.0;
    d = qBound(-4.0, d, 4.0);
    cs.offs = d * cs.scal;
    refresh(c);
    localStep();
    emit channelValue(c + 1, DeviceState::Offs, cs.offs);
}

// ------------------------------------------------------------------ HorizBar

HorizBar::HorizBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("horiz"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(10);
    auto *l1 = new QLabel(tr("HORIZONTAL"), this);
    l1->setProperty("cls", QStringLiteral("barLabel"));
    h->addWidget(l1);
    m_btn[0] = arrow(kLeft, tr("Slower timebase (zoom out horizontally)"), this);
    m_tb = value(this);
    m_tb->setMinimumWidth(110);
    m_btn[1] = arrow(kRight, tr("Faster timebase (zoom in horizontally)"), this);
    connect(m_btn[0], &QPushButton::clicked, this, [this] { emit timebaseStep(+1); });
    connect(m_btn[1], &QPushButton::clicked, this, [this] { emit timebaseStep(-1); });
    h->addWidget(m_btn[0]);
    h->addWidget(m_tb);
    h->addWidget(m_btn[1]);
    auto *l2 = new QLabel(tr("POSITION"), this);
    l2->setProperty("cls", QStringLiteral("barLabel"));
    h->addSpacing(10);
    h->addWidget(l2);
    m_pos = value(this);
    m_pos->setMinimumWidth(96);
    h->addWidget(m_pos);

    // Roll: the meter's ACQ:ROLL. Off/On segments like Noise rej.
    auto *l3 = new QLabel(tr("ROLL"), this);
    l3->setProperty("cls", QStringLiteral("barLabel"));
    h->addSpacing(10);
    h->addWidget(l3);
    auto *g = new QButtonGroup(this);
    auto *rr = new QHBoxLayout;
    rr->setSpacing(0);
    m_rollOff = seg(tr("Off"), this);
    m_rollOn = seg(tr("On"), this);
    g->addButton(m_rollOff);
    g->addButton(m_rollOn);
    rr->addWidget(m_rollOff);
    rr->addWidget(m_rollOn);
    h->addLayout(rr);
    const QString rollTip =
        tr("Roll mode: at 200 ms/div and slower, in Auto sweep, the trace scrolls in "
           "from the right as it is captured, like a chart recorder");
    m_rollOff->setToolTip(rollTip);
    m_rollOn->setToolTip(rollTip);
    connect(m_rollOff, &QPushButton::clicked, this, [this] { emit rollChanged(false); });
    connect(m_rollOn, &QPushButton::clicked, this, [this] { emit rollChanged(true); });

    h->addStretch(1);
    setOnline(false);
}

void HorizBar::setOnline(bool online)
{
    for (QPushButton *b : m_btn)
        b->setEnabled(online);
    m_rollOff->setEnabled(online);
    m_rollOn->setEnabled(online);
    if (!online) {
        m_tb->setText(QStringLiteral("--"));
        m_pos->setText(QStringLiteral("--"));
        setRoll(false, false);
    }
}

void HorizBar::setRoll(bool known, bool on)
{
    setSegChecked(m_rollOff, known && !on);
    setSegChecked(m_rollOn, known && on);
}

void HorizBar::setHoriz(double scal, double pos)
{
    if (scal <= 0.0)
        return;
    m_tb->setText(fmt::seconds(scal) + tr("/div"));
    // TIM:OFFS from TRIG:POS (same knob, meter probe).
    const double offs = (0.5 - pos / 100.0) * 10.0 * scal;
    const QString s = fmt::seconds(offs);
    m_pos->setText(offs > 0.0 ? QStringLiteral("+") + s : s);
}
